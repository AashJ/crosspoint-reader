#include "PluginCatalogActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
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
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "components/icons/search32.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/PluginLocations.h"
#include "util/QrUtils.h"

namespace fui = freeink::ui;

// Header search action, tapped on the browsing screen's search icon. The base
// reserves ACTION_ROW (1); subclass action ids start at ACTION_USER (2).
constexpr fui::ActionId ACTION_SEARCH = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;

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

// Returns `override` unless it's empty, in which case `fallback` applies.
const std::string& pick(const std::string& override, const std::string& fallback) {
  return override.empty() ? fallback : override;
}

// --- XML-list helpers -----------------------------------------------------
std::string urlDecode(const std::string& s) {
  std::string plusesAsSpaces = s;
  std::replace(plusesAsSpaces.begin(), plusesAsSpaces.end(), '+', ' ');
  return FsHelpers::decodeUriEscapes(plusesAsSpaces);
}

// Percent-encodes for a query value: unlike urlEncodePath, '/' is escaped too.
std::string urlEncodeQuery(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || strchr("-_.~", c)) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
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
  manifest.searchUrl = browse["search"]["url"] | "";
  manifest.searchBody = browse["search"]["body"] | "";
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
  const size_t written = file.write(out.data(), out.size());
  file.flush();
  if (written != out.size()) {
    // A short write (SD full, I/O fault) must not persist a truncated token:
    // pollAuth would report success, then every later loadToken() fails.
    LOG_ERR("PCAT", "short token write (%u of %u)", static_cast<unsigned>(written), static_cast<unsigned>(out.size()));
    if (file.isOpen()) file.close();
    Storage.remove(manifest.tokenFile.c_str());
    return false;
  }
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

void PluginCatalogActivity::fail(const StrId msg) {
  state = State::ERROR;
  errorMessage = I18N.get(msg);
  requestUpdate();
}

void PluginCatalogActivity::beginLoading() {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
}

std::vector<std::pair<std::string, std::string>> PluginCatalogActivity::substitutedHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers, const Item* item) const {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(headers.size());
  for (const auto& h : headers) out.emplace_back(h.first, substituted(h.second, item));
  return out;
}

std::string PluginCatalogActivity::substituted(std::string tpl, const Item* item) const {
  substituteAll(tpl, "{token}", token);
  for (const auto& kv : config) substituteAll(tpl, ("{cfg." + kv.first + "}").c_str(), kv.second);
  char num[16];
  snprintf(num, sizeof(num), "%d", page);
  substituteAll(tpl, "{page}", num);
  snprintf(num, sizeof(num), "%d", manifest.pageSize + 1);
  substituteAll(tpl, "{limit}", num);
  substituteAll(tpl, "{query}", urlEncodeQuery(searchQuery));
  substituteAll(tpl, "{query_raw}", searchQuery);
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
    : UiListActivity("PluginCatalog", renderer, mappedInput),
      manifestPath(std::move(manifestPath)),
      catalogTitle(std::move(title)) {}

PluginCatalogActivity::~PluginCatalogActivity() = default;

freeink::SecureHttpClient* PluginCatalogActivity::openApiClient(
    freeink::SecureHttpClient& tmp, const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers) {
  freeink::SecureHttpClient& http = session ? *session : tmp;
  http.setUserAgent("CrossPoint");
  // Same trust posture as every other SecureNet consumer: no CA bundle ships
  // with the transport, so verification is skipped; traffic stays encrypted.
  http.setInsecure();
  http.setTimeout(30000);
  if (!http.begin(url)) return nullptr;
  for (const auto& h : headers) http.addHeader(h.first, h.second);
  return &http;
}

int PluginCatalogActivity::apiRequest(const std::string& url, const std::string& method, const std::string& body,
                                      const std::vector<std::pair<std::string, std::string>>& headers,
                                      std::string& out) {
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient* httpPtr = openApiClient(tmp, url, headers);
  if (!httpPtr) return -1;
  freeink::SecureHttpClient& http = *httpPtr;

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
  freeink::SecureHttpClient* httpPtr = openApiClient(tmp, url, headers);
  if (!httpPtr) return -1;
  freeink::SecureHttpClient& http = *httpPtr;

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
  UiListActivity::onEnter();
  app.on(ACTION_SEARCH, &PluginCatalogActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &PluginCatalogActivity::onCancelEvent, this);
  state = State::CHECK_WIFI;
  items.clear();
  rowsDirty = true;
  page = 1;
  hasMore = false;
  currentList = -1;
  searchActive = false;
  searchQuery.clear();
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  session.reset(new (std::nothrow) freeink::SecureHttpClient());
  if (session) session->setReuse(true);

  if (!loadManifest()) {
    fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
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
    // Same auth gate as fetchPage: without it a signed-out user is shown the
    // list picker and only hits the sign-in screen after picking a list.
    // loadToken() returns true for token-less catalogs, which skip the gate.
    loadConfig();
    if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
      if (manifest.hasDeviceCode()) {
        beginAuth();  // straight to the QR/code sign-in, no interstitial
      } else {
        state = State::NO_TOKEN;
        requestUpdate();
      }
      return;
    }
    items.clear();
    page = 1;
    hasMore = false;
    releaseRows();
    nav.reset();
    state = State::LIST_PICKER;
    requestUpdate();
    return;
  }
  beginLoading();
  fetchPage(1);
}

void PluginCatalogActivity::onSearchEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<PluginCatalogActivity*>(user);
  if (self->state == State::BROWSING && self->manifest.hasSearch()) self->launchSearch();
}

void PluginCatalogActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<PluginCatalogActivity*>(user);
  if (self->state != State::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void PluginCatalogActivity::pumpDownloadInput() {
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
  if (mappedInput.wasHomeGesture()) {
    cancelDownload = true;
    goHomeAfterCancel = true;
  }
  routeTouch(mappedInput);
}

void PluginCatalogActivity::finishCancelledDownload() {
  LOG_INF("PCAT", "Download cancelled");
  if (goHomeAfterCancel) {
    onGoHome();
    return;
  }
  state = State::BROWSING;
  requestUpdate();
}

void PluginCatalogActivity::launchSearch() {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void PluginCatalogActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    requestUpdate();
    return;
  }
  searchQuery = query;
  searchActive = true;
  currentList = -1;  // search spans the whole catalog, not a single list
  releaseRows();
  nav.reset();
  beginLoading();
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
                             fail(StrId::STR_WIFI_CONN_FAILED);
                           }
                         });
}

bool PluginCatalogActivity::refreshCredentialToken() {
  loadConfig();
  const auto headers = substitutedHeaders(manifest.authHeaders, nullptr);
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
    headers = substitutedHeaders(manifest.browseHeaders, nullptr);
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
    fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
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
    fail(StrId::STR_FETCH_FEED_FAILED);
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

  releaseRows();
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
  nav.reset();
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
  // Search overrides the list view, reusing the browse url when unspecified.
  if (searchActive) return pick(manifest.searchUrl, manifest.browseUrl);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseUrl;
  return pick(manifest.browseLists[currentList].url, manifest.browseUrl);
}

const std::string& PluginCatalogActivity::activeBrowseBody() const {
  if (searchActive) return pick(manifest.searchBody, manifest.browseBody);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseBody;
  return pick(manifest.browseLists[currentList].body, manifest.browseBody);
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
    fail(StrId::STR_FETCH_FEED_FAILED);
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
      fail(StrId::STR_PARSE_FEED_FAILED);
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
      fail(StrId::STR_PARSE_FEED_FAILED);
      return;
    }
  }
  Storage.remove(BROWSE_TMP_PATH);

  JsonVariantConst itemsNode = resolvePath(doc.as<JsonVariantConst>(), manifest.itemsPath);
  JsonArrayConst arr = itemsNode.as<JsonArrayConst>();
  releaseRows();
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
  nav.reset();
  state = State::BROWSING;
  requestUpdate();
}

void PluginCatalogActivity::beginAuth() {
  beginLoading();

  const auto headers = substitutedHeaders(manifest.authHeaders, nullptr);
  std::string response;
  const int status = apiRequest(substituted(manifest.authUrl, nullptr), manifest.authMethod,
                                substituted(manifest.authBody, nullptr), headers, response);
  JsonDocument doc;
  if (status < 200 || status >= 300 || deserializeJson(doc, response) != DeserializationError::Ok) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  authUserCode = variantToString(resolvePath(root, manifest.authCodePath));
  authVerifyUrl = variantToString(resolvePath(root, manifest.authVerifyPath));
  authDeviceCode = variantToString(resolvePath(root, manifest.authDeviceCodePath));
  const long interval = resolvePath(root, manifest.authIntervalPath) | 5L;
  const long expires = resolvePath(root, manifest.authExpiresPath) | 900L;
  if (authUserCode.empty() || authDeviceCode.empty()) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
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

  const auto headers = substitutedHeaders(manifest.pollHeaders, nullptr);
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
        fail(StrId::STR_PLUGIN_AUTH_FAILED);
        return;
      }
      startBrowse();
      return;
    }
    const std::string code = variantToString(resolvePath(root, manifest.authErrorPath));
    if (code == "slow_down") {
      authIntervalMs += 5000;
    } else if (code == "expired_token" || code == "access_denied") {
      fail(StrId::STR_PLUGIN_AUTH_FAILED);
      return;
    }
    // authorization_pending (or anything unrecognized): keep polling
  }

  if (static_cast<long>(millis() - authDeadlineMs) >= 0) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
  }
}

void PluginCatalogActivity::downloadItem(const Item& item) {
  state = State::DOWNLOADING;
  statusMessage = item.title;
  downloadProgress = 0;
  cancelDownload = false;
  goHomeAfterCancel = false;
  requestUpdate(true);

  // Multi-file bundle install: fetch every file in item.files from item.base
  // into destDir/<subdir>/, creating intermediate folders. Each file reports
  // its transferred byte count. Generic (a plugin installer, a theme pack, ...).
  if (manifest.isBundle() && !item.files.empty()) {
    const std::string subdir = substituted(manifest.bundleSubdir, &item);
    // Reject path traversal in the subdir (a hostile catalog could escape).
    if (subdir.empty() || subdir.find("..") != std::string::npos || subdir.front() == '/') {
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    std::string dir = manifest.destDir;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    dir += '/';
    dir += subdir;
    if (!Storage.exists(dir.c_str()) && !Storage.mkdir(dir.c_str())) {
      LOG_ERR("PCAT", "bundle mkdir failed: %s", dir.c_str());
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    std::string base = item.base;
    if (!base.empty() && base.back() != '/') base += '/';
    const size_t total = item.files.size();
    // Written files, for rollback: a half-installed bundle folder would show
    // up as a broken plugin/theme in the next discovery scan.
    std::vector<std::string> written;
    written.reserve(total);
    const auto rollback = [&] {
      for (const auto& path : written) Storage.remove(path.c_str());
      Storage.rmdir(dir.c_str());  // only succeeds when the folder emptied out
    };
    for (size_t i = 0; i < total; i++) {
      std::string rel = item.files[i];
      while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
      if (rel.empty() || rel.find("..") != std::string::npos) {
        // A manifest listing traversal entries is hostile or broken either
        // way; abort rather than install a bundle with silent holes.
        LOG_ERR("PCAT", "unsafe bundle entry rejected: %s", item.files[i].c_str());
        rollback();
        fail(StrId::STR_DOWNLOAD_FAILED);
        return;
      }
      downloadProgress = 0;
      const std::string dest = dir + "/" + rel;
      // Create any intermediate folders for nested files ("assets/icon.bin").
      const size_t slash = dest.find_last_of('/');
      if (slash != std::string::npos) {
        const std::string parent = dest.substr(0, slash);
        if (!Storage.exists(parent.c_str())) Storage.mkdir(parent.c_str());
      }
      int lastRenderedPercent = -1;
      unsigned long lastProgressUpdateMs = 0;
      const auto res = HttpDownloader::downloadToFile(
          base + rel, dest,
          [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
            downloadProgress = downloaded;
            pumpDownloadInput();
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
          &cancelDownload);
      if (res == HttpDownloader::ABORTED) {
        rollback();
        finishCancelledDownload();
        return;
      }
      if (res != HttpDownloader::OK) {
        LOG_ERR("PCAT", "bundle file failed: %s (%d)", rel.c_str(), static_cast<int>(res));
        rollback();
        fail(StrId::STR_DOWNLOAD_FAILED);
        return;
      }
      written.push_back(dest);
    }
    state = State::DONE;
    statusMessage = item.title;
    requestUpdate();
    return;
  }

  // Resolve the file URL: either the template itself, or one API hop away.
  std::string fileUrl = substituted(manifest.dlUrl, &item);
  if (!manifest.dlUrlPath.empty()) {
    const auto headers = substitutedHeaders(manifest.dlHeaders, &item);
    std::string response;
    const int status = apiRequest(fileUrl, manifest.dlMethod, substituted(manifest.dlBody, &item), headers, response);
    if (status < 200 || status >= 300) {
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    fileUrl = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.dlUrlPath));
  }
  if (fileUrl.empty()) {
    fail(StrId::STR_DOWNLOAD_FAILED);
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
  // {id}/{author}/{url} come raw from the catalog response; a hostile server
  // could smuggle separators through them to escape destDir (the bundle
  // branch guards its subdir the same way).
  const std::string filename = substituted(manifest.filenameTpl, &named);
  if (filename.empty() || filename.find("..") != std::string::npos || filename.find('/') != std::string::npos) {
    LOG_ERR("PCAT", "unsafe filename rejected: %s", filename.c_str());
    fail(StrId::STR_DOWNLOAD_FAILED);
    return;
  }
  std::string dest;
  dest.reserve(96);
  if (haveFolder) dest += folder;
  dest += '/';
  dest += filename;

  const std::string dlUser = substituted(manifest.dlUser, &item);
  const std::string dlPass = substituted(manifest.dlPass, &item);
  const auto dlHeaders = substitutedHeaders(manifest.dlHeaders, &item);
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      fileUrl, dest,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        pumpDownloadInput();
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
      &cancelDownload, dlUser, dlPass, dlHeaders);

  if (result == HttpDownloader::ABORTED) {
    finishCancelledDownload();
    return;
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("PCAT", "Download failed: %d", static_cast<int>(result));
    fail(StrId::STR_DOWNLOAD_FAILED);
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
    // Sidecar paths legitimately contain '/', but the substituted fields must
    // not climb out of the tree.
    if (path.empty() || path.find("..") != std::string::npos) {
      LOG_ERR("PCAT", "unsafe sidecar path rejected: %s", path.c_str());
    } else {
      std::string body = substituted(manifest.sidecarBody, &sidecarItem);
      substituteAll(body, "{md5}", md5);
      HalFile sidecar;
      if (Storage.openFileForWrite("PCAT", path, sidecar)) {
        if (sidecar.write(body.data(), body.size()) != body.size()) {
          LOG_ERR("PCAT", "short sidecar write: %s", path.c_str());
          if (sidecar.isOpen()) sidecar.close();
          Storage.remove(path.c_str());
        } else {
          sidecar.flush();
        }
      } else {
        LOG_ERR("PCAT", "Sidecar write failed: %s", path.c_str());
      }
    }
  }

  state = State::DONE;
  statusMessage = item.title;
  requestUpdate();
}

// Every state except BROWSING/LIST_PICKER consumes the pass here; the base
// list protocol (Back/Confirm, touch routing, swipe scroll, button
// navigation) only ever runs for the two list states.
bool PluginCatalogActivity::handleCustomInput() {
  if (state == State::WIFI_SELECTION || state == State::DOWNLOADING) return true;

  if (state == State::AUTH) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::NO_TOKEN;
      requestUpdate();
      return true;
    }
    if (static_cast<long>(millis() - authNextPollMs) >= 0) pollAuth();
    return true;
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
        beginLoading();
        fetchPage(page);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return true;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return true;
  }

  if (state == State::DONE) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tx, ty)) {
      state = State::BROWSING;
      requestUpdate();
    }
    return true;
  }

  // The header search icon is reachable by buttons too: on the top row, where
  // previous-nav is a no-op, an Up press (NavPrevious) launches the query. It
  // is never a list row. Touch taps the icon, routed as ACTION_SEARCH.
  if (state == State::BROWSING && manifest.hasSearch() && nav.selected == 0 &&
      mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    launchSearch();
    return true;
  }

  return false;
}

void PluginCatalogActivity::onBackButton() {
  if (searchActive) {
    // Leave the results and return to the pre-search view (picker or page 1).
    searchActive = false;
    searchQuery.clear();
    startBrowse();
    return;
  }
  if (manifest.isXmlList() && !browseHistory.empty()) {
    browseCurrentUrl = browseHistory.back();
    browseHistory.pop_back();
    beginLoading();
    fetchXmlList();
  } else if (state == State::BROWSING && currentList >= 0) {
    // Browsing a picked list: Back returns to the picker, not out.
    currentList = -1;
    startBrowse();
  } else {
    finish();
  }
}

void PluginCatalogActivity::activateIndex(const int index) {
  if (state == State::LIST_PICKER) {
    if (index < 0 || index >= static_cast<int>(manifest.browseLists.size())) return;
    app.clearTapFlash();
    currentList = index;
    startBrowse();
    return;
  }
  // The pager rows bracket the items: "Previous page" ahead of them past
  // page 1, "Next page" after them while more pages exist.
  const int itemIndex = index - (prevRowVisible() ? 1 : 0);
  if (itemIndex == -1 || (nextRowVisible() && itemIndex == static_cast<int>(items.size()))) {
    app.clearTapFlash();
    beginLoading();
    fetchPage(itemIndex == -1 ? page - 1 : page + 1);
    return;
  }
  app.clearTapFlash();  // the row leaves this screen (folder, download view)
  activateItem(itemIndex);
}

void PluginCatalogActivity::activateItem(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const Item& item = items[itemIndex];
  if (manifest.isXmlList() && item.isDir) {
    browseHistory.push_back(browseCurrentUrl);
    browseCurrentUrl = item.url;
    beginLoading();
    fetchXmlList();
    return;
  }
  downloadItem(item);
}

void PluginCatalogActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER: {
      const int count = rowCount();
      const int prevOff = prevRowVisible() ? 1 : 0;
      const int itemSel = nav.selected - prevOff;
      const char* confirmLabel;
      if (state == State::LIST_PICKER) {
        confirmLabel = count > 0 ? tr(STR_OPEN) : "";
      } else {
        // Folders open; items and the pager rows both fetch from the server.
        const bool onDir =
            manifest.isXmlList() && itemSel >= 0 && itemSel < static_cast<int>(items.size()) && items[itemSel].isDir;
        confirmLabel = count == 0 ? "" : (onDir ? tr(STR_OPEN) : tr(STR_FETCH));
      }
      // On the top row of a searchable catalog the previous-nav slot becomes
      // Search (front Left), mirroring the OPDS browser's side-button search.
      const bool searchable = state == State::BROWSING && manifest.hasSearch() && nav.selected == 0;
      const char* up = searchable ? tr(STR_SEARCH) : (count > 1 ? tr(STR_DIR_UP) : "");
      const char* down = count > 1 ? tr(STR_DIR_DOWN) : "";
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
      break;
    }
    case State::ERROR:
    case State::NO_TOKEN: {
      const bool canSignIn = state == State::NO_TOKEN && manifest.hasDeviceCode();
      labels = mappedInput.mapLabels(tr(STR_BACK), canSignIn ? tr(STR_PLUGIN_SIGN_IN) : tr(STR_RETRY), "", "");
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    default:  // CHECK_WIFI / LOADING / AUTH / DONE (and child-activity handoffs)
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PluginCatalogActivity::buildScreen(UiScreen& screen) {
  // One header renderer for every state (catalogScreenHeader), so the header
  // never changes size or shifts as the plugin moves between states. The search
  // icon rides along only while browsing a searchable catalog; the picker and
  // the status screens show the plain plugin title.
  const bool listState = state == State::BROWSING || state == State::LIST_PICKER;
  const std::string title = listState ? browsingHeaderLabel() : catalogTitle;
  const bool withSearch = state == State::BROWSING && manifest.hasSearch();
  catalogScreenHeader(screen, renderer, title.c_str(),
                      withSearch ? fui::bitmapFromIcon(icon_search_32) : fui::BitmapRef{},
                      withSearch ? ACTION_SEARCH : fui::NO_ACTION);

  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER:
      buildBrowsingScreen(screen);
      return;
    case State::AUTH:
      buildAuthScreen(screen);
      return;
    case State::DOWNLOADING:
      catalogDownloadScreen(screen, statusMessage.c_str(), downloadProgress, 0, ACTION_CANCEL,
                            CatalogDownloadProgressStyle::TransferredBytes);
      return;
    case State::DONE:
      catalogCenteredBlock(screen, {{tr(STR_DOWNLOAD_COMPLETE), true}, {statusMessage.c_str()}});
      return;
    case State::ERROR:
      if (mappedInput.hasTouch()) {
        catalogCenteredBlock(screen, {{tr(STR_ERROR_MSG), true}, {errorMessage.c_str()}, {tr(STR_TAP_TO_RETRY)}});
      } else {
        catalogCenteredBlock(screen, {{tr(STR_ERROR_MSG), true}, {errorMessage.c_str()}});
      }
      return;
    case State::NO_TOKEN:
      catalogCenteredBlock(screen,
                           {{manifest.hasDeviceCode() ? tr(STR_PLUGIN_SIGN_IN_HINT) : tr(STR_PLUGIN_NOT_SIGNED_IN)}});
      return;
    default:  // CHECK_WIFI / LOADING (and the brief child-activity handoffs)
      screen.centeredText(statusMessage.c_str(), screen.theme().bodyText);
      return;
  }
}

// Device-code sign-in: verification URL (text + QR) and the user code. The QR
// bitmap itself is painted by render() into the rect measured here.
void PluginCatalogActivity::buildAuthScreen(UiScreen& screen) {
  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  fui::TextStyle code = screen.theme().titleText;
  code.align = fui::TextAlign::Center;
  code.bold = true;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = screen.theme().spaceMd;

  screen.target().text(screen.takeTop(lh, gap), authVerifyUrl.c_str(), centered);
  screen.target().text(screen.takeTop(screen.target().lineHeight(code.font), gap), authUserCode.c_str(), code);

  constexpr int16_t qrSize = 180;
  const fui::Rect band = screen.takeTop(qrSize, gap);
  authQrRect = fui::Rect{static_cast<int16_t>(band.x + (band.width - qrSize) / 2), band.y, qrSize, qrSize};

  screen.target().text(screen.takeTop(lh), tr(STR_PLUGIN_AUTH_WAITING), centered);
}

std::string PluginCatalogActivity::browsingHeaderLabel() const {
  std::string label = catalogTitle;
  if (searchActive) {
    label = std::string(tr(STR_SEARCH)) + ": " + searchQuery;
  } else if (state == State::BROWSING && currentList >= 0 &&
             currentList < static_cast<int>(manifest.browseLists.size())) {
    label = manifest.browseLists[currentList].title;
  }
  if (state == State::BROWSING && page > 1) {
    char suffix[16];
    snprintf(suffix, sizeof(suffix), " %d", page);
    label += suffix;
  }
  return label;
}

void PluginCatalogActivity::buildBrowsingScreen(UiScreen& screen) {
  if (rowsDirty) {
    rebuildRowItems();
    rowsDirty = false;
  }

  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

// Derives rowItems from the current state's row source: the browse lists
// (LIST_PICKER) or the pager rows bracketing the items (BROWSING). Labels
// point into `manifest`/`items` strings, which outlive the buffer.
void PluginCatalogActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(rowCount());
  if (state == State::LIST_PICKER) {
    for (const auto& list : manifest.browseLists) {
      fui::ListItem item;
      item.label = list.title.c_str();
      item.actionValue = static_cast<int16_t>(rowItems.size());
      rowItems.push_back(item);
    }
    return;
  }
  if (prevRowVisible()) {
    fui::ListItem prev;
    prev.label = tr(STR_PREV_PAGE);
    prev.value = ">";
    prev.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(prev);
  }
  for (const auto& entry : items) {
    fui::ListItem item;
    item.label = entry.title.c_str();
    if (!entry.author.empty()) item.subtitle = entry.author.c_str();
    if (entry.isDir) item.value = ">";
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
  if (nextRowVisible()) {
    fui::ListItem next;
    next.label = tr(STR_NEXT_PAGE);
    next.value = ">";
    next.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(next);
  }
}

void PluginCatalogActivity::releaseRows() {
  // The app's interaction table holds row indices (and hit rects) for the old
  // rows; stop routing touches against it until the next render.
  closeRouting();
  rowsDirty = true;
}

void PluginCatalogActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderUi();
  // The QR is a raw-renderer overlay: FreeInkUI has no QR component, and
  // QrUtils draws straight into the framebuffer the app just painted.
  if (state == State::AUTH && authQrRect.width > 0) {
    QrUtils::drawQrCode(renderer, Rect{authQrRect.x, authQrRect.y, authQrRect.width, authQrRect.height}, authVerifyUrl);
  }
  drawFooter();
  renderer.displayBuffer();
}
