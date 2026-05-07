// Jarvis Setup -- captive portal app
//
// Single-file SPA. Renders the schema-driven config UI defined by
// data/web/index.html + style.css against the firmware's REST API
// (see src/net/CaptivePortal.cpp). Class conventions match style.css
// exactly:
//   - Form field wrapper:  <div class="row">
//                            <label>...</label>
//                            <div class="control">...</div>
//                          </div>
//   - Slider value badge:  <span class="slider-value">…</span>
//   - Bool toggle:         <div class="toggle [on]"> (JS toggles .on)
//   - Stat tile:           <div class="stat">
//                            <div class="label">…</div>
//                            <div class="value">…</div>
//                            <div class="sub">…</div>          (optional)
//                          </div>
//   - WiFi list item:      <div class="wifi-item">
//                            <div>
//                              <div class="wifi-name">…</div>
//                              <div class="wifi-meta">…</div>
//                            </div>
//                            <button class="btn ghost mini">…</button>
//                          </div>
//   - Modal / Toast / Savebar overlays toggle the `.visible` class
//     (NOT `.show` — that class is unstyled in this CSS).

'use strict';

const API = {
  config: '/api/config',
  status: '/api/status',
  scan:   '/api/wifi/scan',
  saved:  '/api/wifi/saved',
  add:    '/api/wifi/add',
  remove: '/api/wifi/remove',
  exit:   '/api/exit',
};

// ============================================================================
// State
// ============================================================================

const state = {
  schema: [],          // GET /api/config -> body.fields
  values: {},          // current saved values (mirrors server)
  pending: {},         // unsaved edits keyed by field key
  scanInflight: false,
};

// ============================================================================
// Tiny helpers
// ============================================================================

function $(sel, root) { return (root || document).querySelector(sel); }
function $$(sel, root) { return Array.from((root || document).querySelectorAll(sel)); }

function el(tag, attrs, children) {
  const e = document.createElement(tag);
  if (attrs) {
    for (const k in attrs) {
      if (k === 'class') e.className = attrs[k];
      else if (k === 'text') e.textContent = attrs[k];
      else if (k.startsWith('on') && typeof attrs[k] === 'function') {
        e.addEventListener(k.slice(2), attrs[k]);
      } else if (attrs[k] !== false && attrs[k] !== null && attrs[k] !== undefined) {
        e.setAttribute(k, attrs[k]);
      }
    }
  }
  if (children) {
    for (const c of [].concat(children)) {
      if (c == null) continue;
      e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
    }
  }
  return e;
}

function fmtUptime(ms) {
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const ss = s % 60;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${ss}s`;
  return `${ss}s`;
}

function fmtBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(2)} MB`;
}

// Toast — uses .visible (matches style.css).
let toastTimer = null;
function toast(msg, kind) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast' + (kind === 'error' ? ' error' : '') + ' visible';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.classList.remove('visible'); }, 2400);
}

// Fetch wrappers
async function getJson(url) {
  const r = await fetch(url, { cache: 'no-store' });
  if (!r.ok && r.status !== 202) throw new Error(`${url} -> ${r.status}`);
  return { status: r.status, body: r.status === 204 ? null : await r.json() };
}
async function postJson(url, body) {
  const r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : '',
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) {
    const e = new Error(data.error || `${url} -> ${r.status}`);
    e.status = r.status;
    throw e;
  }
  return data;
}

// ============================================================================
// Tabs
// ============================================================================

function initTabs() {
  $$('.tab').forEach(tab => {
    tab.addEventListener('click', () => {
      const name = tab.dataset.tab;
      $$('.tab').forEach(t => t.classList.toggle('active', t === tab));
      $$('.panel').forEach(p => p.classList.toggle('active', p.dataset.panel === name));
      if (name === 'status') refreshStatus();
      if (name === 'wifi')   loadSaved();
    });
  });
}

// ============================================================================
// Schema-driven config panels
// ============================================================================

async function loadConfig() {
  try {
    const { body } = await getJson(API.config);
    state.schema = body.fields || [];
    state.values = {};
    state.pending = {};
    for (const f of state.schema) state.values[f.key] = f.value;
    renderConfigPanels();
    updateSaveBar();
  } catch (err) {
    toast('Failed to load config', 'error');
    console.error(err);
  }
}

function renderConfigPanels() {
  const byCat = {};
  for (const f of state.schema) {
    (byCat[f.category] = byCat[f.category] || []).push(f);
  }
  for (const cat in byCat) {
    const host = $('#panel-' + cat);
    if (!host) continue;
    host.innerHTML = '';
    for (const f of byCat[cat]) host.appendChild(renderField(f));
  }
}

function renderField(f) {
  const row = el('div', { class: 'row', 'data-key': f.key });
  row.appendChild(el('label', { text: f.label }));
  const control = el('div', { class: 'control' });
  row.appendChild(control);

  switch (f.type) {
    case 'bool': {
      const toggle = el('div', {
        class: 'toggle' + (f.value ? ' on' : ''),
        role: 'switch',
        'aria-checked': f.value ? 'true' : 'false',
        tabindex: '0',
      });
      const setOn = (on) => {
        toggle.classList.toggle('on', on);
        toggle.setAttribute('aria-checked', on ? 'true' : 'false');
        onFieldChange(f, on);
      };
      toggle.addEventListener('click', () => setOn(!toggle.classList.contains('on')));
      toggle.addEventListener('keydown', (e) => {
        if (e.key === ' ' || e.key === 'Enter') {
          e.preventDefault();
          setOn(!toggle.classList.contains('on'));
        }
      });
      control.appendChild(toggle);
      break;
    }
    case 'int': {
      const input = el('input', {
        type: 'range',
        min: f.min, max: f.max, step: 1, value: f.value,
      });
      const value = el('span', { class: 'slider-value', text: String(f.value) });
      input.addEventListener('input', () => {
        value.textContent = input.value;
        onFieldChange(f, parseInt(input.value, 10));
      });
      control.appendChild(input);
      control.appendChild(value);
      break;
    }
    case 'string': {
      const input = el('input', {
        type: f.sensitive ? 'password' : 'text',
        value: f.value || '',
        placeholder: f.sensitive ? '(unchanged)' : '',
        autocomplete: 'off',
        autocapitalize: 'off',
        spellcheck: 'false',
      });
      input.addEventListener('input', () => onFieldChange(f, input.value));
      control.appendChild(input);
      break;
    }
    case 'enum': {
      const select = el('select');
      for (const opt of (f.options || [])) {
        const o = el('option', { value: opt.value, text: opt.label });
        if (opt.value === f.value) o.selected = true;
        select.appendChild(o);
      }
      select.addEventListener('change', () => onFieldChange(f, select.value));
      control.appendChild(select);
      break;
    }
    default:
      control.appendChild(el('span', { text: 'unsupported: ' + f.type }));
      break;
  }
  return row;
}

function onFieldChange(f, newValue) {
  const original = state.values[f.key];
  if (f.sensitive && f.type === 'string' && newValue === '********') {
    delete state.pending[f.key];
  } else if (newValue === original) {
    delete state.pending[f.key];
  } else {
    state.pending[f.key] = newValue;
  }
  updateSaveBar();
}

function updateSaveBar() {
  const dirty = Object.keys(state.pending).length;
  const bar = $('#savebar');
  bar.classList.toggle('visible', dirty > 0);
  $('#saveStatus').textContent = dirty
    ? `${dirty} unsaved change${dirty > 1 ? 's' : ''}`
    : '';
}

async function saveConfig() {
  if (!Object.keys(state.pending).length) return;
  const btn = $('#saveBtn');
  btn.disabled = true;
  try {
    const r = await postJson(API.config, state.pending);
    toast(`Saved ${r.updated || 0} field(s)`);
    await loadConfig();
  } catch (err) {
    toast('Save failed: ' + err.message, 'error');
  } finally {
    btn.disabled = false;
  }
}

function discardChanges() {
  state.pending = {};
  loadConfig();
  toast('Changes discarded');
}

// ============================================================================
// Status panel
// ============================================================================

let statusTimer = null;

async function refreshStatus() {
  try {
    const { body } = await getJson(API.status);
    const grid = $('#statusGrid');
    grid.innerHTML = '';
    const cells = [
      { k: 'Mode',       v: body.mode || '—' },
      { k: 'AP SSID',    v: body.ap_ssid || '—' },
      { k: 'Clients',    v: String(body.ap_clients ?? 0) },
      { k: 'Battery',    v: body.battery_pct != null ? body.battery_pct + '%' : '—',
        sub: body.charging ? 'charging' : null },
      { k: 'Uptime',     v: body.uptime_ms != null ? fmtUptime(body.uptime_ms) : '—' },
      { k: 'Free Heap',  v: body.free_heap != null ? fmtBytes(body.free_heap) : '—' },
    ];
    for (const c of cells) {
      const tile = el('div', { class: 'stat' });
      tile.appendChild(el('div', { class: 'label', text: c.k }));
      tile.appendChild(el('div', { class: 'value', text: c.v }));
      if (c.sub) tile.appendChild(el('div', { class: 'sub', text: c.sub }));
      grid.appendChild(tile);
    }
  } catch (err) {
    console.warn('status refresh failed', err);
  }
}

function startStatusPolling() {
  refreshStatus();
  if (statusTimer) clearInterval(statusTimer);
  statusTimer = setInterval(refreshStatus, 5000);
}

// ============================================================================
// WiFi
// ============================================================================

async function loadSaved() {
  const host = $('#savedList');
  host.innerHTML = '<div class="empty">Loading…</div>';
  try {
    const { body } = await getJson(API.saved);
    host.innerHTML = '';
    if (!body || !body.length) {
      host.appendChild(el('div', { class: 'empty', text: 'No saved networks' }));
      return;
    }
    for (const n of body) host.appendChild(renderSaved(n));
  } catch (err) {
    host.innerHTML = '';
    host.appendChild(el('div', { class: 'empty', text: 'Failed to load saved networks' }));
  }
}

function renderSaved(n) {
  const item = el('div', { class: 'wifi-item' });
  const left = el('div');
  left.appendChild(el('div', { class: 'wifi-name', text: n.ssid }));
  left.appendChild(el('div', { class: 'wifi-meta', text: 'priority ' + n.priority }));
  item.appendChild(left);
  item.appendChild(el('button', {
    class: 'btn ghost mini',
    text: 'Remove',
    onclick: (e) => { e.stopPropagation(); removeNetwork(n.ssid); },
  }));
  return item;
}

async function removeNetwork(ssid) {
  try {
    await postJson(API.remove + '?ssid=' + encodeURIComponent(ssid));
    toast('Removed ' + ssid);
    loadSaved();
  } catch (err) {
    toast('Remove failed', 'error');
  }
}

async function startScan() {
  const host = $('#scanList');
  if (state.scanInflight) return;
  state.scanInflight = true;
  host.innerHTML = '<div class="empty">Scanning…</div>';
  try {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {
      const { status, body } = await getJson(API.scan);
      if (status === 200) {
        renderScanResults(host, body || []);
        return;
      }
      await new Promise(r => setTimeout(r, 700));
    }
    host.innerHTML = '<div class="empty">Scan timed out</div>';
  } catch (err) {
    host.innerHTML = '';
    host.appendChild(el('div', { class: 'empty', text: 'Scan failed' }));
  } finally {
    state.scanInflight = false;
  }
}

function renderScanResults(host, list) {
  host.innerHTML = '';
  if (!list.length) {
    host.appendChild(el('div', { class: 'empty', text: 'No networks found' }));
    return;
  }
  list.sort((a, b) => (b.rssi || -100) - (a.rssi || -100));
  const seen = new Set();
  for (const n of list) {
    if (!n.ssid || seen.has(n.ssid)) continue;
    seen.add(n.ssid);
    host.appendChild(renderScan(n));
  }
}

function renderScan(n) {
  const item = el('div', {
    class: 'wifi-item',
    onclick: () => openWifiModal(n),
  });
  const left = el('div');
  left.appendChild(el('div', { class: 'wifi-name', text: n.ssid }));
  const meta = el('div', { class: 'wifi-meta' });
  meta.appendChild(el('span', { text: (n.rssi || 0) + ' dBm' }));
  if (n.secure) meta.appendChild(el('span', { text: 'secured' }));
  left.appendChild(meta);
  item.appendChild(left);
  return item;
}

let modalCtx = null;

function openWifiModal(net) {
  modalCtx = net;
  $('#wifiModalTitle').textContent = net.secure ? 'Connect to Network' : 'Save Network';
  $('#wifiModalSsid').textContent = net.ssid;
  const pwd = $('#wifiPassword');
  pwd.value = '';
  pwd.style.display = net.secure ? '' : 'none';
  $('#wifiModal').classList.add('visible');
  if (net.secure) setTimeout(() => pwd.focus(), 50);
}

function closeWifiModal() {
  $('#wifiModal').classList.remove('visible');
  modalCtx = null;
}

async function saveWifi() {
  if (!modalCtx) return;
  const body = {
    ssid: modalCtx.ssid,
    password: modalCtx.secure ? $('#wifiPassword').value : '',
  };
  try {
    await postJson(API.add, body);
    toast('Saved ' + body.ssid);
    closeWifiModal();
    loadSaved();
  } catch (err) {
    toast('Save failed: ' + err.message, 'error');
  }
}

// ============================================================================
// Exit
// ============================================================================

async function exitConfig() {
  if (!confirm('Exit Config Mode? The device will return to normal voice operation and this WiFi network will disappear.')) return;
  try {
    await postJson(API.exit);
    toast('Exiting…');
    setTimeout(() => {
      document.body.innerHTML = '<div style="padding:40px;text-align:center;color:#7a8a8a;font-family:ui-monospace,monospace;">Device returned to normal mode.<br>You can disconnect from Jarvis-Setup.</div>';
    }, 1500);
  } catch (err) {
    toast('Exit failed', 'error');
  }
}

// ============================================================================
// Wire up
// ============================================================================

window.addEventListener('DOMContentLoaded', () => {
  initTabs();

  $('#rescanBtn').addEventListener('click', startScan);
  $('#exitBtn').addEventListener('click', exitConfig);
  $('#saveBtn').addEventListener('click', saveConfig);
  $('#discardBtn').addEventListener('click', discardChanges);

  $('#wifiCancel').addEventListener('click', closeWifiModal);
  $('#wifiSave').addEventListener('click', saveWifi);
  $('#wifiModal').addEventListener('click', (e) => {
    if (e.target.id === 'wifiModal') closeWifiModal();
  });
  $('#wifiPassword').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') saveWifi();
  });

  loadConfig();
  startStatusPolling();
});
