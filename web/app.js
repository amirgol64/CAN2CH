/* CAN gateway web UI. Plain JavaScript, no build step. */
(function () {
  'use strict';

  var BITRATES = [25, 50, 100, 125, 250, 500, 800, 1000];
  var $ = function (sel, el) { return (el || document).querySelector(sel); };
  var $$ = function (sel, el) { return Array.prototype.slice.call((el || document).querySelectorAll(sel)); };

  var state = {
    status: null,        // last /api/status
    live: null,          // last WebSocket stats message
    prevLive: null,
    prevLiveAt: 0,
    filters: new Map(),  // "s386" -> {id, ext}
    filterGen: -1,
    rows: new Map(),     // "1s386" -> row object
    order: [],           // row keys in display order
    dbc: { 1: null, 2: null },
    canWrite: true,
    paused: false,
    txTimer: null
  };

  /* ---------------- small helpers ---------------- */

  function hexId(id, ext) {
    var s = id.toString(16).toUpperCase();
    return '0x' + (ext ? ('00000000' + s).slice(-8) : ('000' + s).slice(-3));
  }
  function fkey(id, ext) { return (ext ? 'x' : 's') + id; }
  function num(n) { return Number(n).toLocaleString('en-US'); }
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function parseHexId(text) {
    var t = String(text).trim().replace(/^0x/i, '');
    if (!/^[0-9a-f]{1,8}$/i.test(t)) return null;
    var v = parseInt(t, 16);
    return v <= 0x1FFFFFFF ? v : null;
  }
  function uptime(ms) {
    var s = Math.floor(ms / 1000);
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    if (d) return d + ' d ' + h + ' h';
    if (h) return h + ' h ' + m + ' min';
    return m + ' min ' + (s % 60) + ' s';
  }

  var toastTimer;
  function toast(msg, isError) {
    var t = $('#toast');
    t.textContent = msg;
    t.className = isError ? 'err' : '';
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, isError ? 5000 : 2500);
  }

  /* ---------------- API ---------------- */

  function api(method, path, body, opts) {
    var init = { method: method, headers: {}, credentials: 'same-origin' };
    if (method !== 'GET') init.headers['X-CANGW'] = '1';
    if (body !== undefined && body !== null) {
      if (opts && opts.raw) {
        init.body = body;
      } else {
        init.body = JSON.stringify(body);
        init.headers['Content-Type'] = 'application/json';
      }
    }
    return fetch(path, init).then(function (res) {
      if (res.status === 401 && !(opts && opts.noLogin)) {
        setWritable(false);
        showLogin();
      }
      if (opts && opts.text) {
        if (!res.ok) throw new Error(res.status === 404 ? 'not found' : 'HTTP ' + res.status);
        return res.text();
      }
      return res.json().catch(function () { return {}; }).then(function (data) {
        if (!res.ok) throw new Error(data.error || ('HTTP ' + res.status));
        return data;
      });
    });
  }
  function run(promise, okMsg) {
    return promise.then(function (r) { if (okMsg) toast(okMsg); return r; })
      .catch(function (e) { toast(e.message, true); throw e; });
  }

  /* ---------------- read-only mode and login ---------------- */

  function setWritable(can) {
    state.canWrite = can;
    $$('.w').forEach(function (e) { e.disabled = !can; });
    var required = state.status && state.status.auth.required;
    $('#login-btn').hidden = can || !required;
    $('#logout-btn').hidden = !(can && required);
    renderAllRows();
  }
  function showLogin() {
    var dlg = $('#login-dialog');
    if (dlg.open) return;
    $('#login-error').textContent = '';
    $('#login-pass').value = '';
    dlg.showModal();
  }
  $('#login-btn').onclick = showLogin;
  $('#login-cancel').onclick = function () { $('#login-dialog').close(); };
  $('#login-form').onsubmit = function (ev) {
    ev.preventDefault();
    api('POST', '/api/login', { password: $('#login-pass').value }, { noLogin: true }).then(function () {
      $('#login-dialog').close();
      toast('Logged in');
      loadStatus();
    }).catch(function (e) { $('#login-error').textContent = e.message; });
  };
  $('#logout-btn').onclick = function () {
    run(api('POST', '/api/logout'), 'Logged out').then(loadStatus);
  };

  /* ---------------- tabs ---------------- */

  $$('#tabs button').forEach(function (btn) {
    btn.onclick = function () {
      $$('#tabs button').forEach(function (b) { b.classList.toggle('active', b === btn); });
      $$('.tab').forEach(function (t) { t.classList.toggle('active', t.id === 'tab-' + btn.dataset.tab); });
    };
  });

  /* ---------------- status ---------------- */

  function loadStatus() {
    return api('GET', '/api/status').then(function (s) {
      state.status = s;
      setWritable(!s.auth.required || s.auth.authed);
      $('#wifi-info').textContent = s.wifi.mode === 'ap'
        ? (s.wifi.sta ? 'Cannot reach "' + s.wifi.sta + '" (still retrying). Reachable through access point "' + s.wifi.ssid + '" at ' + s.wifi.ip + '.'
                      : 'No network configured. Access point "' + s.wifi.ssid + '" at ' + s.wifi.ip + '.')
        : (s.wifi.connected ? 'Connected to "' + s.wifi.ssid + '" at ' + s.wifi.ip + ' (' + s.wifi.rssi + ' dBm)'
                            : 'Connecting to "' + s.wifi.ssid + '"');
      /* Fill the field once; never overwrite what someone is entering
       * (status is reloaded on every reconnect). */
      if (!$('#wifi-ssid').dataset.dirty) $('#wifi-ssid').value = s.wifi.sta || '';
      $('#auth-info').textContent = s.auth.required
        ? 'A password is set. Without logging in, this page is read-only.'
        : 'No password is set: anyone on the network can change settings.';
      $('#serial-no').textContent = s.serial || '-';
      $('#fw-info').textContent = 'Running version ' + s.fw + ' from partition ' + s.partition +
        '. Last restart: ' + s.reset + '. Free RAM ' + num(s.heap) + ' bytes (lowest ' + num(s.minheap) + ').';
      $('#set-br1').value = s.b1.br;
      $('#set-lo1').checked = s.b1.lo;
      $('#set-br2').value = s.b2.br;
      applyLive(s);
      return s;
    }).catch(function () {});
  }

  function pillClass(st) {
    if (st === 'running' || st === 'active') return 'pill ok';
    if (st === 'warning' || st === 'passive' || st === 'recovering') return 'pill warn';
    return 'pill bad';
  }
  function setField(card, name, text) {
    var e = $('[data-f="' + name + '"]', card);
    if (e && e.textContent !== text) e.textContent = text;
  }
  function perSec(cur, prev, dt) {
    if (prev === undefined || dt <= 0) return '';
    var d = (cur - prev) >>> 0;
    return ' (' + num(Math.round(d * 1000 / dt)) + '/s)';
  }

  function applyLive(s) {
    var now = performance.now();
    var p = state.prevLive, dt = now - state.prevLiveAt;
    var pb1 = p ? p.b1 : {}, pb2 = p ? p.b2 : {}, pg = p ? p.gw : {};
    state.live = s;

    var c1 = $('#card-b1'), c2 = $('#card-b2'), cg = $('#card-gw');
    var st1 = $('[data-f="st"]', c1), st2 = $('[data-f="st"]', c2), stg = $('[data-f="st"]', cg);
    st1.textContent = s.b1.st + (s.b1.lo ? ', listen-only' : '');
    st1.className = pillClass(s.b1.st);
    st2.textContent = s.b2.st;
    st2.className = pillClass(s.b2.st);
    stg.textContent = s.gw.on ? 'forwarding' : 'stopped';
    stg.className = s.gw.on ? 'pill ok' : 'pill bad';

    setField(c1, 'br', s.b1.br + ' kbit/s');
    setField(c1, 'load', (s.b1.load / 10).toFixed(1) + ' %');
    setField(c1, 'rx', num(s.b1.rx) + perSec(s.b1.rx, pb1.rx, dt));
    setField(c1, 'tx', num(s.b1.tx));
    setField(c1, 'err', num(s.b1.err) + ' (TEC ' + s.b1.tec + ', REC ' + s.b1.rec + ')');
    setField(c1, 'miss', num(s.b1.miss) + ' (queue peak ' + s.b1.qpk + '/' + s.b1.qsz + ')');
    var bar1 = $('[data-f="loadbar"]', c1);
    bar1.style.width = (s.b1.load / 10) + '%';
    bar1.className = s.b1.load > 800 ? 'high' : '';

    setField(c2, 'br', s.b2.br + ' kbit/s' + (s.b2.xtal ? ', ' + s.b2.xtal + ' MHz crystal' : ''));
    setField(c2, 'load', (s.b2.load / 10).toFixed(1) + ' %');
    setField(c2, 'tx', num(s.b2.tx) + perSec(s.b2.tx, pb2.tx, dt));
    setField(c2, 'rx', num(s.b2.rx) + perSec(s.b2.rx, pb2.rx, dt));
    setField(c2, 'fail', num(s.b2.fail));
    setField(c2, 'err', 'TEC ' + s.b2.tec + ', REC ' + s.b2.rec + (s.b2.boff ? ', bus-off ' + s.b2.boff + 'x' : ''));
    var bar2 = $('[data-f="loadbar"]', c2);
    bar2.style.width = (s.b2.load / 10) + '%';
    bar2.className = s.b2.load > 800 ? 'high' : '';

    setField(cg, 'fwd', num(s.gw.fwd) + perSec(s.gw.fwd, pg.fwd, dt));
    setField(cg, 'blk', num(s.gw.blk) + perSec(s.gw.blk, pg.blk, dt));
    setField(cg, 'drop', num(s.gw.drop));
    setField(cg, 'q', s.gw.q + ' / ' + s.gw.qsz + ' (peak ' + s.gw.qpk + ')');
    setField(cg, 'nfilt', state.filters.size + ' IDs');
    setField(cg, 'up', uptime(s.up));
    var qbar = $('[data-f="qbar"]', cg);
    qbar.style.width = (s.gw.q * 100 / s.gw.qsz) + '%';
    qbar.className = s.gw.q > s.gw.qsz / 2 ? 'high' : '';

    var sw = $('#fwd-switch');
    if (sw.checked !== s.gw.on) { sw.checked = s.gw.on; renderAllRows(); }

    var warn = [];
    if (s.b2.st === 'absent') warn.push('The CAN2 controller (XL2515) is not responding.');
    if (!s.gw.on) warn.push('Forwarding is switched off.');
    if (s.b1.untracked || s.b2.untracked) warn.push('More than 1024 different IDs on a bus: the extra ones are forwarded but not listed.');
    $('#banner').hidden = warn.length === 0;
    $('#banner').textContent = warn.join(' ');

    if (s.gw.fg !== state.filterGen) {
      state.filterGen = s.gw.fg;
      loadFilters();
    }
    state.prevLive = s;
    state.prevLiveAt = now;
  }

  $('#fwd-switch').onchange = function () {
    var on = this.checked;
    run(api('PUT', '/api/forwarding', { enabled: on }), on ? 'Forwarding on' : 'Forwarding off')
      .catch(function () { $('#fwd-switch').checked = !on; });
  };

  /* ---------------- WebSocket ---------------- */

  function connect() {
    var ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    var conn = $('#conn');
    ws.onopen = function () { conn.textContent = 'live'; conn.className = 'pill ok'; loadStatus(); };
    ws.onclose = function () {
      conn.textContent = 'offline'; conn.className = 'pill bad';
      setTimeout(connect, 2000);
    };
    ws.onmessage = function (ev) {
      var msg;
      try { msg = JSON.parse(ev.data); } catch (e) { return; }
      if (msg.t === 's') applyLive(msg);
      else if (msg.t === 'i') applyRows(msg.e);
      else if (msg.t === 'clr') clearRows();
    };
  }

  /* ---------------- monitor table ---------------- */

  var tbody = $('#mon-table tbody');

  function rowKey(bus, id, ext) { return bus + (ext ? 'x' : 's') + id; }
  function rowSort(a, b) { return a.bus - b.bus || a.ext - b.ext || a.id - b.id; }

  function clearRows() {
    state.rows.clear();
    state.order = [];
    tbody.textContent = '';
    updateCount();
  }

  function createRow(bus, id, ext) {
    var r = { bus: bus, id: id, ext: ext, count: 0, dlc: 0, rtr: 0, data: '', rate: null, at: 0, open: false, sigTr: null };
    var tr = el('tr', 'frame');
    var tdBlock = el('td');
    if (bus === 1) {
      r.cb = el('input');
      r.cb.type = 'checkbox';
      r.cb.title = 'Block this ID';
      r.cb.onclick = function (ev) { ev.stopPropagation(); };
      r.cb.onchange = function () { toggleFilter(id, !!ext, r.cb.checked); };
      tdBlock.appendChild(r.cb);
    }
    tr.appendChild(tdBlock);
    tr.appendChild(el('td', '', 'CAN' + bus));
    tr.appendChild(el('td', 'id', hexId(id, ext)));
    r.tdName = tr.appendChild(el('td'));
    r.tdDlc = tr.appendChild(el('td'));
    r.tdData = tr.appendChild(el('td', 'data'));
    r.tdCount = tr.appendChild(el('td', 'num'));
    r.tdRate = tr.appendChild(el('td', 'num'));
    r.tdStatus = tr.appendChild(el('td'));
    r.pill = r.tdStatus.appendChild(el('span', 'pill'));
    tr.onclick = function () { r.open = !r.open; renderRow(r); };
    r.tr = tr;

    /* Insert in sorted position. */
    var lo = 0, hi = state.order.length;
    while (lo < hi) {
      var mid = (lo + hi) >> 1;
      if (rowSort(state.rows.get(state.order[mid]), r) < 0) lo = mid + 1; else hi = mid;
    }
    var key = rowKey(bus, id, ext);
    var before = lo < state.order.length ? state.rows.get(state.order[lo]).tr : null;
    state.order.splice(lo, 0, key);
    state.rows.set(key, r);
    tbody.insertBefore(tr, before);
    return r;
  }

  function applyRows(entries) {
    var now = performance.now();
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      var r = state.rows.get(rowKey(e[0], e[1], e[2])) || createRow(e[0], e[1], e[2]);
      var dc = e[3] - r.count;
      if (r.at && dc > 0 && now > r.at) {
        var inst = dc * 1000 / (now - r.at);
        r.rate = r.rate === null ? inst : r.rate * 0.5 + inst * 0.5;
      } else if (!r.at && e[7] > 2000) {
        r.rate = 0;
      }
      r.count = e[3]; r.dlc = e[4]; r.rtr = e[5]; r.data = e[6];
      r.at = now - Math.min(e[7], 60000);
      if (!state.paused) renderRow(r);
    }
    updateCount();
  }

  function dataBytes(hex) {
    var out = new Uint8Array(hex.length / 2);
    for (var i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
    return out;
  }

  function matchesView(r) {
    var bus = Number($('#mon-bus').value);
    if (bus && r.bus !== bus) return false;
    var q = $('#mon-search').value.trim().toLowerCase();
    if (!q) return true;
    var msg = Dbc.lookup(state.dbc[r.bus], r.id, r.ext);
    return hexId(r.id, r.ext).toLowerCase().indexOf(q.replace(/^0x/, '')) >= 0 ||
           (msg && msg.name.toLowerCase().indexOf(q) >= 0);
  }

  function renderRow(r) {
    var visible = matchesView(r);
    r.tr.hidden = !visible;
    if (r.sigTr) r.sigTr.hidden = !visible || !r.open;
    if (!visible) return;

    var msg = Dbc.lookup(state.dbc[r.bus], r.id, r.ext);
    r.tdName.textContent = msg ? msg.name : '';
    r.tdDlc.textContent = r.dlc;
    r.tdData.textContent = r.rtr ? 'remote frame' : (r.data.match(/../g) || []).join(' ');
    r.tdCount.textContent = num(r.count);
    var stale = performance.now() - r.at > 2500;
    r.tdRate.textContent = stale ? '0 /s' : (r.rate === null ? '' : (r.rate >= 10 ? r.rate.toFixed(0) : r.rate.toFixed(1)) + ' /s');
    r.tr.classList.toggle('stale', stale);

    if (r.bus === 1) {
      var blocked = state.filters.has(fkey(r.id, r.ext));
      var on = !state.live || state.live.gw.on;
      r.cb.checked = blocked;
      r.cb.disabled = !state.canWrite;
      r.pill.textContent = blocked ? 'blocked' : (on ? 'forwarded' : 'not forwarded');
      r.pill.className = blocked ? 'pill bad' : (on ? 'pill ok' : 'pill warn');
    } else {
      r.pill.textContent = 'monitor only';
      r.pill.className = 'pill';
    }

    if (r.open) {
      if (!r.sigTr) {
        r.sigTr = el('tr', 'signals');
        r.sigTd = r.sigTr.appendChild(el('td'));
        r.sigTd.colSpan = 9;
        r.tr.parentNode.insertBefore(r.sigTr, r.tr.nextSibling);
      }
      r.sigTr.hidden = false;
      r.sigTd.textContent = '';
      if (!msg) {
        r.sigTd.textContent = 'No DBC message for this ID. Upload a DBC file for CAN' + r.bus + ' to decode signals.';
      } else if (r.rtr) {
        r.sigTd.textContent = msg.name + ': remote frame, no data.';
      } else {
        var sigs = Dbc.decode(msg, dataBytes(r.data));
        if (!sigs.length) r.sigTd.textContent = msg.name + ': no signals fit in this frame.';
        sigs.forEach(function (s) {
          var span = el('span', 'sig');
          span.appendChild(el('b', '', s.name));
          span.appendChild(document.createTextNode(' ' + (s.text !== null ? s.text + ' (' + s.value + ')' : s.value) + (s.unit ? ' ' + s.unit : '')));
          r.sigTd.appendChild(span);
        });
      }
    } else if (r.sigTr) {
      r.sigTr.hidden = true;
    }
  }

  function renderAllRows() {
    state.rows.forEach(renderRow);
    updateCount();
  }
  function updateCount() {
    var shown = 0;
    state.rows.forEach(function (r) { if (!r.tr.hidden) shown++; });
    $('#mon-count').textContent = shown + ' of ' + state.rows.size + ' IDs';
    $('#mon-empty').hidden = shown > 0;
    $('#mon-empty').textContent = state.rows.size ? 'No ID matches the current view.' : 'No frames seen yet.';
  }

  $('#mon-bus').onchange = renderAllRows;
  $('#mon-search').oninput = renderAllRows;
  $('#mon-pause').onchange = function () { state.paused = this.checked; if (!state.paused) renderAllRows(); };
  $('#mon-clear').onclick = function () { run(api('POST', '/api/stats/clear'), 'Counters cleared'); };
  setInterval(function () {
    if (state.paused) return;
    var now = performance.now();
    state.rows.forEach(function (r) {
      if (now - r.at > 2500 && !r.tr.classList.contains('stale')) renderRow(r);
    });
  }, 1000);

  /* ---------------- filters ---------------- */

  function loadFilters() {
    return api('GET', '/api/filters').then(function (list) {
      state.filters = new Map(list.map(function (f) { return [fkey(f.id, f.ext), f]; }));
      renderFilters();
      renderAllRows();
    }).catch(function () {});
  }

  function toggleFilter(id, ext, block) {
    var p = api(block ? 'POST' : 'DELETE', '/api/filters', { id: id, ext: ext });
    return run(p, (block ? 'Blocked ' : 'Unblocked ') + hexId(id, ext)).then(loadFilters, loadFilters);
  }

  function renderFilters() {
    var body = $('#flt-table tbody');
    body.textContent = '';
    state.filters.forEach(function (f) {
      var tr = el('tr');
      tr.appendChild(el('td', 'id', hexId(f.id, f.ext)));
      tr.appendChild(el('td', '', f.ext ? 'Extended' : 'Standard'));
      var msg = Dbc.lookup(state.dbc[1], f.id, f.ext);
      tr.appendChild(el('td', '', msg ? msg.name : ''));
      var td = tr.appendChild(el('td'));
      var btn = td.appendChild(el('button', 'ghost w', 'Remove'));
      btn.disabled = !state.canWrite;
      btn.onclick = function () { toggleFilter(f.id, f.ext, false); };
      body.appendChild(tr);
    });
    $('#flt-empty').hidden = state.filters.size > 0;
    $('#flt-table').hidden = state.filters.size === 0;
    renderDbcSearch();
  }

  $('#flt-add').onsubmit = function (ev) {
    ev.preventDefault();
    var id = parseHexId($('#flt-id').value);
    var ext = $('#flt-ext').value === '1';
    if (id === null || (!ext && id > 0x7FF)) {
      toast(ext ? 'Enter a hex ID up to 1FFFFFFF' : 'Enter a hex ID up to 7FF, or choose Extended', true);
      return;
    }
    toggleFilter(id, ext, true).then(function () { $('#flt-id').value = ''; });
  };
  $('#flt-clear').onclick = function () {
    if (!confirm('Remove every ID from the blocklist?')) return;
    run(api('DELETE', '/api/filters', { all: true }), 'Blocklist cleared').then(loadFilters);
  };

  function renderDbcSearch() {
    var list = $('#flt-dbc-results');
    var q = $('#flt-dbc-search').value.trim().toLowerCase();
    list.textContent = '';
    $('#flt-dbc-search').disabled = !state.dbc[1];
    $('#flt-dbc-search').placeholder = state.dbc[1] ? 'Search CAN1 DBC by message name' : 'Upload a CAN1 DBC to search by message name';
    if (!q || !state.dbc[1]) return;
    var shown = 0;
    state.dbc[1].messages.forEach(function (msg) {
      if (shown >= 12 || msg.name.toLowerCase().indexOf(q) < 0) return;
      shown++;
      var li = el('li');
      li.appendChild(el('span', '', msg.name + '  ' + hexId(msg.id, msg.ext)));
      var blocked = state.filters.has(fkey(msg.id, msg.ext));
      var btn = li.appendChild(el('button', blocked ? 'ghost w' : 'primary w', blocked ? 'Unblock' : 'Block'));
      btn.disabled = !state.canWrite;
      btn.onclick = function () { toggleFilter(msg.id, msg.ext, !blocked); };
      list.appendChild(li);
    });
    if (!shown) list.appendChild(el('li', 'muted', 'No message name contains "' + q + '".'));
  }
  $('#flt-dbc-search').oninput = renderDbcSearch;

  /* ---------------- DBC ---------------- */

  function loadDbc(bus) {
    var info = $('.panel[data-bus="' + bus + '"] .dbc-info');
    return api('GET', '/api/dbc/' + bus, null, { text: true }).then(function (text) {
      var db = Dbc.parse(text);
      state.dbc[bus] = db;
      var signals = 0;
      db.messages.forEach(function (m) { signals += m.signals.length; });
      info.textContent = db.messages.size + ' messages, ' + signals + ' signals (' + num(text.length) + ' bytes).';
    }).catch(function () {
      state.dbc[bus] = null;
      info.textContent = 'No file loaded.';
    }).then(function () { renderFilters(); renderAllRows(); });
  }

  $$('#tab-dbc .panel').forEach(function (panel) {
    var bus = Number(panel.dataset.bus);
    $('.dbc-upload', panel).onclick = function () {
      var file = $('.dbc-file', panel).files[0];
      if (!file) { toast('Choose a .dbc file first', true); return; }
      file.text().then(function (text) {
        if (!Dbc.parse(text).messages.size) throw new Error('No messages found: is this a DBC file?');
        return api('PUT', '/api/dbc/' + bus, text, { raw: true });
      }).then(function () {
        toast('DBC stored for CAN' + bus);
        $('.dbc-file', panel).value = '';
        return loadDbc(bus);
      }).catch(function (e) { toast(e.message, true); });
    };
    $('.dbc-delete', panel).onclick = function () {
      if (!confirm('Delete the DBC file for CAN' + bus + '?')) return;
      run(api('DELETE', '/api/dbc/' + bus), 'DBC deleted').then(function () { return loadDbc(bus); });
    };
  });

  /* ---------------- send ---------------- */

  function sendFrame() {
    var ext = $('#tx-ext').checked;
    var id = parseHexId($('#tx-id').value);
    if (id === null || (!ext && id > 0x7FF)) return Promise.reject(new Error('Invalid ID for ' + (ext ? 'an extended' : 'a standard') + ' frame'));
    return api('POST', '/api/tx', {
      bus: Number($('#tx-bus').value), id: id, ext: ext, rtr: $('#tx-rtr').checked, data: $('#tx-data').value
    });
  }
  function stopRepeat() {
    clearInterval(state.txTimer);
    state.txTimer = null;
    $('#tx-stop').hidden = true;
  }
  $('#tx-form').onsubmit = function (ev) {
    ev.preventDefault();
    stopRepeat();
    var period = Number($('#tx-period').value) || 0;
    var sent = 0;
    var once = function () {
      return sendFrame().then(function () {
        sent++;
        $('#tx-result').textContent = sent + ' sent';
      }).catch(function (e) {
        stopRepeat();
        $('#tx-result').textContent = '';
        toast(e.message, true);
      });
    };
    once();
    if (period > 0) {
      state.txTimer = setInterval(once, Math.max(period, 20));
      $('#tx-stop').hidden = false;
    }
  };
  $('#tx-stop').onclick = stopRepeat;

  /* ---------------- settings ---------------- */

  ['#set-br1', '#set-br2'].forEach(function (sel) {
    BITRATES.forEach(function (b) {
      var o = el('option', '', b + ' kbit/s');
      o.value = b;
      $(sel).appendChild(o);
    });
  });

  $('#bus-form').onsubmit = function (ev) {
    ev.preventDefault();
    var p = api('PUT', '/api/bus/1', { bitrate: Number($('#set-br1').value), listen_only: $('#set-lo1').checked })
      .then(function () {
        return api('PUT', '/api/bus/2', { bitrate: Number($('#set-br2').value), xtal_mhz: Number($('#set-xtal').value) });
      });
    run(p, 'Bus settings applied').then(loadStatus);
  };

  $('#wifi-ssid').oninput = function () { this.dataset.dirty = '1'; };
  $('#wifi-form').onsubmit = function (ev) {
    ev.preventDefault();
    var ssid = $('#wifi-ssid').value;
    if (!confirm(ssid ? 'Save and reboot to join "' + ssid + '"?' : 'Clear the Wi-Fi credentials and reboot into access point mode?')) return;
    run(api('PUT', '/api/wifi', { ssid: ssid, password: $('#wifi-pass').value }), 'Saved. The gateway is rebooting.');
  };

  $('#appw-form').onsubmit = function (ev) {
    ev.preventDefault();
    var pw = $('#appw-new').value;
    if (pw.length < 8 || pw.length > 63) { toast('The password must be 8 to 63 characters', true); return; }
    if (!confirm('Save the access point password and reboot? Devices joined to the access point will have to reconnect with the new password.')) return;
    run(api('PUT', '/api/ap-password', { password: pw }), 'Saved. The gateway is rebooting.').then(function () {
      $('#appw-new').value = '';
    });
  };

  $('#pw-form').onsubmit = function (ev) {
    ev.preventDefault();
    var pw = $('#pw-new').value;
    run(api('PUT', '/api/password', { password: pw }), pw ? 'Password set' : 'Password removed').then(function () {
      $('#pw-new').value = '';
      loadStatus();
    });
  };

  $('#cfg-import').onclick = function () {
    var file = $('#cfg-file').files[0];
    if (!file) { toast('Choose a configuration file first', true); return; }
    file.text().then(function (text) {
      JSON.parse(text);
      return api('PUT', '/api/config', text, { raw: true });
    }).then(function () {
      toast('Configuration imported');
      $('#cfg-file').value = '';
      loadStatus();
      loadConfig();
      loadFilters();
    }).catch(function (e) { toast(e.message, true); });
  };

  $('#ota-upload').onclick = function () {
    var file = $('#ota-file').files[0];
    if (!file) { toast('Choose a firmware .bin file first', true); return; }
    if (!confirm('Upload ' + file.name + ' and reboot into it?')) return;
    var bar = $('#ota-progress');
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.setRequestHeader('X-CANGW', '1');
    xhr.upload.onprogress = function (e) { if (e.lengthComputable) bar.value = e.loaded * 100 / e.total; };
    xhr.onload = function () {
      bar.hidden = true;
      var data = {};
      try { data = JSON.parse(xhr.responseText); } catch (e) { /* ignore */ }
      if (xhr.status === 200) toast('Firmware stored. The gateway is rebooting.');
      else toast(data.error || 'Upload failed (HTTP ' + xhr.status + ')', true);
    };
    xhr.onerror = function () { bar.hidden = true; toast('Upload failed: connection lost', true); };
    bar.value = 0;
    bar.hidden = false;
    xhr.send(file);
  };

  $('#reboot-btn').onclick = function () {
    if (confirm('Reboot the gateway?')) run(api('POST', '/api/reboot'), 'Rebooting');
  };
  $('#factory-btn').onclick = function () {
    if (confirm('Erase all settings (bitrates, blocklist, Wi-Fi, password) and reboot?')) {
      run(api('POST', '/api/factory-reset'), 'Settings erased. Rebooting.');
    }
  };

  function loadConfig() {
    return api('GET', '/api/config').then(function (c) {
      $('#set-xtal').value = c.bus2.xtal_mhz;
    }).catch(function () {});
  }

  /* ---------------- start ---------------- */

  loadStatus().then(function () {
    loadConfig();
    loadFilters();
    loadDbc(1);
    loadDbc(2);
  });
  connect();
})();
