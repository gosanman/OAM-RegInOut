'use strict';
// RegInOut-Simulator - Oberflaeche. Holt Metadaten einmal, fragt den Zustand
// alle 250 ms ab und zeichnet nur, was sich geaendert hat. Eingaben gehen als
// Aktionen an die Python-App, die sie an den Kern weiterreicht.

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const h = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'style') e.setAttribute('style', v);
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'html') e.innerHTML = v;
    else if (k === 'value') e.value = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat()) if (kid !== null && kid !== undefined && kid !== false)
    e.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  return e;
};
const de = (v, d = 2) => (v === null || v === undefined || Number.isNaN(v)) ? '—'
  : Number(v).toLocaleString('de-DE', { minimumFractionDigits: d, maximumFractionDigits: d });
const fmtT = ms => {
  const s = Math.floor(ms / 1000);
  return `T+${String(Math.floor(s / 3600)).padStart(2, '0')}:${String(Math.floor(s / 60) % 60).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
};
const fmtTms = ms => fmtT(ms) + ',' + String(Math.floor(ms % 1000)).padStart(3, '0');
const fmtShortT = ms => {
  const s = ms / 1000;
  if (s < 120) return de(s, 1) + ' s';
  if (s < 7200) return de(s / 60, 1) + ' min';
  return de(s / 3600, 2) + ' h';
};

const RELAY_COLOR = ['#2563eb', '#ea580c', '#16a34a', '#9333ea', '#e11d48', '#0891b2', '#65a30d', '#a16207'];
const PIN_MODE = { '-1': 'hochohmig', 0: 'Eingang', 1: 'Ausgang', 2: 'Eingang, Pull-up', 3: 'Eingang, Pull-down' };

const S = {
  meta: null, params: {}, kos: {}, pages: [],
  poll: null, since: -1, relayIdx: 0, kv: -1,
  relay: [], kovals: {},
  tab: 'inputs', win: 60000,
  values: {}, activeValues: {}, visKos: new Map(),
  logLines: [], busRows: [], checks: [], warnCount: 0,
  sigInputs: '', sigParams: '', sigKos: '', sigHw: '', sigDev: '',
};

// ------------------------------------------------------------ Server
async function act(body) {
  let j;
  try {
    const r = await fetch('/api/action', { method: 'POST', body: JSON.stringify(body) });
    j = await r.json();
  } catch (e) { j = { ok: false, error: String(e) }; }
  if (!j.ok) alert('Fehler: ' + j.error);
  setTimeout(poll, 30);
  return j;
}

// ------------------------------------------------------------ ETS-Sichtbarkeit
function evalTest(v, test) {
  test = String(test).trim();
  const m = test.match(/^(<=|>=|!=|<|>|=)\s*(-?[\d.]+)$/);
  const x = Number(v);
  if (m) {
    const y = Number(m[2]);
    switch (m[1]) {
      case '<': return x < y; case '>': return x > y; case '<=': return x <= y;
      case '>=': return x >= y; case '!=': return x != y; default: return x == y;
    }
  }
  return test.split(/\s+/).some(t => Number(t) === x || t === String(v));
}

function condOk(conds, vals) {
  for (const c of conds) {
    const v = vals[c[0]];
    if (c[1] === null || c[1] === undefined) {
      if ((c[2] || []).some(t => evalTest(v, t))) return false;
    } else if (!evalTest(v, c[1])) return false;
  }
  return true;
}

/// Werte wie die ETS sie sieht: Vorgaben, darueber die Projektwerte, dann die
/// <Assign>-Zuweisungen.
function effectiveValues(changed) {
  const v = {};
  for (const [n, p] of Object.entries(S.params)) v[n] = p.d;
  Object.assign(v, changed);
  for (let pass = 0; pass < 3; pass++)
    for (const pg of S.pages) {
      if (!condOk(pg.cond, v)) continue;
      for (const it of pg.items)
        if (it.type === 'a' && it.target && condOk(it.cond, v))
          v[it.target] = it.source ? v[it.source] : Number(it.value);
    }
  return v;
}

function visibleKos(vals) {
  const m = new Map();
  for (const pg of S.pages) {
    if (!condOk(pg.cond, vals)) continue;
    for (const it of pg.items)
      if (it.type === 'k' && !m.has(it.nr) && condOk(it.cond, vals)) {
        m.set(it.nr, it.dpt);
        const k = S.kos[it.nr];
        if (k) {
          if (k.t0 === undefined) { k.t0 = k.t; k.f0 = k.f; }
          k.t = it.t || k.t0; k.f = it.f || k.f0;
        }
      }
  }
  return m;
}

// ------------------------------------------------------------ Abfrage
async function poll() {
  let j;
  try {
    const r = await fetch(`/api/poll?since=${S.since}&relay=${S.relayIdx}&kv=${S.kv}`);
    j = await r.json();
  } catch (e) { return; }
  S.poll = j;
  S.since = j.seq;
  if (j.relayIdx < S.relayIdx) S.relay = [];   // Kern neu gestartet
  S.relay.push(...j.relay);
  if (S.relay.length > 100000) S.relay.splice(0, 25000);
  S.relayIdx = j.relayIdx;
  if (j.kos) { S.kovals = j.kos; S.kv = j.kosVersion; }
  for (const ev of j.events) onEvent(ev);

  S.values = effectiveValues(j.pending);
  S.activeValues = effectiveValues(j.active);
  S.visKos = visibleKos(S.activeValues);
  render();
}

function onEvent(ev) {
  const t = ev.t !== undefined ? fmtT(ev.t) : '';
  switch (ev.ev) {
    case 'log':
      pushLog(`${t}  ${ev.src ? ev.src + ': ' : ''}${ev.text}`, ev.level); break;
    case 'ko': case 'rx':
      S.busRows.push(ev);
      if (S.busRows.length > 3000) S.busRows.splice(0, 1000);
      if (ev.ev === 'ko' && ev.nr === 7) pushLog(`${t}  Diagnose-KO: ${ev.val}`, 'diag');
      break;
    case 'warn':
      S.checks.push({ t: ev.t, kind: ev.inv, text: ev.text + (ev.count > 1 ? ` (${ev.count}×)` : ''), cls: 'err' });
      S.warnCount++; break;
    case 'info':
      S.checks.push({ t: ev.t, kind: 'Hinweis', text: ev.text, cls: 'wrn' }); break;
    case 'error':
      S.checks.push({ t: ev.t, kind: 'Fehler', text: ev.text, cls: 'err' });
      pushLog(`${t}  FEHLER: ${ev.text}`, 'error'); break;
    case 'boot':
      pushLog(`${t}  ── Neustart, ${ev.configured ? 'parametriert' : 'ohne Parametrierung'} ──`, 'debug');
      S.checks.push({ t: ev.t, kind: 'Start', text: ev.configured ? 'Neustart mit Parametern' : 'Neustart ohne Parameter', cls: 'muted' });
      break;
    case 'download':
      pushLog(`${t}  ETS-Download ${ev.phase === 'start' ? 'beginnt' : 'fertig'}`, 'debug'); break;
    case 'supply':
      S.checks.push({ t: ev.t, kind: 'Relais-Versorgung', text: ev.on ? 'ein' : 'aus', cls: 'muted' }); break;
    case 'contact':
      S.checks.push({ t: ev.t, kind: 'Kontakt', text: `Eingang ${ev.no} ${ev.closed ? 'geschlossen' : 'offen'}`, cls: 'muted' }); break;
    case 'mcu':
      S.checks.push({ t: ev.t, kind: 'KNX-Bus', text: 'Bus aus, Prozessor aus', cls: 'muted' }); break;
    case 'busdip':
      S.checks.push({ t: ev.t, kind: 'Busdip', text: ev.phase === 'start' ? `Busspannung bricht ${ev.ms} ms ein (SAVE)` :
        (ev.reboot ? 'Wiederkehr - Neustart' : 'Wiederkehr nach 1 s - weiter ohne Neustart'), cls: 'muted' }); break;
    case 'flash':
      pushLog(`${t}  Flash gespeichert`, 'debug'); break;
  }
  if (S.checks.length > 2000) S.checks.splice(0, 500);
}

function pushLog(text, cls = '') {
  S.logLines.push({ text, cls });
  if (S.logLines.length > 3000) S.logLines.splice(0, 1000);
  S.logDirty = true;
}

// ------------------------------------------------------------ Kanaele
/// Kanal eines Objekts: "Eingang 1", "Relais 2", "Schaltaktor", "OpenKNX".
function channelOfKo(k) {
  let m = k.name.match(/^BI_Channel(\d+)/);
  if (m) return `Eingang ${m[1]}`;
  m = k.name.match(/^SWA_Ch(\d+)/);
  if (m) return `Relais ${m[1]}`;
  return k.name.startsWith('SWA_') ? 'Schaltaktor' : 'OpenKNX';
}
function chRank(ch) {
  const [t, n] = ch.split(' ');
  return ({ Eingang: 0, Relais: 100, Schaltaktor: 200, OpenKNX: 300 }[t] ?? 400) + Number(n || 0);
}
const chName = ch => ch;
const koLabel = k => (k.t || k.name).replace(/^(BE|SA) ?\w*:\s*/, '');

// ------------------------------------------------------------ Kopfleiste
function renderBar() {
  const st = S.poll.state || {};
  $('#clock').textContent = fmtT(st.t || 0);
  $('#uptime').textContent = st.mcu ? 'Laufzeit ' + fmtShortT(st.uptime || 0) : 'Prozessor aus';
  $$('#speed [data-speed]').forEach(b => b.classList.toggle('active', Number(b.dataset.speed) === S.poll.speed));
  for (const [id, on] of [['#pwr-bus', st.bus], ['#pwr-supply', st.supply]]) {
    $(id).classList.toggle('on', !!on); $(id).classList.toggle('off', !on);
  }
  const dirty = Object.keys(diffValues()).length;
  $('#btn-program').classList.toggle('dirty', dirty > 0);
  $('#btn-program').textContent = dirty ? `Programmieren (${dirty})` : 'Programmieren';
  $('#dirty-count').textContent = dirty || '';
  $('#warn-count').textContent = S.warnCount || '';
  const hello = S.poll.hello || {};
  $('#device-sub').textContent = `${hello.device || 'Reg-InOut'} · ${(hello.inputPins || []).length} Eingänge, ${(hello.relayPins || []).length} Relais · echter Firmware-Code`;

  const b = [];
  const badge = (txt, cls, tip) => b.push(h('span', { class: 'badge ' + cls, title: tip }, txt));
  badge(st.mcu ? 'Prozessor läuft' : 'Prozessor aus', st.mcu ? 'ok' : 'bad');
  badge(st.paramsValid ? 'programmiert' : 'nie programmiert', st.paramsValid ? 'ok' : 'warn');
  if (st.downloading) badge('ETS-Download', 'warn');
  if (!st.supply) badge('keine Relais-Versorgung', 'warn', 'Die Firmware schaltet weiter, die Relais ziehen aber nicht an');
  $('#badges').replaceChildren(...b);

  const sel = $('#project');
  const names = S.poll.projects.join('|') + '#' + S.poll.project;
  if (sel.dataset.sig !== names) {
    sel.dataset.sig = names;
    sel.replaceChildren(h('option', { value: '' }, '— Projekt laden —'),
      ...S.poll.projects.map(p => h('option', { value: p, selected: p === S.poll.project }, p)));
  }
}

function diffValues() {
  const p = S.poll.pending || {}, a = S.poll.active || {}, d = {};
  for (const k of new Set([...Object.keys(p), ...Object.keys(a)]))
    if (p[k] !== a[k]) d[k] = p[k];
  return d;
}

// ------------------------------------------------------------ Geraet (rechte Spalte)
function relayText(r) {
  if (!r.visible) return 'nicht verfügbar';
  if (!r.active) return 'Kanal inaktiv';
  const parts = [];
  if (r.locked) parts.push('gesperrt');
  if (r.onDelay) parts.push(`Ein in ${de(r.onDelay / 1000, 1)} s`);
  if (r.offDelay) parts.push(`Aus in ${de(r.offDelay / 1000, 1)} s`);
  return parts.join(', ') || (r.opener ? 'Öffner' : 'Schließer');
}

function renderDevice() {
  const sim = S.poll.state?.sim;
  if (!sim) return;
  const sig = JSON.stringify([sim.relays, sim.inputs, S.poll.state.mcu]);
  if (sig === S.sigDev) return;
  S.sigDev = sig;
  const tiles = [];
  for (const i of sim.inputs) {
    const st = i.state === undefined ? '—' : i.state === -1 ? 'unbekannt' : i.state ? 'geschlossen' : 'offen';
    tiles.push(h('div', { class: 'tile' + (i.active === false || i.active === undefined ? ' off-dim' : '') },
      h('h4', {}, `Eingang ${i.no}`, h('span', { class: 'muted' }, `GPIO ${i.pin}`)),
      h('div', { class: 'row' }, 'Pegel', h('b', {}, i.read ? 'HIGH' : 'LOW')),
      h('div', { class: 'row' }, 'Firmware', h('b', {}, i.active === undefined ? '—' : i.active ? st : 'inaktiv')),
      h('button', { class: 'contact' + (i.closed ? ' closed' : ''), title: 'Kontakt am Eingang umschalten',
        onclick: () => act({ action: 'contact', no: i.no, closed: !i.closed }) }, i.closed ? 'Kontakt geschlossen' : 'Kontakt offen')));
  }
  for (const r of sim.relays) {
    const mismatch = r.visible && r.active && S.poll.state.supply && r.on !== (r.status !== r.opener);
    tiles.push(h('div', { class: 'tile' + (r.on ? ' on' : '') + (mismatch ? ' mismatch' : '') + (!r.visible || !r.active ? ' off-dim' : ''),
      title: `GPIO ${r.pin}: ${r.output ? 'Ausgang' : 'hochohmig'}, Pegel ${r.level ? 'HIGH' : 'LOW'}, ${r.writes} Schreibzugriffe` },
      h('h4', {}, h('span', {}, h('span', { class: 'coil' + (r.on ? ' on' : '') }), `Relais K${r.no}`), h('span', { class: 'muted' }, `GPIO ${r.pin}`)),
      h('div', { class: 'big' }, r.on ? 'angezogen' : 'abgefallen'),
      h('div', { class: 'row' }, 'Status-KO', h('b', {}, r.status === undefined ? '—' : r.status ? 'Ein' : 'Aus')),
      h('div', { class: 'row' }, h('span', {}, relayText(r)))));
  }
  $('#device').replaceChildren(...tiles);
}

function renderModule() {
  const st = S.poll.state || {}, sim = st.sim || {};
  $('#module').replaceChildren(
    h('div', {}, `Telegramme gesendet: ${sim.tx ?? 0}`),
    h('div', {}, `Projekt: ${S.poll.project || '—'}`),
    h('div', {}, 'Logikmodul und FileTransfer laufen im Simulator nicht mit.'));
}

// ------------------------------------------------------------ Verlauf
function renderChart() {
  const cv = $('#chart');
  const w = cv.clientWidth, hh = cv.clientHeight;
  if (!w) return;
  const dpr = window.devicePixelRatio || 1;
  if (cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(hh * dpr); }
  const g = cv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, hh);
  const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  const now = S.poll.state?.t || 0, t0 = now - S.win;
  const n = (S.poll.hello?.relayPins || []).length || 2;
  const lane = (hh - 20) / n;
  g.font = '11px system-ui'; g.textBaseline = 'middle';
  for (let ch = 0; ch < n; ch++) {
    const y0 = 4 + ch * lane, yOn = y0 + 4, yOff = y0 + lane - 6;
    g.strokeStyle = css('--line'); g.beginPath(); g.moveTo(40, yOff + 2); g.lineTo(w, yOff + 2); g.stroke();
    g.fillStyle = css('--muted'); g.fillText(`K${ch + 1}`, 6, (yOn + yOff) / 2);
    g.strokeStyle = RELAY_COLOR[ch % RELAY_COLOR.length]; g.lineWidth = 2; g.beginPath();
    let last = 0, started = false;
    for (const p of S.relay) {
      if (p[0] < t0) { last = p[1 + ch]; continue; }
      const x = 40 + (p[0] - t0) / S.win * (w - 40);
      const yPrev = last ? yOn : yOff;
      if (!started) { g.moveTo(40, yPrev); started = true; }
      g.lineTo(x, yPrev); g.lineTo(x, p[1 + ch] ? yOn : yOff);
      last = p[1 + ch];
    }
    if (!started) g.moveTo(40, last ? yOn : yOff);
    g.lineTo(w, last ? yOn : yOff); g.stroke(); g.lineWidth = 1;
  }
  g.fillStyle = css('--muted');
  g.fillText(fmtT(Math.max(0, t0)), 40, hh - 8);
  const tr = fmtT(now); g.fillText(tr, w - g.measureText(tr).width - 2, hh - 8);
}

// ------------------------------------------------------------ Objektwerte
function fmtKo(nr, v, dpt) {
  if (!v || v.val === undefined || v.val === null) return '—';
  const [m, s] = String(dpt || v.dpt || '').split('.').map(Number);
  const raw = v.raw || '';
  const b0 = raw ? parseInt(raw.slice(0, 2), 16) : NaN;
  if (raw && !Number.isNaN(b0)) {
    if (m === 1) {
      const txt = { 1: ['Aus', 'Ein'], 2: ['falsch', 'wahr'], 5: ['kein Alarm', 'Alarm'], 17: ['0', 'Auslösen'] }[s] || ['0', '1'];
      return txt[b0 & 1];
    }
    if (m === 17) return 'Szene ' + ((b0 & 0x3F) + 1);
    if (m === 18) return (b0 & 0x80 ? 'Lernen ' : 'Szene ') + ((b0 & 0x3F) + 1);
  }
  const val = v.val;
  if (typeof val === 'boolean') return val ? '1' : '0';
  if (typeof val !== 'number') return String(val);
  if (m === 14) {
    if (s === 27) return de(val, 2) + ' V';
    if (s === 19) return de(val, 3) + ' A';
    if (s === 56) return de(val, 1) + ' W';
    return de(val, 2);
  }
  if (m === 9) return de(val, 1);
  return de(val, Number.isInteger(val) ? 0 : 2);
}
const koValText = (nr, dpt) => fmtKo(nr, S.kovals[nr], dpt);

function sendKo(nr, dpt, value, cyclic) { return act({ action: 'ko', nr, dpt, value, cyclic: cyclic || 0 }); }
function cycPeriod() { try { return Number(localStorage.getItem('iosim.cyc')) || 60; } catch (e) { return 60; } }

function inputControl(k, dpt) {
  const [main, sub] = dpt.split('.').map(Number);
  const cyc = (S.poll.cyclic || []).find(c => c.nr === k.nr);
  const wrap = h('div', { class: 'ctl' });
  const cur = S.kovals[k.nr];
  if (main === 1) {
    if (sub === 17) { wrap.append(h('button', { onclick: () => sendKo(k.nr, dpt, 1) }, 'Auslösen')); return wrap; }
    const b = cur?.raw ? parseInt(cur.raw.slice(0, 2), 16) & 1 : null;
    const txt = sub === 1 ? ['Aus', 'Ein'] : ['0', '1'];
    wrap.append(h('div', { class: 'seg' },
      h('button', { class: b === 0 ? 'active' : '', onclick: () => sendKo(k.nr, dpt, 0) }, txt[0]),
      h('button', { class: b === 1 ? 'active' : '', onclick: () => sendKo(k.nr, dpt, 1) }, txt[1])));
    return wrap;
  }
  if (main === 17 || main === 18) {
    const inp = h('input', { type: 'number', min: 1, max: 64, value: 1, style: 'width:4em',
      onkeydown: e => { if (e.key === 'Enter') sendKo(k.nr, dpt, Number(inp.value) - 1); } });
    wrap.append(h('span', { class: 'unit' }, 'Szene'), inp, h('button', { onclick: () => sendKo(k.nr, dpt, Number(inp.value) - 1) }, 'abrufen'));
    return wrap;
  }
  if (main === 16) {
    const inp = h('input', { placeholder: 'Text', size: 10, maxlength: 14 });
    wrap.append(inp, h('button', { onclick: () => sendKo(k.nr, dpt, inp.value) }, 'senden'));
    return wrap;
  }
  if (main === 10 || main === 11 || main === 19) {
    wrap.append(h('span', { class: 'muted' }, 'Uhrzeit wertet kein simuliertes Modul aus'));
    return wrap;
  }
  const inp = h('input', { type: 'number', step: main === 9 || main === 14 ? 0.1 : 1, value: cyc ? cyc.value : '',
    placeholder: 'Wert', onkeydown: e => { if (e.key === 'Enter') send(false); } });
  const send = cyclic => { if (inp.value !== '') sendKo(k.nr, dpt, inp.value.replace(',', '.'), cyclic ? cycPeriod() : 0); };
  wrap.append(inp, h('button', { onclick: () => send(!!cyc), title: 'Senden (Eingabetaste)' }, 'senden'),
    h('button', { class: 'icon' + (cyc ? ' active' : ''), title: cyc ? `sendet alle ${cyc.period} s – klicken zum Beenden` : `zyklisch senden, alle ${cycPeriod()} s`,
      onclick: () => cyc ? act({ action: 'cyclic_off', nr: k.nr }) : send(true) }, '⟳'));
  return wrap;
}

// ------------------------------------------------------------ Bedienung
function renderInputs(force) {
  const vis = [...S.visKos.entries()];
  const sig = vis.map(x => x.join(':')).join(',') + '|' + JSON.stringify(S.poll.cyclic) + '|' + JSON.stringify(S.poll.active) +
    '|' + (S.poll.state?.mcu ? 1 : 0);
  if (!force && sig === S.sigInputs) { updateKoValues(); return; }
  if (!force && document.activeElement && $('#tab-inputs').contains(document.activeElement) &&
      ['INPUT', 'SELECT'].includes(document.activeElement.tagName)) { updateKoValues(); return; }
  S.sigInputs = sig;
  const groups = new Map();
  for (const [nr, dpt] of vis) {
    const k = S.kos[nr];
    if (!k) continue;
    const ch = channelOfKo(k);
    if (!groups.has(ch)) groups.set(ch, { inp: [], out: [] });
    (k.inp ? groups.get(ch).inp : groups.get(ch).out).push([k, dpt || k.dpts[0] || '']);
  }
  const card = ch => {
    const g = groups.get(ch);
    const row = ([k, dpt]) => h('div', { class: 'korow', title: `${k.t} · ${k.f}\nObjekt ${k.nr} · DPT ${dpt}` },
      h('span', { class: 'nr' }, k.nr), h('span', { class: 'lbl' }, koLabel(k)),
      inputControl(k, dpt), h('span', { class: 'val', 'data-ko': k.nr, 'data-dpt': dpt }, koValText(k.nr, dpt)));
    const outs = g.out.length ? h('details', { open: true }, h('summary', {}, 'Ausgänge'),
      h('div', { class: 'outgrid' }, ...g.out.map(([k, dpt]) =>
        h('div', { class: 'outitem', title: `${k.t} · ${k.f}\nObjekt ${k.nr} · DPT ${dpt}` },
          h('span', { class: 'lbl' }, h('span', { class: 'nr' }, k.nr), ' ', koLabel(k)),
          h('b', { 'data-ko': k.nr, 'data-dpt': dpt }, koValText(k.nr, dpt)))))) : null;
    return h('div', { class: 'chcard' }, h('h4', {}, chName(ch)), ...g.inp.map(row), outs);
  };
  const keys = [...groups.keys()].sort((a, b) => chRank(a) - chRank(b));
  $('#tab-inputs').replaceChildren(
    h('div', { class: 'tools' },
      h('label', { title: 'Gilt für das ⟳ an jedem Zahlenwert' }, 'Zyklisch senden alle ',
        h('input', { type: 'number', min: 1, value: cycPeriod(), style: 'width:5em',
          onchange: e => { try { localStorage.setItem('iosim.cyc', e.target.value); } catch (x) {} } }), ' s'),
      h('span', { class: 'muted' }, 'Die Kontakte der Eingänge schalten die Kacheln rechts.')),
    keys.length ? h('div', { class: 'chcards' }, ...keys.map(card))
      : h('div', { class: 'emptynote' }, 'Keine Objekte sichtbar. Im Reiter „Parameter“ Eingänge und Relais aktivieren und programmieren.'));
  updateKoValues();
}

function updateKoValues() {
  for (const el of $$('[data-ko]')) {
    const nr = Number(el.dataset.ko);
    const txt = koValText(nr, el.dataset.dpt);
    if (el.dataset.txt === txt) continue;
    const first = el.dataset.txt === undefined;
    el.dataset.txt = txt;
    el.textContent = txt;
    if (!first) {
      const rowEl = el.closest('tr, .korow, .outitem');
      rowEl?.classList.remove('flash'); void el.offsetWidth; rowEl?.classList.add('flash');
    }
  }
}

// ------------------------------------------------------------ Objekte
function renderKos(force) {
  if (!$('#kos-tools')) {
    $('#tab-kos').replaceChildren(
      h('div', { class: 'tools', id: 'kos-tools' },
        h('input', { id: 'kos-filter', placeholder: 'Filter: Nummer oder Name', oninput: () => renderKos(true) }),
        h('select', { id: 'kos-dir', onchange: () => renderKos(true) },
          h('option', { value: '' }, 'Ein- und Ausgänge'), h('option', { value: 'in' }, 'nur Eingänge'), h('option', { value: 'out' }, 'nur Ausgänge'))),
      h('p', { class: 'hint' }, 'Alle Objekte, die die ETS mit der programmierten Parametrierung einblendet (ohne Logikmodul). S = schreiben (Eingang), Ü = übertragen (Ausgang). Wert = zuletzt gesendet oder empfangen.'),
      h('div', { class: 'scroll' }, h('table', { class: 'grid', id: 'kos-table' })));
  }
  const f = $('#kos-filter').value.trim().toLowerCase(), dir = $('#kos-dir').value;
  const sig = [...S.visKos.keys()].join(',') + '|' + f + '|' + dir;
  if (!force && sig === S.sigKos) { updateKoValues(); return; }
  S.sigKos = sig;
  const rows = [];
  let lastCh = null;
  const list = [...S.visKos.entries()].filter(([nr]) => S.kos[nr])
    .sort((a, b) => chRank(channelOfKo(S.kos[a[0]])) - chRank(channelOfKo(S.kos[b[0]])) || a[0] - b[0]);
  for (const [nr, dpt] of list) {
    const k = S.kos[nr], ch = channelOfKo(k);
    if (dir === 'in' && !k.inp) continue;
    if (dir === 'out' && !k.out) continue;
    if (f && !(String(nr) === f || k.t.toLowerCase().includes(f) || k.f.toLowerCase().includes(f) || k.name.toLowerCase().includes(f))) continue;
    if (ch !== lastCh) { rows.push(h('tr', { class: 'grouprow' }, h('td', { colspan: 7 }, chName(ch)))); lastCh = ch; }
    const v = S.kovals[nr];
    rows.push(h('tr', {},
      h('td', { class: 'num' }, nr), h('td', {}, koLabel(k), h('div', { class: 'muted' }, k.name)),
      h('td', { class: 'mono' }, dpt), h('td', {}, k.size),
      h('td', {}, (k.inp ? 'S' : '') + (k.out ? 'Ü' : '')),
      h('td', { class: 'num', 'data-ko': nr, 'data-dpt': dpt }, koValText(nr, dpt)),
      h('td', { class: 'mono muted' }, v ? `${v.dir === 'rx' ? '→' : '←'} ${fmtT(v.t)} · ${v.n}×` : '')));
  }
  $('#kos-table').replaceChildren(h('tr', {}, ...['Nr', 'Objekt', 'DPT', 'Größe', 'Flags', 'Wert', 'zuletzt'].map(x => h('th', {}, x))), ...rows);
}

// Parameter: ETS-Ansicht in ets.js (renderParams)

// ------------------------------------------------------------ Hardware
function renderHw(force) {
  const hw = S.poll.hw, sim = S.poll.state?.sim, hello = S.poll.hello || {};
  if (!hw || !sim) return;
  const sig = JSON.stringify([hw, sim.relays, sim.inputs, S.poll.state.mcu]);
  if (!force && sig === S.sigHw) return;
  if (!force && document.activeElement && $('#tab-hw').contains(document.activeElement) && document.activeElement.tagName === 'INPUT') return;
  S.sigHw = sig;
  const pinRows = [];
  for (const i of sim.inputs)
    pinRows.push(h('tr', {}, h('td', { class: 'num' }, i.pin), h('td', {}, `Binäreingang ${i.no}`),
      h('td', {}, PIN_MODE[i.pull] || i.pull), h('td', { class: 'mono' }, i.read ? 'HIGH' : 'LOW'),
      h('td', {}, h('label', {}, h('input', { type: 'checkbox', checked: i.closed,
        onchange: e => act({ action: 'contact', no: i.no, closed: e.target.checked }) }), ' Kontakt geschlossen'))));
  for (const r of sim.relays)
    pinRows.push(h('tr', {}, h('td', { class: 'num' }, r.pin), h('td', {}, `Relais K${r.no}`),
      h('td', {}, r.output ? 'Ausgang' : 'hochohmig'), h('td', { class: 'mono' }, r.level ? 'HIGH' : 'LOW'),
      h('td', { class: 'muted' }, `${r.writes} Schreibzugriffe seit dem Start`)));
  $('#tab-hw').replaceChildren(
    h('div', { class: 'hwgrid' },
      h('div', { class: 'card' }, h('h3', {}, 'Versorgung'),
        h('div', { class: 'hwform' },
          h('label', {}, h('input', { type: 'checkbox', checked: hw.supply, onchange: e => act({ action: 'power', which: 'supply', on: e.target.checked }) }), ' Relais-Versorgung ein'),
          h('label', {}, 'Busdip', h('input', { type: 'number', min: 10, max: 5000, step: 10, value: S.busdipMs || 200,
            onchange: e => { S.busdipMs = Number(e.target.value); } }), h('span', { class: 'unit' }, ' ms'))),
        h('p', { class: 'hint', style: 'margin-top:8px' }, 'Die Relais sind monostabil: Sie ziehen nur an, solange der Prozessor den Pin treibt und die Versorgung anliegt. Ohne KNX-Bus läuft der Prozessor nicht, alle Pins sind hochohmig, die Relais fallen ab. Der Busdip löst SAVE aus; nach 1 s ruft OpenKNX restorePower() ohne Neustart.')),
      h('div', { class: 'card' }, h('h3', {}, 'Board'),
        h('p', { class: 'hint' }, `${hello.device || 'Reg-InOut'}: Binäreingänge an GPIO ${(hello.inputPins || []).join(', ')} (aktiv LOW, Pull-up), Relais an GPIO ${(hello.relayPins || []).join(', ')} (aktiv HIGH). Pins aus include/hardware.h.`))),
    h('div', { class: 'card' }, h('h3', {}, 'Pins'),
      h('table', { class: 'grid ports' },
        h('tr', {}, ...['GPIO', 'Funktion', 'Modus', 'Pegel', ''].map(x => h('th', {}, x))), ...pinRows)));
}

// ------------------------------------------------------------ Telegramme, Konsole, Pruefung
function renderBus() {
  const f = $('#bus-filter').value.trim().toLowerCase();
  const rx = $('#bus-rx').checked, tx = $('#bus-tx').checked;
  const rows = [];
  for (let i = S.busRows.length - 1; i >= 0 && rows.length < 400; i--) {
    const e = S.busRows[i];
    if ((e.ev === 'rx' && !rx) || (e.ev === 'ko' && !tx)) continue;
    const k = S.kos[e.nr] || { t: '?', f: '', name: '' };
    if (f && !(String(e.nr) === f || k.t.toLowerCase().includes(f) || k.f.toLowerCase().includes(f) || k.name.toLowerCase().includes(f))) continue;
    const etsDpt = S.visKos.get(e.nr) || e.dpt;
    const hidden = !S.visKos.has(e.nr);
    rows.push(h('tr', { class: e.ev === 'rx' ? 'rx' : 'tx' }, h('td', { class: 'mono' }, fmtTms(e.t)),
      h('td', {}, e.ev === 'rx' ? (e.delivered ? '→ empfangen' : '→ verworfen') : (hidden ? '← gesendet (Objekt in der ETS ausgeblendet)' : '← gesendet')),
      h('td', { class: 'num' }, e.nr), h('td', {}, `${channelOfKo(k.name ? k : { name: '' })}: ${koLabel(k)}`),
      h('td', { class: 'mono' }, e.dpt === etsDpt ? e.dpt : `${e.dpt} · ETS ${etsDpt}`),
      h('td', { class: 'mono' }, e.raw || ''),
      h('td', { class: 'mono' }, fmtKo(e.nr, e, etsDpt))));
  }
  $('#bus-table').replaceChildren(h('tr', {}, ...['Zeit', 'Richtung', 'Nr', 'Objekt', 'DPT', 'Roh', 'Wert'].map(x => h('th', {}, x))), ...rows);
}

function renderLog() {
  if (!S.logDirty) return;
  S.logDirty = false;
  const el = $('#log');
  const atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 30;
  el.replaceChildren(...S.logLines.slice(-1500).map(l => h('div', { class: l.cls }, l.text)));
  if (atEnd) el.scrollTop = el.scrollHeight;
}

function renderChecks() {
  $('#checks-table').replaceChildren(h('tr', {}, h('th', {}, 'Zeit'), h('th', {}, 'Art'), h('th', {}, 'Meldung')),
    ...S.checks.slice(-600).reverse().map(c => h('tr', {}, h('td', { class: 'mono' }, fmtT(c.t || 0)),
      h('td', { class: c.cls }, c.kind), h('td', {}, c.text))));
}

// ------------------------------------------------------------ Gesamt
function render() {
  if (!S.poll) return;
  etsApplyWide();
  renderBar();
  renderDevice();
  renderChart();
  renderModule();
  if (S.tab === 'inputs') renderInputs();
  else if (S.tab === 'params') renderParams();
  else if (S.tab === 'kos') renderKos();
  else if (S.tab === 'bus') renderBus();
  else if (S.tab === 'console') renderLog();
  else if (S.tab === 'hw') renderHw();
  else if (S.tab === 'checks') renderChecks();
}

function resetHistory() { S.relay = []; S.relayIdx = 0; S.checks = []; S.busRows = []; S.warnCount = 0; }

function setupUi() {
  $$('#tabs button').forEach(b => b.addEventListener('click', () => {
    S.tab = b.dataset.tab;
    $$('#tabs button').forEach(x => x.classList.toggle('active', x === b));
    $$('.tabbody').forEach(x => x.classList.toggle('hidden', x.id !== 'tab-' + S.tab));
    if (S.tab === 'checks') S.warnCount = 0;
    S.logDirty = true; S.sigInputs = S.sigParams = S.sigKos = S.sigHw = '';
    try { history.replaceState(null, '', '#' + S.tab); } catch (e) {}
    render();
  }));
  // Reiter aus der Adresse (#params, #hw ...): bleibt beim Neuladen erhalten.
  const fromHash = location.hash.slice(1);
  $$('#tabs button').find(b => b.dataset.tab === fromHash)?.click();
  $$('#speed [data-speed]').forEach(b => b.addEventListener('click', () => act({ action: 'speed', value: b.dataset.speed })));
  $$('#speed [data-step]').forEach(b => b.addEventListener('click', () => act({ action: 'step', ms: b.dataset.step })));
  $('#pwr-bus').addEventListener('click', () => act({ action: 'power', which: 'bus', on: !S.poll.state.bus }));
  $('#pwr-supply').addEventListener('click', () => act({ action: 'power', which: 'supply', on: !S.poll.state.supply }));
  $('#btn-busdip').addEventListener('click', () => act({ action: 'busdip', ms: S.busdipMs || 200 }));
  $('#btn-program').addEventListener('click', () => act({ action: 'program' }));
  $('#btn-restart').addEventListener('click', () => act({ action: 'restart' }));
  $('#btn-unload').addEventListener('click', () => { if (confirm('Applikation entladen? Das Gerät startet ohne Parameter, wie neu.')) act({ action: 'unload' }); });
  $$('[data-btn]').forEach(b => b.addEventListener('click', () => act({ action: 'btn', kind: b.dataset.btn })));
  $('#project').addEventListener('change', e => { if (e.target.value) act({ action: 'project_load', name: e.target.value }).then(resetHistory); });
  $('#btn-save').addEventListener('click', () => {
    const name = prompt('Projektname:', S.poll.project || 'Mein Aufbau');
    if (name) act({ action: 'project_save', name });
  });
  $$('#window button').forEach(b => b.addEventListener('click', () => {
    S.win = Number(b.dataset.win); $$('#window button').forEach(x => x.classList.toggle('active', x === b)); renderChart();
  }));
  const sendCmd = text => act({ action: 'console', text, diag: $('#cmd-diag').checked });
  $('#cmd-send').addEventListener('click', () => { const t = $('#cmd').value.trim(); if (t) sendCmd(t); });
  $('#cmd').addEventListener('keydown', e => { if (e.key === 'Enter') { const t = e.target.value.trim(); if (t) sendCmd(t); } });
  $$('[data-cmd]').forEach(b => b.addEventListener('click', () => sendCmd(b.dataset.cmd)));
  $('#log-clear').addEventListener('click', () => { S.logLines = []; S.logDirty = true; renderLog(); });
  $('#bus-clear').addEventListener('click', () => { S.busRows = []; renderBus(); });
  for (const id of ['#bus-filter', '#bus-rx', '#bus-tx']) $(id).addEventListener('input', renderBus);
  window.addEventListener('resize', renderChart);
}

async function main() {
  S.meta = await (await fetch('/api/meta')).json();
  S.params = S.meta.params;
  S.pages = S.meta.pages;
  for (const k of S.meta.kos) S.kos[k.nr] = k;
  setupUi();
  await poll();
  setInterval(poll, 250);
}
main();
