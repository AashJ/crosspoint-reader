# SD-card plugins

CrossPoint keeps service integrations off the firmware. A plugin is a folder on
the SD card; the firmware provides three generic, scheme-neutral surfaces that
plugins compose. Adding or updating a plugin never requires a firmware build,
and the firmware carries no vendor names, URLs, or file-format knowledge.

```
/.crosspoint/plugins/<name>/
    manifest.json     web UI card metadata (optional)
    plugin.js         browser-side plugin (optional)
    device.json       on-device catalog screen (optional)
    ...assets
```

A plugin can ship any combination: `plugin.js` alone (web-only),
`device.json` alone (on-device only), or both (e.g. sign in from either side,
browse on the reader).

The plugin sources and examples live in the separate `sd-plugins` repository,
which also documents the browser-side `plugin.js` API. This document covers the
firmware surfaces: the job queue and the `device.json` on-device screens.

## Surface 1: browser plugins (`plugin.js`)

JS loaded into the File Manager or Settings web page, backed by generic device
endpoints (`/api/relay`, `/api/crypto`, `/api/fetch`, `/api/plugin-fs`). See
`docs/webserver-endpoints.md` for the endpoints and the sd-plugins repository
for the JS contract.

## Surface 2: the plugin job queue (external automation)

External systems (a companion app, a script) can trigger plugin actions
without a human clicking the web UI. The firmware stores small opaque
`{plugin, action, args}` JSON blobs in a fixed 6-slot pool; it never interprets
them. Any open page hosting the plugin — including the headless runner at
`GET /plugins-run` — claims jobs, executes the plugin's registered handler in
the browser context, and posts the result back.

| Endpoint | Purpose |
|---|---|
| `POST /api/plugin-jobs` | enqueue `{plugin, action, args?}` → `{id}` (503 when the pool is full) |
| `GET /api/plugin-jobs/claim?plugin=` | executor claims the next pending job |
| `POST /api/plugin-jobs/complete` | executor posts `{id, ok, result?}` |
| `GET /api/plugin-jobs/status?id=` | caller polls: `{id, state, result}`; states: `pending`, `running`, `done`, `error`, `unknown` (recycled) |
| `GET /plugins-run` | headless page that loads every plugin (UI hidden) and executes jobs while open |

Flow for an external system:

```sh
# 1. upload whatever the action needs (e.g. a file, via /upload)
# 2. enqueue
curl -X POST http://crosspoint.local/api/plugin-jobs \
  -d '{"plugin":"<name>","action":"<action>","args":{"path":"/folder/file"}}'
# -> {"id":7}
# 3. open http://crosspoint.local/plugins-run in a browser tab or hidden
#    webview (jobs only execute while a hosting page is open)
# 4. poll
curl "http://crosspoint.local/api/plugin-jobs/status?id=7"
# -> {"id":7,"state":"done","result":{...}}
```

Limits: plugin/action names < 24 chars, args and result < 192 bytes of JSON
each. A job claimed by a runner that dies is reclaimable after 10 minutes.
Results persist only until their slot is recycled — poll promptly.

Plugins register handlers in `plugin.js`:

```js
api.registerAction('myaction', async (args) => {
  // runs in the hosting page; throw -> state "error" with {error: message}
  return { anything: 'small' };  // -> state "done" with this result
});
```

## Surface 3: on-device catalog screens (`device.json`)

A declarative manifest the firmware's generic `PluginCatalogActivity` renders
under **Settings → System → Plugins**. It expresses "authenticated JSON
catalog: sign in, browse, download, sidecar" — enough for most book services —
without any code running on the device. Anything beyond this vocabulary
belongs in `plugin.js`.

The firmware pieces (all service-agnostic, in `src/activities/plugins/`):
`PluginListActivity` (the Plugins menu), `PluginCatalogActivity` (browse /
download / device-code sign-in), and `discoverPluginCatalogs()` (manifest scan
when the menu opens; nothing stays resident).

### Schema

Every string field is a template. Available substitutions:

| Variable | Meaning | Available in |
|---|---|---|
| `{token}` | contents of the token file at `token.path` | everywhere |
| `{page}` | current 1-based page | browse |
| `{limit}` | `page_size + 1` (the extra row detects "more pages") | browse |
| `{id}` `{title}` `{author}` `{url}` | fields of the selected item | download, sidecar |
| `{md5}` | MD5 hex of the destination file path | sidecar |
| `{device_code}` | from the auth request response | auth poll |

```jsonc
{
  "title": "Service Name",                  // menu label; defaults to folder name

  "token": {                                // omit for token-less catalogs
    "file": "/.crosspoint/<name>.json",     // written by auth (either side)
    "path": "token"                         // dotted JSON path inside the file
  },

  "browse": {                               // required
    "url": "https://.../search",
    "method": "POST",                       // default GET
    "headers": { "Authorization": "Bearer {token}" },
    "body": "{\"page\":{page},\"per_page\":{limit}}",
    "items": "",                            // dotted path to the item array; "" = response root
    "fields": {                             // dotted paths, numeric = array index
      "title": "title",                     // required (items without one are dropped)
      "author": "authors.0.name",
      "id": "id",
      "url": "download_url"                 // when the item carries a direct file URL
    },
    "page_size": 8                          // 1..16; response should honor {limit}
  },

  "download": {
    "url": "https://.../books/{id}/download",
    "method": "POST",
    "headers": { ... },
    "body": "{}",
    "url_path": "url",                      // response field with the file URL;
                                            // omit to treat "url" itself as the file URL
    "dest_dir": "/ServiceName",             // created if missing; falls back to SD root
    "filename": "{title}.epub",             // {title} is filesystem-sanitized here
    "sidecar": {                            // optional per-book metadata file
      "path": "/.crosspoint/<name>_{md5}.json",
      "body": "{\"book_id\":{id}}"
    }
  },

  "auth": {                                 // optional on-device OAuth device-code flow
    "request": { "url": "...", "method": "POST", "headers": { ... }, "body": "..." },
    "poll":    { "url": "...", "method": "POST", "headers": { ... },
                 "body": "{...\"device_code\":\"{device_code}\"}" },
    // response field paths, with their defaults:
    "code_path": "user_code", "verify_url_path": "verification_uri",
    "device_code_path": "device_code", "interval_path": "interval",
    "expires_path": "expires_in", "token_path": "access_token",
    "error_path": "error"
  }
}
```

### Behavior

- **Sign-in**: with an `auth` block, the not-signed-in screen offers Sign in on
  the device: it shows the verification URL (text + QR) and user code, then
  polls until authorized, honoring `authorization_pending`, `slow_down`,
  `expired_token`, and `access_denied`. The token is written to `token.file`
  in the shape `token.path` expects, so the web plugin and the device share
  one sign-in. Without an `auth` block the screen directs the user to the web
  plugin instead.
- **Stale tokens**: a 401/403 from browse returns to the sign-in screen rather
  than an error.
- **Pagination**: the browse request should return up to `{limit}` items; the
  firmware displays `page_size` and uses the extra row to know another page
  exists. The page key cycles forward and wraps to page 1.
- **After download**: the book's layout cache is invalidated and the optional
  sidecar is written (e.g. a service book id keyed by path MD5, for a future
  progress-sync stage).

### Heap budget (ESP32-C3, ~380KB total)

- Manifest ≤ 8KB, token file ≤ 2KB, API responses ≤ 48KB (hard caps).
- Catalog responses are parsed with a dynamically built ArduinoJson filter
  containing only the declared field paths, so a ~20KB page parses into a few
  KB instead of several times the body. Keep `page_size` at 8 unless the
  service's items are tiny.
- The book download streams to SD via `HttpDownloader`; file size is
  unconstrained.

### Testing a new manifest

1. Copy the plugin folder to `/.crosspoint/plugins/<name>/` on the SD card.
2. Settings → System → Plugins → your title. With no token and no `auth`
   block you should see the not-signed-in screen; with `auth`, the code/QR
   screen.
3. Watch serial (`[PCAT]` tag) for request/parse failures — the log includes
   HTTP status and truncation flags.
4. Token-less services (public JSON APIs) work by omitting the `token` block
   entirely; that is the quickest way to validate `items`/`fields` paths.
