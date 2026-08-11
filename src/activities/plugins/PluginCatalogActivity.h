#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HalFile;
namespace freeink {
class SecureHttpClient;
}

// One installed SD plugin, as surfaced in the on-device Plugins list.
struct PluginRef {
  std::string name;          // folder name
  std::string title;         // from device.json or manifest.json (falls back to name)
  std::string description;   // one-line summary, if provided
  std::string manifestPath;  // device.json path, "" when the plugin has no on-device screen
  std::string readmePath;    // README.md path, "" when absent
  bool hasCatalog = false;   // a device.json is present (an Open action is offered)
};

// Scans every plugin folder across the SD plugin roots. Called on demand (when
// the list opens), so nothing stays resident while it is closed.
std::vector<PluginRef> discoverPlugins();

// Cheap check for the home screen: true if any plugin folder exists (a folder
// under a plugin root holding plugin.js or device.json). Reads no manifests.
bool anyPluginInstalled();

/**
 * Generic on-device catalog browser driven by an SD plugin's device.json.
 * The manifest is pure data (URL/header/body templates plus JSON field
 * paths), so a new service is an SD card file, not firmware. Anything the
 * vocabulary cannot express stays in the plugin's browser-side plugin.js.
 */
class PluginCatalogActivity final : public Activity {
 public:
  enum class State {
    CHECK_WIFI,
    WIFI_SELECTION,
    LIST_PICKER,
    LOADING,
    BROWSING,
    DOWNLOADING,
    DONE,
    ERROR,
    NO_TOKEN,
    AUTH
  };

  // Both out of line: ctor and dtor instantiate ~unique_ptr<SecureHttpClient>,
  // which needs the complete type.
  explicit PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string manifestPath,
                                 std::string title);
  ~PluginCatalogActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Manifest {
    // {token} comes from tokenFile at tokenPath (dotted JSON path). {cfg.KEY}
    // comes from a flat JSON config file (configFile), letting a plugin store
    // user-entered values (e.g. a server URL and credentials) outside the
    // manifest instead of hardcoding them.
    std::string tokenFile, tokenPath;
    std::string configFile;
    // "json" (default) parses a paged JSON list. "xml" walks a repeating XML
    // element (a WebDAV multistatus, an OPDS/Atom feed, ...) with optional
    // folder navigation — the format is data, so the firmware knows no protocol.
    std::string browseFormat;
    // Browse request: templates may use {token}, {cfg.KEY}, {page}, {limit}.
    std::string browseUrl, browseMethod, browseBody;
    std::vector<std::pair<std::string, std::string>> browseHeaders;
    std::string itemsPath;  // JSON: dotted path to the item array; "" = response root
    // JSON field paths (dotted); XML field selectors ("elem", "elem@attr", "@attr").
    std::string titlePath, authorPath, idPath, urlPath;
    int pageSize = 8;
    // Optional named sub-catalogs ("lists"): each entry may override the
    // browse url/body, so one service exposes several server-side views
    // (categories, shelves, sort orders). When present (JSON lists only), a
    // picker screen precedes browsing and Back returns to it.
    struct BrowseList {
      std::string title, url, body;
    };
    std::vector<BrowseList> browseLists;
    // XML list options:
    std::string xmlItem;                     // local-name of the repeating element (required)
    std::string xmlContainer;                // local-name whose presence marks a navigable folder
    bool xmlSkipSelf = false;                // drop the entry whose url equals the request url
    bool xmlResolveUrls = false;             // resolve url field against the request origin
    std::vector<std::string> xmlExtensions;  // allowed file extensions ("" = all)

    bool isXmlList() const { return browseFormat == "xml"; }
    // Download: templates may additionally use {id}, {title}, {author}, {url}.
    // When dlUrlPath is empty, the substituted dlUrl IS the file URL;
    // otherwise a request is made and the file URL read from dlUrlPath.
    std::string dlUrl, dlMethod, dlBody, dlUrlPath;
    std::vector<std::pair<std::string, std::string>> dlHeaders;
    // Optional HTTP Basic credentials for the file GET; templates.
    std::string dlUser, dlPass;
    std::string destDir, filenameTpl;
    // Optional multi-file "bundle" download: instead of one file, the selected
    // item carries a base URL and a JSON array of relative paths, and every file
    // is fetched into destDir/<subdir>/. Generic (a plugin installer, a theme
    // pack, ...); when bundleFilesPath is set it replaces the single-file path.
    // bundleBasePath/bundleFilesPath are dotted field paths within the item;
    // bundleSubdir is a template (default {id}).
    std::string bundleBasePath, bundleFilesPath, bundleSubdir;
    bool isBundle() const { return !bundleFilesPath.empty(); }
    // Optional sidecar written after a successful download; templates may use
    // {id}, {title}, {md5} (MD5 of the destination path).
    std::string sidecarPath, sidecarBody;
    // Optional on-device sign-in. "device_code": interactive OAuth device-code
    // (shows a code + QR, polls). "password": a silent credential grant that
    // mints a token from stored config credentials before browsing. Both write
    // the token to tokenFile at tokenPath.
    std::string authType;  // "device_code" (default) or "password"
    std::string authUrl, authMethod, authBody;
    std::vector<std::pair<std::string, std::string>> authHeaders;
    std::string pollUrl, pollMethod, pollBody;
    std::vector<std::pair<std::string, std::string>> pollHeaders;
    std::string authCodePath, authVerifyPath, authDeviceCodePath;
    std::string authIntervalPath, authExpiresPath, authTokenPath, authErrorPath;

    bool hasDeviceCode() const { return authType == "device_code" && !authUrl.empty() && !pollUrl.empty(); }
    bool hasPasswordGrant() const { return authType == "password" && !authUrl.empty(); }
  };

  struct Item {
    std::string title, author, id, url;
    bool isDir = false;  // a container/folder (navigable), not a downloadable file
    // Bundle download only: base URL + relative file paths for this item.
    std::string base;
    std::vector<std::string> files;
  };

  std::string manifestPath;
  std::string catalogTitle;
  Manifest manifest;
  ButtonNavigator buttonNavigator;
  State state = State::LOADING;
  std::vector<Item> items;
  std::string token;
  std::vector<std::pair<std::string, std::string>> config;  // {cfg.KEY} values
  int page = 1;
  bool hasMore = false;
  int currentList = -1;  // index into manifest.browseLists; -1 = none/default
  // One TLS session reused across browse requests (setReuse): repeated
  // handshakes permanently fragment the heap. Freed on exit; a request falls
  // back to a stack client when the allocation failed.
  std::unique_ptr<freeink::SecureHttpClient> session;
  // XML-list folder navigation: current container URL and the trail back out.
  std::string browseCurrentUrl;
  std::vector<std::string> browseHistory;
  int selectorIndex = 0;
  bool consumeConfirm = false;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  // Device-code sign-in state
  std::string authUserCode, authVerifyUrl, authDeviceCode;
  unsigned long authIntervalMs = 5000;
  unsigned long authNextPollMs = 0;
  unsigned long authDeadlineMs = 0;

  bool loadManifest();
  bool loadToken();
  void loadConfig();
  bool saveToken(const std::string& value);
  void checkAndConnectWifi();
  void launchWifiSelection();
  // Wi-Fi is up (or a token just arrived): open the list picker when the
  // manifest defines browse lists, else fetch the first page directly.
  void startBrowse();
  // Synthetic pager rows, mirroring the OPDS browser: "Previous page" ahead
  // of the items past page 1, "Next page" after them while more pages exist.
  bool prevRowVisible() const;
  bool nextRowVisible() const;
  // Rows on the current screen: pager rows + items (BROWSING), or the browse
  // lists (LIST_PICKER). selectorIndex runs over these.
  int rowCount() const;
  // Row selectorIndex points at → dispatch: pager rows page, picker rows pick,
  // item rows open/download.
  void activateRow(int row);
  // Browse url/body with the selected browse list's overrides applied.
  const std::string& activeBrowseUrl() const;
  const std::string& activeBrowseBody() const;
  void fetchPage(int newPage);
  void fetchXmlList();
  void activateItem(int itemIndex);  // XML list: navigate into a folder, else download
  void downloadItem(const Item& item);
  void beginAuth();
  void pollAuth();
  bool refreshCredentialToken();  // password grant: mint a token from config creds
  // Substitutes + runs the browse request, streaming the response body to
  // `destPath` on the SD card (a page of catalog JSON can exceed what DRAM
  // holds), retrying once after a fresh password grant on 401/403. Returns
  // HTTP status (or -1 on transport failure).
  int browseRequestToFile(const std::string& urlTemplate, const std::string& bodyTemplate, const char* destPath);
  // Returns the HTTP status, or -1 on transport failure / truncation / cap.
  // `out` holds the body for any real status (error bodies carry OAuth codes).
  int apiRequest(const std::string& url, const std::string& method, const std::string& body,
                 const std::vector<std::pair<std::string, std::string>>& headers, std::string& out);
  // Same request, but the body goes to a file instead of DRAM.
  int apiRequestToFile(const std::string& url, const std::string& method, const std::string& body,
                       const std::vector<std::pair<std::string, std::string>>& headers, const char* destPath);
  std::string substituted(std::string tpl, const Item* item) const;
  bool preventAutoSleep() override { return true; }
};
