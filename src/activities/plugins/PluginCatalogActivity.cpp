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
#include <strings.h>

#include <algorithm>

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
// A page of filtered catalog JSON measures ~20KB; the cap bounds a
// misbehaving server, not normal use.
constexpr size_t MAX_API_RESPONSE = 48 * 1024;
constexpr int MAX_PAGE_SIZE = 16;
constexpr int HEADER_Y = 15;
constexpr int HEADER_X = 16;
constexpr int LIST_TOP = 60;
constexpr int ROW_STEP = 30;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

// Reads a small SD file fully. Files above `cap` fail rather than allocate.
bool readSmallFile(const std::string& path, size_t cap, std::string& out) {
  HalFile file;
  if (!Storage.openFileForRead("PCAT", path, file)) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap) return false;
  out.resize(size);
  return file.read(out.data(), size) == static_cast<int>(size);
}

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
// XML bodies prefix elements with a namespace ("d:", "D:", "atom:", ...), so
// match on the local name only.
std::string xmlLocalName(const std::string& tag) {
  size_t start = 0;
  if (!tag.empty() && tag[0] == '/') start = 1;
  size_t end = start;
  while (end < tag.size() && tag[end] != ' ' && tag[end] != '\t' && tag[end] != '\r' && tag[end] != '\n' &&
         tag[end] != '/' && tag[end] != '>')
    end++;
  std::string name = tag.substr(start, end - start);
  const size_t colon = name.rfind(':');
  return colon == std::string::npos ? name : name.substr(colon + 1);
}

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

// Reads attribute `attr` from a raw tag body (the text between < and >).
// Accepts both quote styles; requires a word boundary before the name.
std::string xmlAttr(const std::string& tag, const std::string& attr) {
  size_t pos = 0;
  while ((pos = tag.find(attr, pos)) != std::string::npos) {
    const bool boundary = pos == 0 || tag[pos - 1] == ' ' || tag[pos - 1] == '\t';
    const size_t after = pos + attr.size();
    if (boundary && after < tag.size() && tag[after] == '=') {
      const char quote = after + 1 < tag.size() ? tag[after + 1] : '\0';
      if (quote == '"' || quote == '\'') {
        const size_t start = after + 2;
        const size_t end = tag.find(quote, start);
        if (end != std::string::npos) return tag.substr(start, end - start);
      }
    }
    pos = after;
  }
  return "";
}

// Evaluates an XML field selector against one item element's inner XML and its
// own opening tag. Selectors: "elem" (child element text), "elem@attr" (child
// element attribute), "@attr" (attribute on the item element itself).
std::string xmlSelect(const std::string& block, const std::string& itemTag, const std::string& selector) {
  if (selector.empty()) return "";
  if (selector[0] == '@') return xmlAttr(itemTag, selector.substr(1));
  const size_t at = selector.find('@');
  const std::string elem = at == std::string::npos ? selector : selector.substr(0, at);
  const std::string attr = at == std::string::npos ? "" : selector.substr(at + 1);
  size_t pos = 0;
  while (pos < block.size()) {
    const size_t lt = block.find('<', pos);
    if (lt == std::string::npos) break;
    const size_t gt = block.find('>', lt);
    if (gt == std::string::npos) break;
    const std::string tag = block.substr(lt + 1, gt - lt - 1);
    if (!tag.empty() && tag[0] != '/' && xmlLocalName(tag) == elem) {
      if (attr.empty()) {
        const size_t te = block.find('<', gt + 1);
        std::string text = te == std::string::npos ? block.substr(gt + 1) : block.substr(gt + 1, te - gt - 1);
        // trim surrounding whitespace
        const size_t b = text.find_first_not_of(" \t\r\n");
        const size_t e = text.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? "" : text.substr(b, e - b + 1);
      }
      return xmlAttr(tag, attr);
    }
    pos = gt + 1;
  }
  return "";
}

// True when the item element's inner XML contains an element with local-name.
bool xmlHasElement(const std::string& block, const std::string& localName) {
  size_t pos = 0;
  while (pos < block.size()) {
    const size_t lt = block.find('<', pos);
    if (lt == std::string::npos) break;
    const size_t gt = block.find('>', lt);
    if (gt == std::string::npos) break;
    const std::string tag = block.substr(lt + 1, gt - lt - 1);
    if (!tag.empty() && tag[0] != '/' && xmlLocalName(tag) == localName) return true;
    pos = gt + 1;
  }
  return false;
}
}  // namespace

std::vector<PluginCatalogRef> discoverPluginCatalogs() {
  std::vector<PluginCatalogRef> catalogs;
  std::vector<std::string> seen;  // earlier roots win on name collisions
  for (size_t r = 0; r < PluginLocations::kRootCount; r++) {
    HalFile root = Storage.open(PluginLocations::kRoots[r]);
    if (!root || !root.isDirectory()) continue;
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) continue;
      char name[64];
      if (entry.getName(name, sizeof(name)) == 0 || name[0] == '.') continue;
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      std::string manifestPath = std::string(PluginLocations::kRoots[r]) + "/" + name + "/device.json";
      if (!Storage.exists(manifestPath.c_str())) continue;
      seen.emplace_back(name);
      std::string raw;
      std::string title = name;
      if (readSmallFile(manifestPath, MAX_MANIFEST_SIZE, raw)) {
        JsonDocument filter;
        filter["title"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) == DeserializationError::Ok &&
            doc["title"].is<const char*>()) {
          title = doc["title"].as<const char*>();
        }
      }
      catalogs.push_back({std::move(title), std::move(manifestPath)});
    }
  }
  return catalogs;
}

bool PluginCatalogActivity::loadManifest() {
  std::string raw;
  if (!readSmallFile(manifestPath, MAX_MANIFEST_SIZE, raw)) return false;
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
  // XML-list items already carry the file URL; default the template to it.
  if (manifest.isXmlList() && manifest.dlUrl.empty()) manifest.dlUrl = "{url}";
  manifest.sidecarPath = dl["sidecar"]["path"] | "";
  manifest.sidecarBody = dl["sidecar"]["body"] | "";

  JsonVariantConst auth = doc["auth"];
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
  if (!readSmallFile(manifest.tokenFile, MAX_TOKEN_FILE_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  token = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.tokenPath));
  return !token.empty();
}

void PluginCatalogActivity::loadConfig() {
  config.clear();
  if (manifest.configFile.empty()) return;
  std::string raw;
  if (!readSmallFile(manifest.configFile, MAX_TOKEN_FILE_SIZE, raw)) return;
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

int PluginCatalogActivity::apiRequest(const std::string& url, const std::string& method, const std::string& body,
                                      const std::vector<std::pair<std::string, std::string>>& headers,
                                      std::string& out) {
  freeink::SecureHttpClient http;
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

void PluginCatalogActivity::onEnter() {
  Activity::onEnter();
  state = State::CHECK_WIFI;
  items.clear();
  page = 1;
  hasMore = false;
  selectorIndex = 0;
  consumeConfirm = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

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
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void PluginCatalogActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchPage(1);
    return;
  }
  launchWifiSelection();
}

void PluginCatalogActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             state = State::LOADING;
                             statusMessage = tr(STR_LOADING);
                             requestUpdate(true);
                             fetchPage(1);
                           } else {
                             state = State::ERROR;
                             errorMessage = tr(STR_WIFI_CONN_FAILED);
                             requestUpdate();
                           }
                         });
}

void PluginCatalogActivity::fetchXmlList() {
  loadConfig();
  if (!loadToken()) {
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

  const std::string body = substituted(manifest.browseBody, nullptr);
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(manifest.browseHeaders.size());
  for (const auto& h : manifest.browseHeaders) headers.emplace_back(h.first, substituted(h.second, nullptr));

  std::string response;
  const int status = apiRequest(browseCurrentUrl, manifest.browseMethod, body, headers, response);
  if (status == 401 || status == 403) {
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (status < 200 || status >= 300) {  // 207 Multi-Status counts as success
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

  // Walk each repeating <item> element and evaluate the field selectors.
  items.clear();
  size_t pos = 0;
  const std::string openPrefix = "<";
  while (items.size() < 200) {
    // Find the next opening tag whose local-name is the item element.
    size_t itemLt = std::string::npos;
    size_t scan = pos;
    std::string itemTag;
    while (scan < response.size()) {
      const size_t lt = response.find('<', scan);
      if (lt == std::string::npos) break;
      const size_t gt = response.find('>', lt);
      if (gt == std::string::npos) break;
      const std::string tag = response.substr(lt + 1, gt - lt - 1);
      if (!tag.empty() && tag[0] != '/' && tag.back() != '/' && xmlLocalName(tag) == manifest.xmlItem) {
        itemLt = gt + 1;  // content starts after this tag
        itemTag = tag;
        break;
      }
      scan = gt + 1;
    }
    if (itemLt == std::string::npos) break;

    // Find its matching close tag (item elements do not nest in practice).
    size_t blockEnd = std::string::npos;
    size_t cscan = itemLt;
    while (cscan < response.size()) {
      const size_t lt = response.find('<', cscan);
      if (lt == std::string::npos) break;
      const size_t gt = response.find('>', lt);
      if (gt == std::string::npos) break;
      const std::string tag = response.substr(lt + 1, gt - lt - 1);
      if (!tag.empty() && tag[0] == '/' && xmlLocalName(tag) == manifest.xmlItem) {
        blockEnd = lt;
        pos = gt + 1;
        break;
      }
      cscan = gt + 1;
    }
    if (blockEnd == std::string::npos) break;
    const std::string block = response.substr(itemLt, blockEnd - itemLt);

    const std::string rawUrl = xmlSelect(block, itemTag, manifest.urlPath);
    if (rawUrl.empty()) continue;
    Item item;
    item.isDir = !manifest.xmlContainer.empty() && xmlHasElement(block, manifest.xmlContainer);
    item.url =
        manifest.xmlResolveUrls && rawUrl.rfind("http", 0) != 0 ? origin + urlEncodePath(urlDecode(rawUrl)) : rawUrl;
    item.author = xmlSelect(block, itemTag, manifest.authorPath);
    item.id = xmlSelect(block, itemTag, manifest.idPath);
    std::string title = xmlSelect(block, itemTag, manifest.titlePath);
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

void PluginCatalogActivity::fetchPage(const int newPage) {
  if (manifest.isXmlList()) {
    fetchXmlList();
    return;
  }
  page = newPage;
  if (!loadToken()) {
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }

  loadConfig();
  const std::string url = substituted(manifest.browseUrl, nullptr);
  const std::string body = substituted(manifest.browseBody, nullptr);
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(manifest.browseHeaders.size());
  for (const auto& h : manifest.browseHeaders) headers.emplace_back(h.first, substituted(h.second, nullptr));

  std::string response;
  const int status = apiRequest(url, manifest.browseMethod, body, headers, response);
  if (status == 401 || status == 403) {
    // Stale or revoked token: back to the sign-in screen, not a raw error.
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (status < 200 || status >= 300) {
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

  JsonDocument doc;
  if (deserializeJson(doc, response, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    state = State::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }
  response.clear();
  response.shrink_to_fit();

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
      state = State::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate(true);
      fetchPage(1);
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
      nullptr, dlUser, dlPass);

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

  state = State::BROWSING;
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
      } else if (state == State::NO_TOKEN && manifest.hasAuth()) {
        beginAuth();
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

  // BROWSING
  const int rows = visibleRows();
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!items.empty()) activateSelected();
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
    } else {
      finish();
    }
    return;
  }
  if (!manifest.isXmlList() && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Next API page; wraps to the first page after the last.
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchPage(hasMore ? page + 1 : 1);
    return;
  }

  if (!items.empty()) {
    int row = -1;
    const auto touch = mappedInput.rowTouch(row, LIST_TOP, ROW_STEP, rows);
    if (touch != MappedInputManager::RowTouch::None) {
      const int touched = selectorIndex / rows * rows + row;
      if (touched >= 0 && touched < static_cast<int>(items.size())) {
        if (touch == MappedInputManager::RowTouch::Down) {
          if (selectorIndex != touched) {
            selectorIndex = touched;
            requestUpdate();
          }
        } else {
          selectorIndex = touched;
          activateSelected();
        }
        return;
      }
    }

    buttonNavigator.onNextRelease([this] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, items.size());
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, items.size());
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this, rows] {
      selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, items.size(), rows);
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this, rows] {
      selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, items.size(), rows);
      requestUpdate();
    });
  }
}

int PluginCatalogActivity::visibleRows() const {
  const int avail = renderer.getScreenHeight() - LIST_TOP - UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int rows = avail / ROW_STEP;
  return rows < 1 ? 1 : rows;
}

void PluginCatalogActivity::activateSelected() {
  if (items.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(items.size())) return;
  const Item& item = items[selectorIndex];
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
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  std::string header = catalogTitle;
  if (state == State::BROWSING && page > 1) {
    char suffix[16];
    snprintf(suffix, sizeof(suffix), " %d", page);
    header += suffix;
  }
  const auto clippedHeader = renderer.truncatedText(UI_12_FONT_ID, header.c_str(), pageWidth - HEADER_X * 2);
  renderer.drawText(UI_12_FONT_ID, HEADER_X, HEADER_Y, clippedHeader.c_str(), true, EpdFontFamily::BOLD);

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::AUTH) {
    // Device-code sign-in: verification URL (text + QR) and the user code.
    renderer.drawCenteredText(UI_10_FONT_ID, 70, authVerifyUrl.c_str());
    renderer.drawCenteredText(UI_12_FONT_ID, 105, authUserCode.c_str(), true, EpdFontFamily::BOLD);
    const int qrSize = 180;
    QrUtils::drawQrCode(renderer, Rect{(pageWidth - qrSize) / 2, 140, qrSize, qrSize}, authVerifyUrl);
    renderer.drawCenteredText(UI_10_FONT_ID, 140 + qrSize + 30, tr(STR_PLUGIN_AUTH_WAITING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR || state == State::NO_TOKEN) {
    const bool canSignIn = state == State::NO_TOKEN && manifest.hasAuth();
    if (state == State::NO_TOKEN) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2,
                                canSignIn ? tr(STR_PLUGIN_SIGN_IN_HINT) : tr(STR_PLUGIN_NOT_SIGNED_IN));
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    }
    const char* confirmLabel = canSignIn ? tr(STR_PLUGIN_SIGN_IN) : tr(STR_RETRY);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  const bool onDir = manifest.isXmlList() && !items.empty() && items[selectorIndex].isDir;
  const char* confirmLabel = onDir ? tr(STR_OPEN) : tr(STR_DOWNLOAD);
  // XML lists page by folder navigation (Confirm/Back), not the API pager.
  const char* moreLabel = (!manifest.isXmlList() && (hasMore || page > 1)) ? tr(STR_NEXT_PAGE) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, moreLabel, "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (items.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const int rows = visibleRows();
    const int pageStart = selectorIndex / rows * rows;
    renderer.fillRect(0, LIST_TOP + (selectorIndex % rows) * ROW_STEP - 2, pageWidth - 1, ROW_STEP);
    for (size_t i = pageStart; i < items.size() && i < static_cast<size_t>(pageStart + rows); i++) {
      std::string text = manifest.isXmlList() && items[i].isDir ? "> " + items[i].title : items[i].title;
      if (!items[i].author.empty()) text += " - " + items[i].author;
      auto line = renderer.truncatedText(UI_10_FONT_ID, text.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, LIST_TOP + static_cast<int>(i % rows) * ROW_STEP, line.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}
