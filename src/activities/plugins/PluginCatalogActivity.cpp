#include "PluginCatalogActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <XmlParserUtils.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/PluginLocations.h"
#include "util/QrUtils.h"

namespace {
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
constexpr size_t MAX_TOKEN_FILE_SIZE = 2 * 1024;
// In-DRAM responses (auth, download-url hops) are small; the cap bounds a
// misbehaving server, not normal use.
constexpr size_t MAX_API_RESPONSE = 48 * 1024;
// Browse responses stream to this SD temp file instead of DRAM: one page of
// raw catalog JSON can run 60+ KB (BookFusion inlines heavy per-book
// metadata), and buffering that in a std::string aborts on low heap.
constexpr char BROWSE_TMP_PATH[] = "/.pcat_tmp.json";
constexpr size_t MAX_BROWSE_RESPONSE = 1024 * 1024;
constexpr int MAX_PAGE_SIZE = 16;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

void splitPath(const std::string& dotted, std::vector<std::string>& out) {
  out.clear();
  size_t start = 0;
  while (start <= dotted.size()) {
    const size_t dot = dotted.find('.', start);
    if (dot == std::string::npos) {
      if (start < dotted.size()) out.push_back(dotted.substr(start));
      break;
    }
    if (dot > start) out.push_back(dotted.substr(start, dot - start));
    start = dot + 1;
  }
}

bool segIsIndex(const std::string& seg) { return !seg.empty() && isdigit(static_cast<unsigned char>(seg[0])); }

// Resolves a dotted path ("authors.0.name") against a parsed document.
JsonVariantConst resolvePath(JsonVariantConst node, const std::string& dotted) {
  if (dotted.empty()) return node;
  std::vector<std::string> segs;
  splitPath(dotted, segs);
  for (const auto& seg : segs) {
    if (node.isNull()) break;
    if (segIsIndex(seg)) {
      node = node[atoi(seg.c_str())];
    } else {
      node = node[seg.c_str()];
    }
  }
  return node;
}

std::string variantToString(JsonVariantConst v) {
  if (v.isNull()) return "";
  if (v.is<const char*>()) return v.as<const char*>();
  char buf[24];
  if (v.is<long long>() || v.is<int>()) {
    snprintf(buf, sizeof(buf), "%lld", v.as<long long>());
    return buf;
  }
  return "";
}

// Builds an ArduinoJson deserialization filter keeping only one dotted path.
// A numeric segment becomes filter index [0], which ArduinoJson applies to
// every array element. Filtering keeps the parsed document to a few KB where
// the unfiltered catalog response would cost several times the body size.
void addFilterPath(JsonDocument& filter, const std::vector<std::string>& segs) {
  JsonVariant node = filter.as<JsonVariant>();
  for (size_t i = 0; i < segs.size(); i++) {
    const bool last = i + 1 == segs.size();
    if (segIsIndex(segs[i])) {
      JsonArray arr = node.is<JsonArray>() ? node.as<JsonArray>() : node.to<JsonArray>();
      if (last) {
        if (arr.size() == 0) arr.add(true);
        return;
      }
      const bool nextIndex = segIsIndex(segs[i + 1]);
      if (arr.size() == 0) {
        if (nextIndex)
          arr.add<JsonArray>();
        else
          arr.add<JsonObject>();
      }
      node = arr[0];
    } else {
      JsonObject obj = node.is<JsonObject>() ? node.as<JsonObject>() : node.to<JsonObject>();
      if (last) {
        obj[segs[i]] = true;
        return;
      }
      const bool nextIndex = segIsIndex(segs[i + 1]);
      JsonVariant child = obj[segs[i]];
      if (child.isNull()) {
        if (nextIndex)
          child = obj[segs[i]].to<JsonArray>();
        else
          child = obj[segs[i]].to<JsonObject>();
      }
      node = child;
    }
  }
}

void addFieldFilter(JsonDocument& filter, const std::string& itemsPath, const std::string& fieldPath) {
  if (fieldPath.empty()) return;
  std::vector<std::string> segs;
  splitPath(itemsPath, segs);
  segs.push_back("0");  // the item array: index filter applies to all elements
  std::vector<std::string> fieldSegs;
  splitPath(fieldPath, fieldSegs);
  segs.insert(segs.end(), fieldSegs.begin(), fieldSegs.end());
  addFilterPath(filter, segs);
}

void substituteAll(std::string& s, const char* key, const std::string& value) {
  const size_t keyLen = strlen(key);
  size_t pos = 0;
  while ((pos = s.find(key, pos)) != std::string::npos) {
    s.replace(pos, keyLen, value);
    pos += value.size();
  }
}

std::string sanitizedFilename(const std::string& title) {
  std::string out;
  out.reserve(title.size());
  for (const char c : title) {
    if (static_cast<unsigned char>(c) < 0x20 || strchr("/\\:*?\"<>|", c)) {
      out += ' ';
    } else {
      out += c;
    }
  }
  // collapse runs of spaces and trim
  std::string clean;
  clean.reserve(out.size());
  for (const char c : out) {
    if (c == ' ' && (clean.empty() || clean.back() == ' ')) continue;
    clean += c;
  }
  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
  if (clean.size() > 60) clean.resize(60);
  return clean.empty() ? "book" : clean;
}

std::string md5Hex(const std::string& text) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  md5.calculate();
  return md5.toString().c_str();
}

void readHeaders(JsonVariantConst node, std::vector<std::pair<std::string, std::string>>& out) {
  out.clear();
  for (JsonPairConst kv : node.as<JsonObjectConst>()) {
    out.emplace_back(kv.key().c_str(), kv.value().as<const char*>() ? kv.value().as<const char*>() : "");
  }
}

// --- XML-list helpers -----------------------------------------------------
std::string urlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      auto hex = [](char c) { return c <= '9' ? c - '0' : (tolower(c) - 'a' + 10); };
      out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
      i += 2;
    } else if (s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string urlEncodePath(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (const unsigned char c : s) {
    if (isalnum(c) || strchr("-_.~/", c)) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// scheme://host[:port] of a URL, for turning server-absolute hrefs into full URLs.
std::string originOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t hostEnd = url.find('/', schemeEnd + 3);
  return hostEnd == std::string::npos ? url : url.substr(0, hostEnd);
}

std::string pathOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t hostEnd = url.find('/', schemeEnd + 3);
  return hostEnd == std::string::npos ? "/" : url.substr(hostEnd);
}

std::string basename(const std::string& path) {
  std::string p = path;
  while (!p.empty() && p.back() == '/') p.pop_back();
  const size_t slash = p.rfind('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

// True when `path` ends in one of the allowed extensions (case-insensitive).
// An empty list allows everything.
bool hasAllowedExtension(const std::string& path, const std::vector<std::string>& exts) {
  if (exts.empty()) return true;
  std::string lower = path;
  for (auto& c : lower) c = tolower(static_cast<unsigned char>(c));
  while (!lower.empty() && lower.back() == '/') lower.pop_back();
  for (const auto& ext : exts) {
    if (lower.size() >= ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) return true;
  }
  return false;
}

// Extracts one row per repeating item element from an XML list via expat (the
// parser the OPDS browser already uses), instead of scanning tags by hand.
// Elements match on local name (namespace prefix stripped). Selector forms:
// "elem" (leading text of the first matching descendant), "elem@attr"
// (attribute of the first matching descendant), "@attr" (attribute on the
// item element itself).
class XmlListParser {
 public:
  enum Field { F_URL, F_TITLE, F_AUTHOR, F_ID, F_COUNT };
  struct RawItem {
    std::string field[F_COUNT];
    bool isDir = false;
  };

  XmlListParser(const std::string& itemName, const std::string& containerName, const std::string (&selectors)[F_COUNT])
      : item(itemName), container(containerName) {
    for (int i = 0; i < F_COUNT; i++) splitSelector(selectors[i], sel[i]);
  }

  // Parses the whole document from an SD file in small chunks, so the raw XML
  // (a large WebDAV multistatus, ...) never occupies DRAM. Rows collected
  // before a parse error are kept, mirroring the previous scanner, which
  // stopped at the first bad tag.
  std::vector<RawItem> parseFile(HalFile& f) {
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p) return std::move(rows);
    XML_SetUserData(p, this);
    XML_SetElementHandler(
        p,
        [](void* self, const XML_Char* name, const XML_Char** atts) {
          static_cast<XmlListParser*>(self)->onStart(name, atts);
        },
        [](void* self, const XML_Char* name) { static_cast<XmlListParser*>(self)->onEnd(name); });
    XML_SetCharacterDataHandler(
        p, [](void* self, const XML_Char* s, int len) { static_cast<XmlListParser*>(self)->onText(s, len); });
    std::vector<char> buf(2048);
    for (;;) {
      const int n = f.read(buf.data(), buf.size());
      const bool last = n <= 0;
      if (XML_Parse(p, buf.data(), last ? 0 : n, last ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
        LOG_ERR("PCAT", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(p),
                XML_ErrorString(XML_GetErrorCode(p)));
        break;
      }
      if (last) break;
    }
    destroyXmlParser(p);
    return std::move(rows);
  }

 private:
  static constexpr size_t MAX_ITEMS = 200;
  static constexpr size_t MAX_FIELD_CHARS = 768;

  struct Selector {
    std::string elem, attr;
    bool onItemTag = false;  // "@attr": read from the item element's own tag
    bool isSet() const { return !elem.empty() || !attr.empty(); }
  };

  static void splitSelector(const std::string& s, Selector& out) {
    if (s.empty()) return;
    if (s[0] == '@') {
      out.onItemTag = true;
      out.attr = s.substr(1);
      return;
    }
    const size_t at = s.find('@');
    out.elem = at == std::string::npos ? s : s.substr(0, at);
    if (at != std::string::npos) out.attr = s.substr(at + 1);
  }

  static const char* localName(const XML_Char* name) {
    const char* colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
  }

  static const char* findAttr(const XML_Char** atts, const std::string& attr) {
    for (int i = 0; atts[i]; i += 2) {
      if (attr == atts[i]) return atts[i + 1];
    }
    return nullptr;
  }

  void onStart(const XML_Char* name, const XML_Char** atts) {
    depth++;
    const char* local = localName(name);
    if (itemDepth < 0) {
      if (rows.size() < MAX_ITEMS && item == local) {
        itemDepth = depth;
        current = RawItem{};
        capturingMask = 0;
        for (int i = 0; i < F_COUNT; i++) {
          done[i] = !sel[i].isSet();
          if (sel[i].onItemTag) {
            const char* v = findAttr(atts, sel[i].attr);
            if (v) current.field[i] = v;
            done[i] = true;
          }
        }
      }
      return;
    }
    // Inside an item: a child element ends any leading-text capture.
    capturingMask = 0;
    if (!container.empty() && container == local) current.isDir = true;
    for (int i = 0; i < F_COUNT; i++) {
      if (done[i] || sel[i].onItemTag || sel[i].elem != local) continue;
      done[i] = true;  // first matching descendant wins
      if (!sel[i].attr.empty()) {
        const char* v = findAttr(atts, sel[i].attr);
        if (v) current.field[i] = v;
      } else {
        capturingMask |= 1u << i;
      }
    }
  }

  void onEnd(const XML_Char* name) {
    if (itemDepth >= 0) {
      capturingMask = 0;
      if (depth == itemDepth && item == localName(name)) {
        for (auto& f : current.field) trim(f);
        rows.push_back(std::move(current));
        itemDepth = -1;
      }
    }
    depth--;
  }

  void onText(const XML_Char* s, const int len) {
    if (itemDepth < 0 || capturingMask == 0) return;
    for (int i = 0; i < F_COUNT; i++) {
      if (!(capturingMask & (1u << i)) || current.field[i].size() >= MAX_FIELD_CHARS) continue;
      current.field[i].append(s, std::min<size_t>(len, MAX_FIELD_CHARS - current.field[i].size()));
    }
  }

  static void trim(std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
      s.clear();
      return;
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
  }

  std::string item;
  std::string container;
  Selector sel[F_COUNT];
  std::vector<RawItem> rows;
  RawItem current;
  bool done[F_COUNT] = {};
  uint8_t capturingMask = 0;
  int depth = 0;
  int itemDepth = -1;
};
}  // namespace

namespace {
// Reads "title"/"description" from a plugin JSON file into the ref, only
// overwriting non-empty values (so device.json wins over manifest.json).
void readTitleDesc(const std::string& path, PluginRef& ref) {
  std::string raw;
  if (!Storage.readFileToString("PCAT", path, MAX_MANIFEST_SIZE, raw)) return;
  JsonDocument filter;
  filter["title"] = true;
  filter["description"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return;
  if (doc["title"].is<const char*>()) ref.title = doc["title"].as<const char*>();
  if (doc["description"].is<const char*>()) ref.description = doc["description"].as<const char*>();
}
}  // namespace

std::vector<PluginRef> discoverPlugins() {
  const auto entries = PluginLocations::scanPlugins();
  std::vector<PluginRef> plugins;
  plugins.reserve(entries.size());
  for (const auto& e : entries) {
    const std::string devicePath = e.dir + "/device.json";
    const std::string readmePath = e.dir + "/README.md";

    PluginRef ref;
    ref.name = e.name;
    ref.title = e.name;
    ref.hasCatalog = e.hasDevice;
    if (e.hasDevice) ref.manifestPath = devicePath;
    if (Storage.exists(readmePath.c_str())) ref.readmePath = readmePath;
    // manifest.json first, then device.json overrides (on-device authority).
    if (e.hasManifest) readTitleDesc(e.dir + "/manifest.json", ref);
    if (e.hasDevice) readTitleDesc(devicePath, ref);
    plugins.push_back(std::move(ref));
  }
  return plugins;
}

bool anyPluginInstalled() {
  // manifest.json alone means web-card metadata only: listed for its README,
  // but not enough to claim the home screen's library slot.
  for (const auto& e : PluginLocations::scanPlugins()) {
    if (e.hasPluginJs || e.hasDevice) return true;
  }
  return false;
}

bool PluginCatalogActivity::loadManifest() {
  std::string raw;
  if (!Storage.readFileToString("PCAT", manifestPath, MAX_MANIFEST_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;

  manifest.tokenFile = doc["token"]["file"] | "";
  manifest.tokenPath = doc["token"]["path"] | "token";
  manifest.configFile = doc["config"]["file"] | "";

  JsonVariantConst browse = doc["browse"];
  manifest.browseFormat = browse["format"] | "json";
  manifest.browseUrl = browse["url"] | "";
  manifest.browseMethod = browse["method"] | "GET";
  manifest.browseBody = browse["body"] | "";
  readHeaders(browse["headers"], manifest.browseHeaders);
  manifest.itemsPath = browse["items"] | "";
  manifest.titlePath = browse["fields"]["title"] | (manifest.isXmlList() ? "" : "title");
  manifest.authorPath = browse["fields"]["author"] | "";
  manifest.idPath = browse["fields"]["id"] | "";
  manifest.urlPath = browse["fields"]["url"] | "";
  manifest.pageSize = browse["page_size"] | 8;
  if (manifest.pageSize < 1) manifest.pageSize = 1;
  if (manifest.pageSize > MAX_PAGE_SIZE) manifest.pageSize = MAX_PAGE_SIZE;
  for (JsonVariantConst l : browse["lists"].as<JsonArrayConst>()) {
    Manifest::BrowseList entry;
    entry.title = l["title"] | "";
    entry.url = l["url"] | "";
    entry.body = l["body"] | "";
    if (!entry.title.empty()) manifest.browseLists.push_back(std::move(entry));
  }
  manifest.xmlItem = browse["item"] | "";
  manifest.xmlContainer = browse["container_element"] | "";
  manifest.xmlSkipSelf = browse["skip_self"] | false;
  manifest.xmlResolveUrls = browse["resolve_urls"] | false;
  for (JsonVariantConst ext : browse["extensions"].as<JsonArrayConst>()) {
    if (ext.is<const char*>()) manifest.xmlExtensions.emplace_back(ext.as<const char*>());
  }

  JsonVariantConst dl = doc["download"];
  manifest.dlUrl = dl["url"] | "";
  manifest.dlMethod = dl["method"] | "GET";
  manifest.dlBody = dl["body"] | "";
  readHeaders(dl["headers"], manifest.dlHeaders);
  manifest.dlUrlPath = dl["url_path"] | "";
  manifest.dlUser = dl["username"] | "";
  manifest.dlPass = dl["password"] | "";
  manifest.destDir = dl["dest_dir"] | "";
  manifest.filenameTpl = dl["filename"] | "{title}.epub";
  // Multi-file bundle install (generic): base URL + a files array per item.
  manifest.bundleBasePath = dl["bundle"]["base"] | "";
  manifest.bundleFilesPath = dl["bundle"]["files"] | "";
  manifest.bundleSubdir = dl["bundle"]["subdir"] | "{id}";
  // XML-list items already carry the file URL; default the template to it.
  if (manifest.isXmlList() && manifest.dlUrl.empty()) manifest.dlUrl = "{url}";
  manifest.sidecarPath = dl["sidecar"]["path"] | "";
  manifest.sidecarBody = dl["sidecar"]["body"] | "";

  JsonVariantConst auth = doc["auth"];
  manifest.authType = auth["type"] | "device_code";
  manifest.authUrl = auth["request"]["url"] | "";
  manifest.authMethod = auth["request"]["method"] | "POST";
  manifest.authBody = auth["request"]["body"] | "";
  readHeaders(auth["request"]["headers"], manifest.authHeaders);
  manifest.pollUrl = auth["poll"]["url"] | "";
  manifest.pollMethod = auth["poll"]["method"] | "POST";
  manifest.pollBody = auth["poll"]["body"] | "";
  readHeaders(auth["poll"]["headers"], manifest.pollHeaders);
  manifest.authCodePath = auth["code_path"] | "user_code";
  manifest.authVerifyPath = auth["verify_url_path"] | "verification_uri";
  manifest.authDeviceCodePath = auth["device_code_path"] | "device_code";
  manifest.authIntervalPath = auth["interval_path"] | "interval";
  manifest.authExpiresPath = auth["expires_path"] | "expires_in";
  manifest.authTokenPath = auth["token_path"] | "access_token";
  manifest.authErrorPath = auth["error_path"] | "error";

  return !manifest.browseUrl.empty();
}

bool PluginCatalogActivity::saveToken(const std::string& value) {
  if (manifest.tokenFile.empty()) return false;
  JsonDocument doc;
  // Build the nesting the read path expects (numeric segments unsupported).
  std::vector<std::string> segs;
  splitPath(manifest.tokenPath, segs);
  if (segs.empty()) return false;
  JsonVariant node = doc.to<JsonObject>();
  for (size_t i = 0; i + 1 < segs.size(); i++) node = node[segs[i]].to<JsonObject>();
  node[segs.back()] = value;
  std::string out;
  serializeJson(doc, out);
  HalFile file;
  if (!Storage.openFileForWrite("PCAT", manifest.tokenFile, file)) return false;
  file.write(out.data(), out.size());
  file.flush();
  return true;
}

bool PluginCatalogActivity::loadToken() {
  token.clear();
  if (manifest.tokenFile.empty()) return true;  // token-less catalog
  std::string raw;
  if (!Storage.readFileToString("PCAT", manifest.tokenFile, MAX_TOKEN_FILE_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  token = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.tokenPath));
  return !token.empty();
}

void PluginCatalogActivity::loadConfig() {
  config.clear();
  if (manifest.configFile.empty()) return;
  std::string raw;
  if (!Storage.readFileToString("PCAT", manifest.configFile, MAX_TOKEN_FILE_SIZE, raw)) return;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    config.emplace_back(kv.key().c_str(), variantToString(kv.value()));
  }
}

std::string PluginCatalogActivity::substituted(std::string tpl, const Item* item) const {
  substituteAll(tpl, "{token}", token);
  for (const auto& kv : config) substituteAll(tpl, ("{cfg." + kv.first + "}").c_str(), kv.second);
  char num[16];
  snprintf(num, sizeof(num), "%d", page);
  substituteAll(tpl, "{page}", num);
  snprintf(num, sizeof(num), "%d", manifest.pageSize + 1);
  substituteAll(tpl, "{limit}", num);
  if (item) {
    substituteAll(tpl, "{id}", item->id);
    substituteAll(tpl, "{title}", item->title);
    substituteAll(tpl, "{author}", item->author);
    substituteAll(tpl, "{url}", item->url);
  }
  return tpl;
}

PluginCatalogActivity::PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string manifestPath, std::string title)
    : Activity("PluginCatalog", renderer, mappedInput),
      manifestPath(std::move(manifestPath)),
      catalogTitle(std::move(title)) {}

PluginCatalogActivity::~PluginCatalogActivity() = default;

int PluginCatalogActivity::apiRequest(const std::string& url, const std::string& method, const std::string& body,
                                      const std::vector<std::pair<std::string, std::string>>& headers,
                                      std::string& out) {
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient& http = session ? *session : tmp;
  http.setUserAgent("CrossPoint");
  // Same trust posture as every other SecureNet consumer: no CA bundle ships
  // with the transport, so verification is skipped; traffic stays encrypted.
  http.setInsecure();
  http.setTimeout(30000);
  if (!http.begin(url)) return -1;
  for (const auto& h : headers) http.addHeader(h.first, h.second);

  out.clear();
  bool overflow = false;
  const int status = http.sendRequest(method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                      [&](const uint8_t* data, size_t len) {
                                        if (out.size() + len > MAX_API_RESPONSE) {
                                          overflow = true;
                                          return false;
                                        }
                                        // Grow the buffer only as needed, and only when a nothrow probe proves
                                        // the larger block is available. A bare string reallocation would abort
                                        // the device on low heap when a response is large (e.g. an API that
                                        // inlines article HTML); here we just stop and fail the request.
                                        if (out.size() + len > out.capacity()) {
                                          size_t want = out.capacity() ? out.capacity() * 2 : 2048;
                                          if (want < out.size() + len) want = out.size() + len;
                                          if (want > MAX_API_RESPONSE) want = MAX_API_RESPONSE;
                                          void* probe = malloc(want + 256);
                                          if (!probe) {
                                            overflow = true;
                                            return false;
                                          }
                                          free(probe);
                                          out.reserve(want);
                                        }
                                        out.append(reinterpret_cast<const char*>(data), len);
                                        return true;
                                      });
  // Error statuses still return their body: OAuth device-code polling carries
  // its state ("authorization_pending") in 4xx response bodies.
  if (overflow || status < 0 || !http.responseComplete()) {
    LOG_ERR("PCAT", "API request failed: status=%d overflow=%d complete=%d %s", status, overflow,
            http.responseComplete(), url.c_str());
    return -1;
  }
  return status;
}

int PluginCatalogActivity::apiRequestToFile(const std::string& url, const std::string& method, const std::string& body,
                                            const std::vector<std::pair<std::string, std::string>>& headers,
                                            const char* destPath) {
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient& http = session ? *session : tmp;
  http.setUserAgent("CrossPoint");
  http.setInsecure();
  http.setTimeout(30000);
  if (!http.begin(url)) return -1;
  for (const auto& h : headers) http.addHeader(h.first, h.second);

  int status = -1;
  bool writeOk = true;
  size_t written = 0;
  {
    HalFile file;
    if (!Storage.openFileForWrite("PCAT", destPath, file)) return -1;
    status = http.sendRequest(method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                              [&](const uint8_t* data, size_t len) {
                                if (written + len > MAX_BROWSE_RESPONSE) {
                                  writeOk = false;
                                  return false;
                                }
                                if (file.write(data, len) != len) {
                                  writeOk = false;
                                  return false;
                                }
                                written += len;
                                return true;
                              });
    file.flush();
    // file closed at scope exit, before any Storage.remove of destPath
  }
  if (!writeOk || status < 0 || !http.responseComplete()) {
    LOG_ERR("PCAT", "API request (to file) failed: status=%d writeOk=%d complete=%d %s", status, writeOk,
            http.responseComplete(), url.c_str());
    Storage.remove(destPath);
    return -1;
  }
  return status;
}

void PluginCatalogActivity::onEnter() {
  Activity::onEnter();
  state = State::CHECK_WIFI;
  items.clear();
  page = 1;
  hasMore = false;
  currentList = -1;
  selectorIndex = 0;
  consumeConfirm = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  session.reset(new (std::nothrow) freeink::SecureHttpClient());
  if (session) session->setReuse(true);

  if (!loadManifest()) {
    state = State::ERROR;
    errorMessage = tr(STR_PLUGIN_MANIFEST_INVALID);
    requestUpdate();
    return;
  }
  requestUpdate();
  checkAndConnectWifi();
}

void PluginCatalogActivity::onExit() {
  Activity::onExit();
  items.clear();
  session.reset();  // drop the reused TLS session before Wi-Fi teardown
  Storage.remove(BROWSE_TMP_PATH);
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void PluginCatalogActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    startBrowse();
    return;
  }
  launchWifiSelection();
}

void PluginCatalogActivity::startBrowse() {
  // Browse lists apply to JSON catalogs; XML lists navigate by folder instead.
  if (!manifest.browseLists.empty() && !manifest.isXmlList() && currentList < 0) {
    items.clear();
    page = 1;
    hasMore = false;
    selectorIndex = 0;
    state = State::LIST_PICKER;
    requestUpdate();
    return;
  }
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchPage(1);
}

void PluginCatalogActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             startBrowse();
                           } else {
                             state = State::ERROR;
                             errorMessage = tr(STR_WIFI_CONN_FAILED);
                             requestUpdate();
                           }
                         });
}

bool PluginCatalogActivity::refreshCredentialToken() {
  loadConfig();
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(manifest.authHeaders.size());
  for (const auto& h : manifest.authHeaders) headers.emplace_back(h.first, substituted(h.second, nullptr));
  std::string response;
  const int status = apiRequest(substituted(manifest.authUrl, nullptr), manifest.authMethod,
                                substituted(manifest.authBody, nullptr), headers, response);
  if (status < 200 || status >= 300) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) return false;
  const std::string minted = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.authTokenPath));
  if (minted.empty() || !saveToken(minted)) return false;
  token = minted;  // usable immediately, without re-reading the file
  return true;
}

int PluginCatalogActivity::browseRequestToFile(const std::string& urlTemplate, const std::string& bodyTemplate,
                                               const char* destPath) {
  auto build = [&](std::string& url, std::string& body, std::vector<std::pair<std::string, std::string>>& headers) {
    url = substituted(urlTemplate, nullptr);
    body = substituted(bodyTemplate, nullptr);
    headers.clear();
    headers.reserve(manifest.browseHeaders.size());
    for (const auto& h : manifest.browseHeaders) headers.emplace_back(h.first, substituted(h.second, nullptr));
  };
  std::string url, body;
  std::vector<std::pair<std::string, std::string>> headers;
  build(url, body, headers);
  int status = apiRequestToFile(url, manifest.browseMethod, body, headers, destPath);
  // A password-grant token expires; on 401/403 mint a fresh one and retry once.
  if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) {
    build(url, body, headers);
    status = apiRequestToFile(url, manifest.browseMethod, body, headers, destPath);
  }
  return status;
}

void PluginCatalogActivity::fetchXmlList() {
  loadConfig();
  if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (manifest.xmlItem.empty()) {
    state = State::ERROR;
    errorMessage = tr(STR_PLUGIN_MANIFEST_INVALID);
    requestUpdate();
    return;
  }
  if (browseCurrentUrl.empty()) browseCurrentUrl = substituted(manifest.browseUrl, nullptr);

  const int status = browseRequestToFile(browseCurrentUrl, manifest.browseBody, BROWSE_TMP_PATH);
  if (status == 401 || status == 403) {
    Storage.remove(BROWSE_TMP_PATH);
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (status < 200 || status >= 300) {  // 207 Multi-Status counts as success
    Storage.remove(BROWSE_TMP_PATH);
    state = State::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

  const std::string origin = originOf(browseCurrentUrl);
  const std::string selfPath = pathOf(browseCurrentUrl);
  auto trimSlash = [](std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
  };
  const std::string decodedSelf = trimSlash(urlDecode(selfPath));

  // Extract one row per repeating item element, then apply the list rules.
  const std::string selectors[XmlListParser::F_COUNT] = {manifest.urlPath, manifest.titlePath, manifest.authorPath,
                                                         manifest.idPath};
  XmlListParser parser(manifest.xmlItem, manifest.xmlContainer, selectors);
  std::vector<XmlListParser::RawItem> rows;
  {
    HalFile file;
    if (Storage.openFileForRead("PCAT", BROWSE_TMP_PATH, file)) rows = parser.parseFile(file);
    // file closed at scope exit, before the remove below
  }
  Storage.remove(BROWSE_TMP_PATH);

  items.clear();
  items.reserve(rows.size());
  for (const auto& row : rows) {
    const std::string& rawUrl = row.field[XmlListParser::F_URL];
    if (rawUrl.empty()) continue;
    Item item;
    item.isDir = row.isDir;
    item.url =
        manifest.xmlResolveUrls && rawUrl.rfind("http", 0) != 0 ? origin + urlEncodePath(urlDecode(rawUrl)) : rawUrl;
    item.author = row.field[XmlListParser::F_AUTHOR];
    item.id = row.field[XmlListParser::F_ID];
    const std::string& title = row.field[XmlListParser::F_TITLE];
    item.title = title.empty() ? basename(urlDecode(rawUrl)) : title;

    if (manifest.xmlSkipSelf && trimSlash(urlDecode(rawUrl)) == decodedSelf) continue;
    if (!item.isDir && !hasAllowedExtension(urlDecode(rawUrl), manifest.xmlExtensions)) continue;
    items.push_back(std::move(item));
  }

  // Folders first, then files, each alphabetical — matches how file managers list.
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
  });
  hasMore = false;
  selectorIndex = 0;
  state = State::BROWSING;
  requestUpdate();
}

bool PluginCatalogActivity::prevRowVisible() const {
  return state == State::BROWSING && !manifest.isXmlList() && page > 1;
}

bool PluginCatalogActivity::nextRowVisible() const {
  return state == State::BROWSING && !manifest.isXmlList() && hasMore;
}

int PluginCatalogActivity::rowCount() const {
  if (state == State::LIST_PICKER) return static_cast<int>(manifest.browseLists.size());
  if (state != State::BROWSING) return 0;
  return static_cast<int>(items.size()) + (prevRowVisible() ? 1 : 0) + (nextRowVisible() ? 1 : 0);
}

const std::string& PluginCatalogActivity::activeBrowseUrl() const {
  if (currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size()) &&
      !manifest.browseLists[currentList].url.empty()) {
    return manifest.browseLists[currentList].url;
  }
  return manifest.browseUrl;
}

const std::string& PluginCatalogActivity::activeBrowseBody() const {
  if (currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size()) &&
      !manifest.browseLists[currentList].body.empty()) {
    return manifest.browseLists[currentList].body;
  }
  return manifest.browseBody;
}

void PluginCatalogActivity::fetchPage(const int newPage) {
  if (manifest.isXmlList()) {
    fetchXmlList();
    return;
  }
  page = newPage;
  loadConfig();
  if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }

  const int status = browseRequestToFile(activeBrowseUrl(), activeBrowseBody(), BROWSE_TMP_PATH);
  if (status == 401 || status == 403) {
    // Stale or revoked token: back to the sign-in screen, not a raw error.
    Storage.remove(BROWSE_TMP_PATH);
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (status < 200 || status >= 300) {
    Storage.remove(BROWSE_TMP_PATH);
    state = State::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

  JsonDocument filter;
  addFieldFilter(filter, manifest.itemsPath, manifest.titlePath);
  addFieldFilter(filter, manifest.itemsPath, manifest.authorPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.idPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.urlPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.bundleBasePath);
  addFieldFilter(filter, manifest.itemsPath, manifest.bundleFilesPath);

  // Filtered parse straight from the SD temp file — the raw response never
  // occupies DRAM, only the few fields the filter admits.
  JsonDocument doc;
  {
    HalFile file;
    if (!Storage.openFileForRead("PCAT", BROWSE_TMP_PATH, file)) {
      state = State::ERROR;
      errorMessage = tr(STR_PARSE_FEED_FAILED);
      requestUpdate();
      return;
    }
    struct HalFileReader {
      HalFile& f;
      int read() { return f.read(); }
      size_t readBytes(char* buf, size_t n) {
        const int r = f.read(buf, n);
        return r < 0 ? 0 : static_cast<size_t>(r);
      }
    } reader{file};
    const auto parseErr = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    // file closed at scope exit, before the remove below
    if (parseErr != DeserializationError::Ok) {
      LOG_ERR("PCAT", "browse JSON parse error: %s", parseErr.c_str());
      Storage.remove(BROWSE_TMP_PATH);
      state = State::ERROR;
      errorMessage = tr(STR_PARSE_FEED_FAILED);
      requestUpdate();
      return;
    }
  }
  Storage.remove(BROWSE_TMP_PATH);

  JsonVariantConst itemsNode = resolvePath(doc.as<JsonVariantConst>(), manifest.itemsPath);
  JsonArrayConst arr = itemsNode.as<JsonArrayConst>();
  items.clear();
  if (!arr.isNull()) {
    items.reserve(manifest.pageSize);
    for (JsonVariantConst v : arr) {
      if (static_cast<int>(items.size()) >= manifest.pageSize + 1) break;
      Item item;
      item.title = variantToString(resolvePath(v, manifest.titlePath));
      item.author = variantToString(resolvePath(v, manifest.authorPath));
      item.id = variantToString(resolvePath(v, manifest.idPath));
      item.url = variantToString(resolvePath(v, manifest.urlPath));
      if (manifest.isBundle()) {
        item.base = variantToString(resolvePath(v, manifest.bundleBasePath));
        JsonArrayConst fileArr = resolvePath(v, manifest.bundleFilesPath).as<JsonArrayConst>();
        if (!fileArr.isNull()) {
          item.files.reserve(fileArr.size());
          for (JsonVariantConst f : fileArr) {
            if (f.is<const char*>()) item.files.emplace_back(f.as<const char*>());
          }
        }
      }
      if (!item.title.empty()) items.push_back(std::move(item));
    }
  }
  hasMore = static_cast<int>(items.size()) > manifest.pageSize;
  if (hasMore) items.resize(manifest.pageSize);
  selectorIndex = 0;
  state = State::BROWSING;
  requestUpdate();
}

void PluginCatalogActivity::beginAuth() {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);

  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(manifest.authHeaders.size());
  for (const auto& h : manifest.authHeaders) headers.emplace_back(h.first, substituted(h.second, nullptr));
  std::string response;
  const int status = apiRequest(substituted(manifest.authUrl, nullptr), manifest.authMethod,
                                substituted(manifest.authBody, nullptr), headers, response);
  JsonDocument doc;
  if (status < 200 || status >= 300 || deserializeJson(doc, response) != DeserializationError::Ok) {
    state = State::ERROR;
    errorMessage = tr(STR_PLUGIN_AUTH_FAILED);
    requestUpdate();
    return;
  }
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  authUserCode = variantToString(resolvePath(root, manifest.authCodePath));
  authVerifyUrl = variantToString(resolvePath(root, manifest.authVerifyPath));
  authDeviceCode = variantToString(resolvePath(root, manifest.authDeviceCodePath));
  const long interval = resolvePath(root, manifest.authIntervalPath) | 5L;
  const long expires = resolvePath(root, manifest.authExpiresPath) | 900L;
  if (authUserCode.empty() || authDeviceCode.empty()) {
    state = State::ERROR;
    errorMessage = tr(STR_PLUGIN_AUTH_FAILED);
    requestUpdate();
    return;
  }
  authIntervalMs = (interval < 3 ? 3 : interval) * 1000UL;
  authDeadlineMs = millis() + (expires < 60 ? 60 : expires) * 1000UL;
  authNextPollMs = millis() + authIntervalMs;
  state = State::AUTH;
  requestUpdate();
}

void PluginCatalogActivity::pollAuth() {
  authNextPollMs = millis() + authIntervalMs;

  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(manifest.pollHeaders.size());
  for (const auto& h : manifest.pollHeaders) headers.emplace_back(h.first, substituted(h.second, nullptr));
  std::string url = substituted(manifest.pollUrl, nullptr);
  std::string body = substituted(manifest.pollBody, nullptr);
  substituteAll(url, "{device_code}", authDeviceCode);
  substituteAll(body, "{device_code}", authDeviceCode);

  std::string response;
  const int status = apiRequest(url, manifest.pollMethod, body, headers, response);
  if (status < 0) return;  // transient transport failure: keep polling

  JsonDocument doc;
  if (deserializeJson(doc, response) == DeserializationError::Ok) {
    const JsonVariantConst root = doc.as<JsonVariantConst>();
    const std::string newToken = variantToString(resolvePath(root, manifest.authTokenPath));
    if (!newToken.empty()) {
      if (!saveToken(newToken)) {
        state = State::ERROR;
        errorMessage = tr(STR_PLUGIN_AUTH_FAILED);
        requestUpdate();
        return;
      }
      startBrowse();
      return;
    }
    const std::string code = variantToString(resolvePath(root, manifest.authErrorPath));
    if (code == "slow_down") {
      authIntervalMs += 5000;
    } else if (code == "expired_token" || code == "access_denied") {
      state = State::ERROR;
      errorMessage = tr(STR_PLUGIN_AUTH_FAILED);
      requestUpdate();
      return;
    }
    // authorization_pending (or anything unrecognized): keep polling
  }

  if (static_cast<long>(millis() - authDeadlineMs) >= 0) {
    state = State::ERROR;
    errorMessage = tr(STR_PLUGIN_AUTH_FAILED);
    requestUpdate();
  }
}

void PluginCatalogActivity::downloadItem(const Item& item) {
  state = State::DOWNLOADING;
  statusMessage = item.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Multi-file bundle install: fetch every file in item.files from item.base
  // into destDir/<subdir>/, creating intermediate folders. Progress advances
  // per file. Generic (a plugin installer, a theme pack, ...).
  if (manifest.isBundle() && !item.files.empty()) {
    const std::string subdir = substituted(manifest.bundleSubdir, &item);
    // Reject path traversal in the subdir (a hostile catalog could escape).
    if (subdir.empty() || subdir.find("..") != std::string::npos || subdir.front() == '/') {
      state = State::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    std::string dir = manifest.destDir;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    dir += '/';
    dir += subdir;
    if (!Storage.exists(dir.c_str()) && !Storage.mkdir(dir.c_str())) {
      LOG_ERR("PCAT", "bundle mkdir failed: %s", dir.c_str());
      state = State::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    std::string base = item.base;
    if (!base.empty() && base.back() != '/') base += '/';
    const size_t total = item.files.size();
    for (size_t i = 0; i < total; i++) {
      std::string rel = item.files[i];
      while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
      if (rel.empty() || rel.find("..") != std::string::npos) continue;  // skip unsafe entries
      downloadProgress = i;
      downloadTotal = total;
      requestUpdate(true);
      const std::string dest = dir + "/" + rel;
      // Create any intermediate folders for nested files ("assets/icon.bin").
      const size_t slash = dest.find_last_of('/');
      if (slash != std::string::npos) {
        const std::string parent = dest.substr(0, slash);
        if (!Storage.exists(parent.c_str())) Storage.mkdir(parent.c_str());
      }
      const auto res = HttpDownloader::downloadToFile(base + rel, dest);
      if (res != HttpDownloader::OK) {
        LOG_ERR("PCAT", "bundle file failed: %s (%d)", rel.c_str(), static_cast<int>(res));
        state = State::ERROR;
        errorMessage = tr(STR_DOWNLOAD_FAILED);
        requestUpdate();
        return;
      }
    }
    state = State::DONE;
    statusMessage = item.title;
    requestUpdate();
    return;
  }

  // Resolve the file URL: either the template itself, or one API hop away.
  std::string fileUrl = substituted(manifest.dlUrl, &item);
  if (!manifest.dlUrlPath.empty()) {
    std::vector<std::pair<std::string, std::string>> headers;
    headers.reserve(manifest.dlHeaders.size());
    for (const auto& h : manifest.dlHeaders) headers.emplace_back(h.first, substituted(h.second, &item));
    std::string response;
    const int status = apiRequest(fileUrl, manifest.dlMethod, substituted(manifest.dlBody, &item), headers, response);
    if (status < 200 || status >= 300) {
      state = State::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
      state = State::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    fileUrl = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.dlUrlPath));
  }
  if (fileUrl.empty()) {
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  const char* folder = manifest.destDir.c_str();
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("PCAT", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  Item named = item;
  named.title = sanitizedFilename(item.title);
  std::string dest;
  dest.reserve(96);
  if (haveFolder) dest += folder;
  dest += '/';
  dest += substituted(manifest.filenameTpl, &named);

  const std::string dlUser = substituted(manifest.dlUser, &item);
  const std::string dlPass = substituted(manifest.dlPass, &item);
  std::vector<std::pair<std::string, std::string>> dlHeaders;
  dlHeaders.reserve(manifest.dlHeaders.size());
  for (const auto& h : manifest.dlHeaders) dlHeaders.emplace_back(h.first, substituted(h.second, &item));
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      fileUrl, dest,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      nullptr, dlUser, dlPass, dlHeaders);

  if (result != HttpDownloader::OK) {
    LOG_ERR("PCAT", "Download failed: %d", static_cast<int>(result));
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }
  clearBookCache(dest);

  // Optional per-book sidecar (e.g. a service book id keyed by the file's
  // path hash) so a later sync stage can associate the file with the service.
  if (!manifest.sidecarPath.empty() && !manifest.sidecarBody.empty()) {
    Item sidecarItem = item;
    const std::string md5 = md5Hex(dest);
    std::string path = substituted(manifest.sidecarPath, &sidecarItem);
    substituteAll(path, "{md5}", md5);
    std::string body = substituted(manifest.sidecarBody, &sidecarItem);
    substituteAll(body, "{md5}", md5);
    HalFile sidecar;
    if (Storage.openFileForWrite("PCAT", path, sidecar)) {
      sidecar.write(body.data(), body.size());
      sidecar.flush();
    } else {
      LOG_ERR("PCAT", "Sidecar write failed: %s", path.c_str());
    }
  }

  state = State::DONE;
  statusMessage = item.title;
  requestUpdate();
}

void PluginCatalogActivity::loop() {
  if (state == State::WIFI_SELECTION || state == State::DOWNLOADING) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }

  if (state == State::AUTH) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::NO_TOKEN;
      requestUpdate();
      return;
    }
    if (static_cast<long>(millis() - authNextPollMs) >= 0) pollAuth();
    return;
  }

  if (state == State::ERROR || state == State::NO_TOKEN) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
        launchWifiSelection();
      } else if (state == State::NO_TOKEN && manifest.hasDeviceCode()) {
        beginAuth();
      } else if (!manifest.browseLists.empty() && !manifest.isXmlList() && currentList < 0) {
        startBrowse();  // nothing picked yet: retry lands on the list picker
      } else {
        state = State::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchPage(page);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state == State::DONE) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tx, ty)) {
      state = State::BROWSING;
      requestUpdate();
    }
    return;
  }

  // BROWSING / LIST_PICKER: one list protocol; rowCount() spans the pager
  // rows plus the items, or the browse lists.
  const ListLayout layout = GUI.getListLayout(renderer, /*hasSubtitle=*/true);
  const int rows = layout.pageItems;
  const int count = rowCount();
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (count > 0) activateRow(selectorIndex);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (manifest.isXmlList() && !browseHistory.empty()) {
      browseCurrentUrl = browseHistory.back();
      browseHistory.pop_back();
      state = State::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate(true);
      fetchXmlList();
    } else if (state == State::BROWSING && currentList >= 0) {
      // Browsing a picked list: Back returns to the picker, not out.
      currentList = -1;
      startBrowse();
    } else {
      finish();
    }
    return;
  }

  if (count > 0) {
    int row = -1;
    const auto touch = mappedInput.rowTouch(row, layout.list.y, layout.rowStep, rows);
    if (touch != MappedInputManager::RowTouch::None) {
      const int touched = selectorIndex / rows * rows + row;
      if (touched >= 0 && touched < count) {
        if (touch == MappedInputManager::RowTouch::Down) {
          if (selectorIndex != touched) {
            selectorIndex = touched;
            requestUpdate();
          }
        } else {
          selectorIndex = touched;
          activateRow(selectorIndex);
        }
        return;
      }
    }

    buttonNavigator.onNextRelease([this, count] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this, count] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this, rows, count] {
      selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, count, rows);
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this, rows, count] {
      selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, count, rows);
      requestUpdate();
    });
  }
}

void PluginCatalogActivity::activateRow(const int row) {
  if (state == State::LIST_PICKER) {
    if (row < 0 || row >= static_cast<int>(manifest.browseLists.size())) return;
    currentList = row;
    startBrowse();
    return;
  }
  // The pager rows bracket the items: "Previous page" ahead of them past
  // page 1, "Next page" after them while more pages exist.
  const int itemIndex = row - (prevRowVisible() ? 1 : 0);
  if (itemIndex == -1 || (nextRowVisible() && itemIndex == static_cast<int>(items.size()))) {
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchPage(itemIndex == -1 ? page - 1 : page + 1);
    return;
  }
  activateItem(itemIndex);
}

void PluginCatalogActivity::activateItem(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const Item& item = items[itemIndex];
  if (manifest.isXmlList() && item.isDir) {
    browseHistory.push_back(browseCurrentUrl);
    browseCurrentUrl = item.url;
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchXmlList();
    return;
  }
  downloadItem(item);
}

void PluginCatalogActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& m = UITheme::getInstance().getMetrics();
  const int centerY = pageHeight / 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  std::string header = catalogTitle;
  if (state == State::BROWSING && currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size())) {
    header = manifest.browseLists[currentList].title;
  }
  if (state == State::BROWSING && page > 1) {
    char suffix[16];
    snprintf(suffix, sizeof(suffix), " %d", page);
    header += suffix;
  }
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageWidth, m.headerHeight}, header.c_str());

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::AUTH) {
    // Device-code sign-in: verification URL (text + QR) and the user code.
    const int top = m.topPadding + m.headerHeight + m.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, authVerifyUrl.c_str());
    renderer.drawCenteredText(UI_12_FONT_ID, top + 35, authUserCode.c_str(), true, EpdFontFamily::BOLD);
    const int qrSize = 180;
    QrUtils::drawQrCode(renderer, Rect{(pageWidth - qrSize) / 2, top + 70, qrSize, qrSize}, authVerifyUrl);
    renderer.drawCenteredText(UI_10_FONT_ID, top + 70 + qrSize + 20, tr(STR_PLUGIN_AUTH_WAITING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR || state == State::NO_TOKEN) {
    const bool canSignIn = state == State::NO_TOKEN && manifest.hasDeviceCode();
    if (state == State::NO_TOKEN) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY,
                                canSignIn ? tr(STR_PLUGIN_SIGN_IN_HINT) : tr(STR_PLUGIN_NOT_SIGNED_IN));
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_ERROR_MSG), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + m.verticalSpacing, errorMessage.c_str());
    }
    const char* confirmLabel = canSignIn ? tr(STR_PLUGIN_SIGN_IN) : tr(STR_RETRY);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DOWNLOADING) {
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - m.contentSidePadding * 2);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOADING));
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, title.c_str());
    const int pct =
        downloadTotal > 0 ? static_cast<int>(static_cast<uint64_t>(downloadProgress) * 100 / downloadTotal) : 0;
    GUI.drawProgressBar(renderer,
                        Rect{m.contentSidePadding, centerY + m.verticalSpacing + lineHeight,
                             pageWidth - m.contentSidePadding * 2, m.progressBarHeight},
                        pct, 100);
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DONE) {
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - m.contentSidePadding * 2);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOAD_COMPLETE), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + m.verticalSpacing, title.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // BROWSING / LIST_PICKER: one list body. Pager rows bracket the items:
  // "« Previous page" ahead of the items past page 1, "Next page »" after
  // them while more pages exist — tappable and button-reachable like any
  // row. The direction markers come from the translated strings themselves.
  const ListLayout layout = GUI.getListLayout(renderer, /*hasSubtitle=*/true);
  const int count = rowCount();
  const int prevOff = prevRowVisible() ? 1 : 0;
  const int itemSel = selectorIndex - prevOff;
  const char* confirmLabel;
  if (state == State::LIST_PICKER) {
    confirmLabel = count > 0 ? tr(STR_OPEN) : "";
  } else if (prevOff && selectorIndex == 0) {
    confirmLabel = tr(STR_PREV_PAGE);
  } else if (nextRowVisible() && itemSel == static_cast<int>(items.size())) {
    confirmLabel = tr(STR_NEXT_PAGE);
  } else {
    const bool onDir = manifest.isXmlList() && itemSel >= 0 && itemSel < static_cast<int>(items.size()) &&
                       items[itemSel].isDir;
    confirmLabel = items.empty() ? "" : (onDir ? tr(STR_OPEN) : tr(STR_FETCH));
  }
  const char* up = count > 1 ? tr(STR_DIR_UP) : "";
  const char* down = count > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (count == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_ENTRIES));
  } else {
    GUI.drawList(
        renderer, layout.list, count, selectorIndex,
        [this, prevOff](int i) -> std::string {
          if (state == State::LIST_PICKER) return manifest.browseLists[i].title;
          if (prevOff && i == 0) return tr(STR_PREV_PAGE);
          const int item = i - prevOff;
          if (item >= static_cast<int>(items.size())) return tr(STR_NEXT_PAGE);
          return manifest.isXmlList() && items[item].isDir ? "> " + items[item].title : items[item].title;
        },
        [this, prevOff](int i) -> std::string {
          if (state == State::LIST_PICKER) return "";
          const int item = i - prevOff;
          if (item < 0 || item >= static_cast<int>(items.size())) return "";  // pager rows
          return items[item].author;
        });
  }
  renderer.displayBuffer();
}
