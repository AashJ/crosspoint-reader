  // --- browser-side plugin host (shared by Settings + File Manager) ---------
  // Discovers plugins from the SD card (/api/plugins), loads each plugin.js,
  // and calls its registered render fn with a container + a small api. A plugin
  // is just JS on the SD; adding one needs no firmware change.
  window.CrossPoint = window.CrossPoint || {
    _pending: null,
    registerPlugin(fn) { this._pending = fn; },
    // --- plugin job queue -------------------------------------------------
    // A plugin registers named actions; external systems enqueue jobs with
    // POST /api/plugin-jobs, and any open page hosting the plugin (File
    // Manager, Settings, or the headless /plugins-run page) claims and
    // executes them here, then posts the result back for the caller to poll.
    _actions: {},
    _actionBusy: false,
    _actionTimer: null,
    registerAction(pluginName, action, fn) {
      (this._actions[pluginName] = this._actions[pluginName] || {})[action] = fn;
      if (!this._actionTimer) this._actionTimer = setInterval(() => this._pollJobs(), 2500);
    },
    async _pollJobs() {
      if (this._actionBusy) return;
      this._actionBusy = true;
      try {
        for (const plugin of Object.keys(this._actions)) {
          let job;
          try { job = await (await fetch('/api/plugin-jobs/claim?plugin=' + encodeURIComponent(plugin))).json(); }
          catch (e) { continue; }
          if (!job || !job.id) continue;
          const fn = this._actions[plugin][job.action];
          let ok = false;
          let result;
          if (!fn) {
            result = { error: 'unknown action: ' + job.action };
          } else {
            try { result = (await fn(job.args || {})) || {}; ok = true; }
            catch (e) { result = { error: String((e && e.message) || e) }; }
          }
          try { await this._post('/api/plugin-jobs/complete', { id: job.id, ok, result }); } catch (e) {}
        }
      } finally {
        this._actionBusy = false;
      }
    },
    async _post(path, body) {
      const r = await fetch(path, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
      });
      if (!r.ok) {
        let detail = '';
        try { detail = (await r.json()).error || ''; } catch (e) {}
        throw new Error(path + ' ' + r.status + (detail ? ': ' + detail : ''));
      }
      return r.json();
    },
    // Device capabilities a page can't do itself. relay() resolves to
    // { status, body, headers }, where headers is an ordered list of
    // [name, value] pairs (duplicates preserved, so every Set-Cookie shows up).
    relay(plugin, method, url, headers, body) {
      return this._post('/api/relay', { plugin, method, url, headers: headers || {}, body: body || '' });
    },
    // Collect Set-Cookie values from a relay response into a Cookie header
    // string, so a plugin can carry a session across requests. Generic — this
    // just reads the standard header; it ascribes no meaning to the contents.
    cookiesFrom(resp, existing) {
      const jar = {};
      (existing || '').split(';').forEach((p) => {
        const i = p.indexOf('='); if (i > 0) jar[p.slice(0, i).trim()] = p.slice(i + 1).trim();
      });
      ((resp && resp.headers) || []).forEach(([name, value]) => {
        if (String(name).toLowerCase() !== 'set-cookie') return;
        const first = String(value).split(';')[0];
        const i = first.indexOf('='); if (i > 0) jar[first.slice(0, i).trim()] = first.slice(i + 1).trim();
      });
      return Object.keys(jar).map((k) => k + '=' + jar[k]).join('; ');
    },
    crypto(op, fields) { return this._post('/api/crypto', Object.assign({ op }, fields || {})); },
    async writeFile(plugin, path, dataB64) {
      const binary = atob(dataB64 || '');
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      const url = '/api/plugin-fs?plugin=' + encodeURIComponent(plugin) +
        '&path=' + encodeURIComponent(path);
      const r = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: bytes
      });
      if (!r.ok) {
        let detail = '';
        try { detail = (await r.json()).error || ''; } catch (e) {}
        throw new Error('/api/plugin-fs ' + r.status + (detail ? ': ' + detail : ''));
      }
      return r.json();
    },
    async fetchToSd(plugin, url, dest, headers) {
      // Keep each browser-facing request short. Mobile browsers may terminate
      // a request after a few minutes even when response heartbeats are flowing,
      // while the device itself is still downloading successfully.
      const maxBytes = 2 * 1024 * 1024;
      let offset = 0;
      for (;;) {
        const result = await this._post('/api/fetch', {
          plugin, url, dest, headers: headers || {}, offset, maxBytes
        });
        if (result.complete !== false) return result;
        if (!Number.isFinite(result.bytes) || result.bytes <= offset) {
          throw new Error('/api/fetch made no progress');
        }
        offset = result.bytes;
      }
    }
  };

  async function loadPlugins(mount) {
    const host = document.getElementById('plugin-container');
    if (!host) return;
    let list;
    try { list = await (await fetch('/api/plugins')).json(); } catch (e) { return; }
    for (const p of list) {
      // A null mount loads everything: the headless runner page hosts all
      // plugins so any registered action can execute there.
      if (mount && (p.mount || 'settings') !== mount) continue;
      const container = document.createElement('div');
      container.className = 'card';
      container.id = 'plugin-' + p.name;
      host.appendChild(container);
      CrossPoint._pending = null;
      try {
        await new Promise((resolve, reject) => {
          const s = document.createElement('script');
          s.src = '/plugin?name=' + encodeURIComponent(p.name) + '&file=plugin.js';
          s.onload = resolve; s.onerror = () => reject(new Error('load ' + p.name));
          document.body.appendChild(s);
        });
        if (typeof CrossPoint._pending === 'function') {
          CrossPoint._pending(container, {
            name: p.name,
            relay: (m, u, h, b) => CrossPoint.relay(p.name, m, u, h, b),
            cookiesFrom: (resp, existing) => CrossPoint.cookiesFrom(resp, existing),
            crypto: (op, f) => CrossPoint.crypto(op, f),
            writeFile: (path, d) => CrossPoint.writeFile(p.name, path, d),
            fetchToSd: (u, dest, h) => CrossPoint.fetchToSd(p.name, u, dest, h),
            registerAction: (a, fn) => CrossPoint.registerAction(p.name, a, fn),
            pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) + '&file=' + encodeURIComponent(f)
          });
        }
      } catch (e) {
        container.innerHTML = '<h2>' + (p.title || p.name) + '</h2>' +
          '<p style="color:#c0392b">Failed to load plugin.</p>';
      }
    }
  }
