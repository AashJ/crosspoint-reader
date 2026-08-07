#pragma once
#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// A settings-menu entry contributed by an SD plugin's device.json manifest.
struct PluginCatalogRef {
  std::string title;
  std::string manifestPath;
};

// Scans /.crosspoint/plugins/*/device.json. Called when the settings menu is
// (re)built, so nothing stays resident while the menu is closed.
std::vector<PluginCatalogRef> discoverPluginCatalogs();

/**
 * Generic on-device catalog browser driven by an SD plugin's device.json.
 * The manifest is pure data (URL/header/body templates plus JSON field
 * paths), so a new service is an SD card file, not firmware. Anything the
 * vocabulary cannot express stays in the plugin's browser-side plugin.js.
 */
class PluginCatalogActivity final : public Activity {
 public:
  enum class State { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, NO_TOKEN, AUTH };

  explicit PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string manifestPath,
                                 std::string title)
      : Activity("PluginCatalog", renderer, mappedInput),
        manifestPath(std::move(manifestPath)),
        catalogTitle(std::move(title)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Manifest {
    // {token} comes from tokenFile at tokenPath (dotted JSON path).
    std::string tokenFile, tokenPath;
    // Browse request: templates may use {token}, {page}, {limit}.
    std::string browseUrl, browseMethod, browseBody;
    std::vector<std::pair<std::string, std::string>> browseHeaders;
    std::string itemsPath;  // dotted path to the item array; "" = response root
    std::string titlePath, authorPath, idPath, urlPath;
    int pageSize = 8;
    // Download: templates may additionally use {id}, {title}, {author}, {url}.
    // When dlUrlPath is empty, the substituted dlUrl IS the file URL;
    // otherwise a request is made and the file URL read from dlUrlPath.
    std::string dlUrl, dlMethod, dlBody, dlUrlPath;
    std::vector<std::pair<std::string, std::string>> dlHeaders;
    std::string destDir, filenameTpl;
    // Optional sidecar written after a successful download; templates may use
    // {id}, {title}, {md5} (MD5 of the destination path).
    std::string sidecarPath, sidecarBody;
    // Optional on-device sign-in (OAuth device-code flow). The request returns
    // a user code + verification URL to show on-screen; poll templates may use
    // {device_code}. On success the token is written to tokenFile at tokenPath.
    std::string authUrl, authMethod, authBody;
    std::vector<std::pair<std::string, std::string>> authHeaders;
    std::string pollUrl, pollMethod, pollBody;
    std::vector<std::pair<std::string, std::string>> pollHeaders;
    std::string authCodePath, authVerifyPath, authDeviceCodePath;
    std::string authIntervalPath, authExpiresPath, authTokenPath, authErrorPath;

    bool hasAuth() const { return !authUrl.empty() && !pollUrl.empty(); }
  };

  struct Item {
    std::string title, author, id, url;
  };

  std::string manifestPath;
  std::string catalogTitle;
  Manifest manifest;
  ButtonNavigator buttonNavigator;
  State state = State::LOADING;
  std::vector<Item> items;
  std::string token;
  int page = 1;
  bool hasMore = false;
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
  bool saveToken(const std::string& value);
  void checkAndConnectWifi();
  void launchWifiSelection();
  void fetchPage(int newPage);
  void downloadItem(const Item& item);
  void beginAuth();
  void pollAuth();
  // Returns the HTTP status, or -1 on transport failure / truncation / cap.
  // `out` holds the body for any real status (error bodies carry OAuth codes).
  int apiRequest(const std::string& url, const std::string& method, const std::string& body,
                 const std::vector<std::pair<std::string, std::string>>& headers, std::string& out);
  std::string substituted(std::string tpl, const Item* item) const;
  bool preventAutoSleep() override { return true; }
};
