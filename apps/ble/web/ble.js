/* The BLE app's page in the board's portal (docs/PORTAL-PAGES.md): the scan
 * seen from a computer, while the app is open on the board.
 *
 * Live through /api/live (docs/PORTAL-PAGES.md "Live data"): the app puts
 * "state" (the scan and the air's two minutes), "devices" (the table) and
 * "dev" (the device this page has open, its packets explained and raw), once
 * a second while someone looks. The page sends lines of key=value: sel,
 * pause, active, duty, env, minrssi, csv, mqtt, forget, fav, alias. The CSVs
 * of the sensors are plain card files under /ble. Texts in Spanish, like the
 * rest of the portal, through tr() (web.ble.lang). */
const P = window.P4OS.forPage ? window.P4OS.forPage('ble') : window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const num = P.num || ((v, d) => v == null ? '--' : v.toFixed(d).replace('.', ','));
const APP = 'aos.ble';
const LIVE = 'live?app=' + APP;

/* What the app puts in the live channel (the classes, the labels, the
 * packets explained, the GATT states) are its own keys, in Spanish: the app
 * translates them on its screen. Here they are looked up in its pack,
 * /lang/<code>/aos.ble.lang, which tools/install_lang.sh puts on the card
 * next to this page's; without it they stay as they come. */
const APP_TR = new Map();
const appTr = s => s ? (APP_TR.get(s) ?? tr(s)) : s;
const appPack = (async () => {
  const code = P.lang ? P.lang() : 'es';
  if (code === 'es') return;
  try {
    const r = await fetch(`/lang/${code}/aos.ble.lang`, { cache: 'no-store' });
    if (!r.ok) return;
    const unesc = x => x.replace(/\\(.)/g, (_, c) => c === 'n' ? '\n' : c === 't' ? '\t' : c);
    for (const line of (await r.text()).split('\n')) {
      if (!line || line[0] === '#') continue;
      const tab = line.indexOf('\t');
      if (tab > 0 && tab < line.length - 1) APP_TR.set(unesc(line.slice(0, tab)), unesc(line.slice(tab + 1).replace(/\r$/, '')));
    }
  } catch { /* the page works without it */ }
})();

const css = `
.bl-top{display:flex;flex-wrap:wrap;gap:10px 16px;align-items:center}
.bl-canvas{width:100%;display:block;border-radius:10px;background:var(--card2)}
.bl-tbl{width:100%;border-collapse:collapse;font-size:14px}
.bl-tbl th{text-align:left;font-weight:600;color:var(--dim);padding:6px 4px;cursor:pointer;white-space:nowrap;user-select:none}
.bl-tbl td{padding:6px 4px;border-top:1px solid var(--line);vertical-align:top}
.bl-tbl tr.k{cursor:pointer}
.bl-tbl tr.k:hover td{background:var(--card2)}
.bl-tbl tr.sel td{background:color-mix(in srgb,var(--accent) 18%,transparent)}
.bl-tbl tr.gone td{opacity:.5}
.bl-tbl .num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.bl-tbl .sub{color:var(--dim);font-size:12px}
.bl-bar{display:inline-block;height:8px;border-radius:4px;vertical-align:middle;margin-left:6px}
.bl-dot{display:inline-block;width:10px;height:10px;border-radius:5px;margin-right:6px;vertical-align:middle}
.bl-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px}
.bl-sen{background:var(--card2);border-radius:12px;padding:12px;cursor:pointer}
.bl-sen .big{font-size:2em;font-weight:600;font-variant-numeric:tabular-nums}
.bl-kv{display:grid;grid-template-columns:minmax(120px,30%) 1fr;gap:4px 12px;font-size:14px}
.bl-kv .k{color:var(--dim)}
.bl-hex{font-family:ui-monospace,Menlo,monospace;font-size:13px;word-break:break-all;color:var(--dim)}
.bl-wrap{overflow-x:auto}
@media (max-width:700px){.bl-hide-s{display:none}}
`;

/* by the class the app sends, which is its Spanish key (bl_class_name);
 * the name shown goes through appTr() */
const CLS_COLOR = Object.fromEntries([
  [N_('Teléfono'), '#0A84FF'], [N_('Computadora'), '#5E5CE6'], [N_('Tableta'), '#5E5CE6'], [N_('Reloj'), '#FF9F0A'], [N_('Audio'), '#BF5AF2'],
  [N_('Rastreador'), '#FF375F'], [N_('Sensor'), '#30D158'], [N_('Baliza'), '#40C8E0'], [N_('Teclado o mouse'), '#64D2FF'], [N_('Joystick'), '#FF6482'],
  [N_('Tele'), '#AC8E68'], [N_('Salud'), '#FF453A'], [N_('Luz'), '#FFD60A'], [N_('Deporte'), '#66D4CF'], [N_('Placa de desarrollo'), '#E5484D'],
  [N_('Desconocido'), '#8e8e93'],
]);
/* a number for the eye, one decimal unless told, and nothing when missing */
const nf = (v, d = 1) => v === undefined || v === null ? '' : num(+v, d);
const age = ms => ms < 2000 ? tr('ahora') : ms < 60000 ? Math.round(ms / 1000) + ' s' : ms < 3600000 ? Math.round(ms / 60000) + ' min' : Math.round(ms / 3600000) + ' h';
const rssiColor = r => r >= -60 ? 'var(--green)' : r >= -75 ? 'var(--accent)' : r >= -88 ? '#e6b800' : 'var(--orange)';
const devName = d => d.al || d.n || appTr(d.lbl) || d.co || tr('Sin nombre');
const ivText = v => v <= 0 ? '' : v < 1000 ? v + ' ms' : nf(v / 1000) + ' s';

function senLine(s) {
  if (!s) return '';
  if (s.encrypted) return tr('cifrado');
  const p = [];
  if (s.temp !== undefined) p.push(nf(s.temp) + ' °C');
  if (s.hum !== undefined) p.push(nf(s.hum, 0) + ' %');
  if (s.press !== undefined) p.push(nf(s.press, 0) + ' hPa');
  if (s.co2 !== undefined) p.push(s.co2 + ' ppm');
  if (s.lux !== undefined) p.push(s.lux + ' lx');
  if (s.open !== undefined) p.push(s.open ? tr('abierto') : tr('cerrado'));
  if (s.motion !== undefined) p.push(s.motion ? tr('movimiento') : tr('quieto'));
  if (s.hr !== undefined) p.push(tr('%s lpm', s.hr));
  if (s.batt !== undefined) p.push(tr('bat. %s', s.batt + ' %'));
  else if (s.volt !== undefined) p.push(nf(s.volt, 2) + ' V');
  return p.join(' · ');
}

P.registerPage({
  id: 'ble',
  name: 'BLE',
  icon: 'ᛒ',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-ble')) document.head.append(h('style', { id: 'css-ble' }, css));
    let st = null, devs = [], sel = null, dev = null, alive = true;
    let sortKey = 'r', sortDir = -1, query = '', onlyAlive = true, clsFilter = '';
    const drawn = {};
    /* the app's pack arrived: everything drawn again with it */
    appPack.then(() => { if (APP_TR.size) for (const k in drawn) drawn[k] = null; });
    const changed = (part, sig) => { if (drawn[part] === sig) return false; drawn[part] = sig; return true; };
    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));

    /* ---- the top: the state and the controls ---- */
    const status = h('div', { class: 'note' }, '');
    const controls = h('div', { class: 'bl-top', style: 'margin-top:10px' });
    /* told when a link went down while the board was scanning */
    const lostBox = h('div', { style: 'display:none;margin-top:10px;padding:10px 12px;border-radius:10px;background:color-mix(in srgb,var(--orange) 18%,transparent)' });
    const chart = h('canvas', { class: 'bl-canvas', height: 140, style: 'margin-top:12px' });
    const top = h('div', { class: 'card' }, h('h2', {}, 'Bluetooth LE'), status, controls, lostBox, chart);
    const missing = h('div', { class: 'card' });

    /* ---- the devices ---- */
    const search = h('input', { type: 'search', placeholder: tr('Buscar por nombre, dirección o fabricante'), style: 'flex:1;min-width:200px',
      oninput: () => { query = search.value.toLowerCase(); drawTable(true); } });
    const clsSel = h('select', { onchange: () => { clsFilter = clsSel.value; drawTable(true); } }, h('option', { value: '' }, tr('Todo')));
    const aliveBox = h('input', { type: 'checkbox', checked: true, onchange: () => { onlyAlive = aliveBox.checked; drawTable(true); } });
    /* the table is drawn again every second: the clicks are taken on its
     * box, which stays, at pointerdown, so a redraw between pressing and
     * releasing does not swallow them */
    const tableBox = h('div', { class: 'bl-wrap' });
    tableBox.addEventListener('pointerdown', ev => {
      const th = ev.target.closest('th[data-k]'), row = ev.target.closest('tr[data-a]');
      if (th) {
        const k = th.dataset.k;
        if (sortKey === k) sortDir = -sortDir; else { sortKey = k; sortDir = ['r', 'fav', 'adv'].includes(k) ? -1 : 1; }
        drawTable(true);
      } else if (row) select(row.dataset.a);
    });
    const exportBtns = h('div', { class: 'btns' },
      h('button', { class: 'btn', onclick: () => exportCsv() }, tr('Bajar la tabla (CSV)')),
      h('button', { class: 'btn', onclick: () => P.saveBlob(new Blob([JSON.stringify(devs, null, 1)], { type: 'application/json' }), `ble-${Date.now()}.json`) }, tr('Bajar todo (JSON)')));
    const list = h('div', { class: 'card' }, h('h2', {}, tr('Equipos')),
      h('div', { class: 'row', style: 'gap:10px;flex-wrap:wrap' }, search, clsSel, h('label', { class: 'row', style: 'gap:6px' }, aliveBox, tr('Sólo los que se oyen'))),
      tableBox, exportBtns);

    /* ---- one device ---- */
    const detail = h('div', { class: 'card', style: 'display:none' });
    /* ---- the sensors, and their files ---- */
    const sensors = h('div', { class: 'card' });
    const files = h('div', { class: 'card' });

    put(main, missing, top, sensors, list, detail, files);

    function showMissing(m) {
      missing.style.display = m ? '' : 'none';
      top.style.display = list.style.display = sensors.style.display = m ? 'none' : '';
      if (m) put(missing, h('h2', {}, 'BLE'), h('p', { class: 'note' }, tr('La app BLE no está abierta en la placa: los datos en vivo salen de ella.')),
        h('button', { class: 'btn pri', onclick: () => P.openApp(APP).then(() => P.toast(tr('Abriendo BLE en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir BLE en la placa')));
    }

    function drawControls(s) {
      if (!changed('ctl', [s.paused, s.active, s.duty, s.env, s.min_rssi, s.csv, s.mqtt, s.mqtt_ready, s.bt_off].join())) return;
      const select = (cmd, cur, opts) => h('select', { onchange: ev => send(cmd + '=' + ev.target.value) },
        ...opts.map(([v, t]) => h('option', { value: v, selected: v === cur }, t)));
      const sw = (label, on, cmd) => h('label', { class: 'row', style: 'gap:6px' },
        h('input', { type: 'checkbox', checked: on, onchange: ev => send(cmd + '=' + (ev.target.checked ? 1 : 0)) }), label);
      put(controls,
        h('button', { class: 'btn' + (s.paused ? ' pri' : ''), onclick: () => send('pause=' + (s.paused ? 0 : 1)) }, s.paused ? tr('Seguir') : tr('Pausa')),
        h('span', {}, tr('Escaneo') + ' ', select('active', s.active ? 1 : 0, [[1, tr('activo')], [0, tr('pasivo')]])),
        h('span', {}, tr('Escuchando') + ' ', select('duty', s.duty, [[10, '10 %'], [30, '30 %'], [60, '60 %'], [100, '100 %']])),
        h('span', {}, tr('Entorno') + ' ', select('env', s.env, [[0, tr('al aire libre')], [1, tr('casa')], [2, tr('oficina')]])),
        h('span', {}, tr('Señal mínima') + ' ', select('minrssi', s.min_rssi, [[-100, tr('todos')], [-90, '-90 dBm'], [-80, '-80 dBm'], [-70, '-70 dBm']])),
        sw(tr('CSV de los sensores'), s.csv, 'csv'),
        sw(s.mqtt_ready ? 'MQTT' : tr('MQTT (no conectado)'), s.mqtt, 'mqtt'),
        h('button', { class: 'btn', onclick: () => { if (confirm(tr('¿Vaciar la lista? Los favoritos quedan.'))) send('forget=1'); } }, tr('Vaciar la lista')));
    }

    function drawLost(s) {
      if (!changed('lost', s.lost + '|' + Math.floor((s.lost_age || 0) / 60000))) return;
      if (!s.lost) { lostBox.style.display = 'none'; return; }
      /* what went down, by its bits: 1 the phone, 2 the computer, 4 the wifi */
      const what = ['', N_('el teléfono'), N_('la computadora'), N_('el teléfono y la computadora'), N_('el Wi-Fi'),
        N_('el teléfono y el Wi-Fi'), N_('la computadora y el Wi-Fi'), N_('el teléfono, la computadora y el Wi-Fi')][s.lost & 7];
      const when = s.lost_age < 60000 ? tr('recién') : tr('hace %s', age(s.lost_age));
      lostBox.style.display = '';
      put(lostBox, h('div', {}, tr('⚠ Mientras escaneaba se desconectó %s (%s). El Bluetooth y el Wi-Fi de la placa comparten una sola radio: escuchar menos tiempo o pausar el escaneo les deja más aire.', tr(what), when)),
        h('div', { class: 'btns', style: 'margin-top:8px' }, h('button', { class: 'btn', onclick: () => send('lost_ok=1') }, tr('Entendido'))));
    }

    function drawChart(s) {
      const W = chart.clientWidth || 600, H = 140, dpr = window.devicePixelRatio || 1;
      if (chart.width !== W * dpr) { chart.width = W * dpr; chart.height = H * dpr; }
      const g = chart.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, W, H);
      const pk = s.pkts || [], dv = s.devs || [];
      const mp = Math.max(10, ...pk), md = Math.max(5, ...dv), n = pk.length || 1, bw = W / n;
      const accent = getComputedStyle(document.body).getPropertyValue('--accent') || '#0a84ff';
      g.fillStyle = accent;
      g.globalAlpha = .55;
      pk.forEach((v, i) => { const bh = v / mp * (H - 24); g.fillRect(i * bw, H - bh, Math.max(1, bw - 1), bh); });
      g.globalAlpha = 1;
      g.strokeStyle = '#FF9F0A';
      g.lineWidth = 2;
      g.beginPath();
      dv.forEach((v, i) => { const y = H - v / md * (H - 24); i ? g.lineTo(i * bw, y) : g.moveTo(i * bw, y); });
      g.stroke();
      g.font = '12px sans-serif';
      g.fillStyle = accent;
      g.fillText(tr('paquetes/s (máx. %s)', mp), 6, 14);
      g.fillStyle = '#FF9F0A';
      g.textAlign = 'right';
      g.fillText(tr('equipos/s (máx. %s)', md), W - 6, 14);
      g.textAlign = 'left';
    }

    function showState(s) {
      st = s;
      showMissing(false);
      status.textContent = s.bt_off ? tr('Bluetooth apagado en la placa (se prende en Ajustes o desde la app).')
        : [tr('%s cerca', s.alive), tr('%s vistos', s.seen ?? s.known), tr('%s paquetes/s', nf(s.pps)), tr('%s en total', s.total), tr('%s perdidos', s.lost),
          s.paused ? tr('en pausa') : (s.active ? tr('activo') : tr('pasivo')) + ` ${s.duty} %`].join(' · ')
          /* the table's top, and what fits in one answer of the live channel */
          + (s.forgotten ? ' · ' + tr('la placa guarda %s equipos y olvidó %s para hacer lugar', s.cap, s.forgotten) : '')
          + (s.listed !== undefined && s.listed < s.known ? ' · ' + tr('la tabla de abajo muestra los %s más fuertes de %s', s.listed, s.known) : '');
      drawControls(s);
      drawLost(s);
      drawChart(s);
    }

    /* ---- the table ---- */
    const COLS = [
      ['fav', '★', d => d.fav ? 1 : 0], ['name', N_('Nombre'), d => devName(d).toLowerCase()], ['cls', N_('Qué es'), d => appTr(d.cls)],
      ['co', N_('Fabricante'), d => d.co || ''], ['a', N_('Dirección'), d => d.a], ['r', N_('Señal'), d => d.age < 30000 ? d.ra : -200],
      ['dist', N_('Distancia'), d => d.dist], ['iv', N_('Intervalo'), d => d.iv || 1e9], ['adv', N_('Paquetes'), d => d.adv + d.rsp],
      ['age', N_('Oído'), d => d.age], ['sub', N_('Datos'), d => d.sub],
    ];
    const HIDE_S = new Set(['co', 'dist', 'iv', 'adv']);

    function visible() {
      let v = devs;
      if (onlyAlive) v = v.filter(d => d.age < 30000 || d.fav);
      if (clsFilter) v = v.filter(d => d.cls === clsFilter);
      if (query) v = v.filter(d => [devName(d), d.n, d.a, d.co, appTr(d.lbl), appTr(d.cls), d.sub].join(' ').toLowerCase().includes(query));
      const col = COLS.find(c => c[0] === sortKey) || COLS[5];
      return v.slice().sort((x, y) => { const a = col[2](x), b = col[2](y); return (a < b ? -1 : a > b ? 1 : 0) * sortDir; });
    }

    function drawTable(force) {
      const v = visible();
      const sig = v.map(d => [d.a, d.r, d.age > 30000, d.fav, d.al, d.n, d.sub, d.adv].join('|')).join(';') + sel + sortKey + sortDir;
      if (!force && !changed('table', sig)) return;
      const clses = [...new Set(devs.map(d => d.cls))].sort((a, b) => String(appTr(a) ?? '').localeCompare(String(appTr(b) ?? '')));
      if (changed('cls', clses.join())) {
        put(clsSel, h('option', { value: '' }, tr('Todo')), ...clses.map(c => h('option', { value: c, selected: c === clsFilter }, appTr(c))));
      }
      const head = h('tr', {}, ...COLS.map(([k, t]) => h('th', {
        class: (HIDE_S.has(k) ? 'bl-hide-s' : '') + (['r', 'dist', 'iv', 'adv', 'age'].includes(k) ? ' num' : ''), 'data-k': k,
      }, tr(t) + (sortKey === k ? (sortDir < 0 ? ' ▾' : ' ▴') : ''))));
      const rows = v.map(d => {
        const gone = d.age >= 30000;
        const w = Math.max(4, Math.min(60, (d.r + 100) * 0.85));
        return h('tr', { class: 'k' + (d.a === sel ? ' sel' : '') + (gone ? ' gone' : ''), 'data-a': d.a },
          h('td', {}, d.fav ? '★' : ''),
          h('td', {}, h('span', { class: 'bl-dot', style: `background:${CLS_COLOR[d.cls] || '#8e8e93'}` }), devName(d),
            d.al && d.n ? h('div', { class: 'sub' }, d.n) : null),
          h('td', {}, appTr(d.cls), d.lbl && appTr(d.lbl) !== devName(d) ? h('div', { class: 'sub' }, appTr(d.lbl)) : null),
          h('td', { class: 'bl-hide-s' }, d.co || '', d.cid !== undefined ? h('div', { class: 'sub' }, '0x' + d.cid.toString(16).toUpperCase().padStart(4, '0')) : null),
          h('td', {}, h('span', { class: 'mono small' }, d.a), h('div', { class: 'sub' }, appTr(d.k))),
          h('td', { class: 'num' }, gone ? '—' : d.r + ' dBm', gone ? null : h('span', { class: 'bl-bar', style: `width:${w}px;background:${rssiColor(d.r)}` })),
          h('td', { class: 'num bl-hide-s' }, '≈ ' + nf(d.dist, d.dist < 10 ? 1 : 0) + ' m'),
          h('td', { class: 'num bl-hide-s' }, ivText(d.iv)),
          h('td', { class: 'num bl-hide-s' }, d.adv + (d.rsp ? ' + ' + d.rsp : '')),
          h('td', { class: 'num' }, age(d.age)),
          h('td', { class: 'sub' }, d.sen ? senLine(d.sen) : appTr(d.sub)));
      });
      put(tableBox, h('table', { class: 'bl-tbl' }, h('thead', {}, head), h('tbody', {}, ...rows)),
        v.length ? null : h('p', { class: 'note' }, devs.length ? tr('Nadie con este filtro.') : tr('Escuchando...')));
    }

    function exportCsv() {
      const esc = s => '"' + String(s ?? '').replace(/"/g, '""') + '"';
      const lines = ['direccion,tipo_dir,nombre,alias,clase,fabricante,rssi,rssi_prom,rssi_min,rssi_max,distancia_m,intervalo_ms,anuncios,respuestas,oido_hace_ms,datos'];
      for (const d of devs) lines.push([d.a, d.k, d.n, d.al, d.cls, d.co, d.r, d.ra, d.rmin, d.rmax, d.dist, d.iv, d.adv, d.rsp, d.age, d.sen ? senLine(d.sen) : d.sub].map(esc).join(','));
      P.saveBlob(new Blob([lines.join('\n') + '\n'], { type: 'text/csv' }), `ble-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.csv`);
    }

    /* ---- the sensors ---- */
    function drawSensors() {
      const ss = devs.filter(d => d.sen);
      const sig = ss.map(d => [d.a, senLine(d.sen), d.al, d.age > 120000].join('|')).join(';');
      if (!changed('sen', sig)) return;
      put(sensors, h('h2', {}, tr('Sensores')),
        ss.length ? h('div', { class: 'bl-grid' }, ...ss.map(d => h('div', { class: 'bl-sen', onclick: () => select(d.a), style: d.age > 120000 ? 'opacity:.5' : '' },
          h('div', {}, devName(d)), h('div', { class: 'small muted' }, `${d.sen.format} · ${d.a}`),
          h('div', { class: 'big' }, d.sen.temp !== undefined ? nf(d.sen.temp) + '°' : d.sen.encrypted ? tr('cifrado') : ''),
          h('div', {}, d.sen.hum !== undefined ? tr('%s de humedad', nf(d.sen.hum, 0) + ' %') : ''),
          h('div', { class: 'small muted' }, senLine({ ...d.sen, temp: undefined, hum: undefined }) + ' · ' + age(d.age)))))
          : h('p', { class: 'note' }, tr('Ninguno a la vista. Aparecen solos los que anuncian en BTHome, pvvx/ATC, MiBeacon sin cifrar, Govee, Ruuvi, SwitchBot, Qingping, Inkbird o Eddystone TLM.')));
    }

    /* ---- one device ---- */
    function select(a) {
      sel = sel === a ? null : a;
      dev = null;
      drawn.detail = drawn.detailHead = null;
      send('sel=' + (sel || ''));
      drawTable(true);
      drawDetail();
      if (sel) setTimeout(() => detail.scrollIntoView({ behavior: 'smooth', block: 'start' }), 50);
    }

    const sigCanvas = h('canvas', { class: 'bl-canvas', height: 120 });
    const tempCanvas = h('canvas', { class: 'bl-canvas', height: 120 });
    const detailHead = h('div', {});
    const detailBody = h('div', {});

    function drawLine(cv, vals, lo, hi, color, bars) {
      const W = cv.clientWidth || 600, H = 120, dpr = window.devicePixelRatio || 1;
      if (cv.width !== W * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
      const g = cv.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, W, H);
      const n = vals.length, bw = W / n;
      g.fillStyle = g.strokeStyle = color;
      g.lineWidth = 2;
      g.beginPath();
      let pen = false;
      vals.forEach((v, i) => {
        if (v === null || v === 0) { pen = false; return; }
        const y = H - 6 - (v - lo) / (hi - lo) * (H - 12);
        if (bars) g.fillRect(i * bw, y, Math.max(1, bw - 1), H - y);
        else { pen ? g.lineTo(i * bw, y) : g.moveTo(i * bw, y); pen = true; }
      });
      if (!bars) g.stroke();
      g.font = '11px sans-serif';
      g.fillStyle = '#8e8e93';
      g.fillText(nf(hi, bars ? 0 : 1), 4, 12);
      g.fillText(nf(lo, bars ? 0 : 1), 4, H - 4);
    }

    /* the key of a device that encrypts: typed here, kept by the app in
     * ble/claves.txt; the input is made once per device so typing is not
     * undone by the redraws */
    const keyBox = h('div', {});

    /* ---- the GATT explorer, on the board's connection: the page asks
     * (gatt=<address>, gatt_readall, gatt_read, gatt_sub, gatt_write,
     * gatt_mi for a Xiaomi's activation) and draws what the app puts as
     * "gatt" ---- */
    let gatt = null;
    const gattBox = h('div', {});
    const GATT_STATE = [N_('Desconectado'), N_('Conectando...'), N_('Conectado, listando servicios...'), N_('Conectado'), N_('Sin conexión')];
    const propsText = p => [[2, N_('lee')], [8, N_('escribe')], [4, N_('escribe sin respuesta')], [16, N_('notifica')], [32, N_('indica')]]
      .filter(([b]) => p & b).map(([, t]) => tr(t)).join(' · ');
    gattBox.addEventListener('pointerdown', ev => {
      const b = ev.target.closest('[data-g]');
      if (!b) return;
      const act = b.dataset.g, hd = b.dataset.h;
      if (act === 'connect') send('gatt=' + sel);
      else if (act === 'off') send('gatt=off');
      else if (act === 'all') send('gatt_readall=1');
      else if (act === 'read') send('gatt_read=' + hd);
      else if (act === 'sub') send(`gatt_sub=${hd},${b.dataset.m}`);
      else if (act === 'hilink') send('gatt_hilink=' + hd);
      else if (act === 'mi') {
        if (confirm(tr('Activar este sensor Xiaomi: le da una clave nueva y la guarda en la placa. El sensor olvida la que tenía: Mi Home y quien lo lea con la clave vieja (Home Assistant, por ejemplo) van a necesitar la nueva. ¿Seguir?')))
          send('gatt_mi=1');
      }
      else if (act === 'write') {
        const v = prompt(tr('Escribir: texto, o bytes con 0x (por ejemplo 0x01 A0)'));
        if (v !== null && v !== '') send(`gatt_write=${hd},${v}`);
      }
    });
    function drawGatt() {
      const g = gatt && gatt.a === sel ? gatt : null;
      if (!changed('gatt', JSON.stringify(g) + sel)) return;
      const btn = (t, act, extra = {}, cls = 'btn') => h('button', { class: cls, 'data-g': act, ...extra }, t);
      if (!g || (!g.open && g.state === 0)) {
        put(gattBox, h('h3', {}, 'GATT'), h('p', { class: 'note' }, tr('Conectarse para ver sus servicios y características, leerlas, escribirlas y escuchar sus notificaciones. Mientras tanto la placa deja de escanear un momento; muchos equipos aceptan una sola conexión a la vez.')),
          h('div', { class: 'btns' }, btn(tr('Conectar por GATT'), 'connect', {}, 'btn pri')));
        return;
      }
      let st = GATT_STATE[g.state] ? tr(GATT_STATE[g.state]) : '';
      if (g.state === 3) st += ` · MTU ${g.mtu}` + (g.rssi !== null ? ' · ' + tr('señal %s dBm', g.rssi) : '') + (g.reading[0] < g.reading[1] ? ' · ' + tr('leyendo %s de %s', g.reading[0] + 1, g.reading[1]) : '');
      if (g.state === 4) st += `: ${appTr(g.why)} (${g.reason})`;
      const live = g.state >= 1 && g.state <= 3;
      const rows = [];
      for (const a of g.attrs) {
        if (a.k === 0) {
          rows.push(h('tr', {}, h('td', { colspan: 3, style: 'padding-top:14px' }, h('b', { style: 'color:var(--accent)' }, a.n), h('div', { class: 'sub' }, tr('Servicio %s · handles %s a %s', a.u, a.h, a.end)))));
          continue;
        }
        const desc = a.k === 2;
        const acts = [];
        if (!desc && (a.p & 2)) acts.push(btn(tr('Leer'), 'read', { 'data-h': a.h }));
        if (!desc && (a.p & 12)) acts.push(btn(tr('Escribir'), 'write', { 'data-h': a.h }));
        if (a.hl) acts.push(btn(tr('Pedir los datos (Hi-Link)'), 'hilink', { 'data-h': a.h }, 'btn pri'));
        if (!desc && (a.p & 48)) acts.push(btn(a.sub ? tr('Dejar de escuchar') : (a.p & 16) ? tr('Notificaciones') : tr('Indicaciones'), 'sub',
          { 'data-h': a.h, 'data-m': a.sub ? 0 : (a.p & 16) ? 1 : 2 }, a.sub ? 'btn pri' : 'btn'));
        rows.push(h('tr', {},
          h('td', { style: desc ? 'padding-left:22px' : '' }, desc ? h('span', { class: 'sub' }, a.n) : a.n,
            h('div', { class: 'sub' }, `${a.u} · ${a.h}` + (desc ? '' : ' · ' + propsText(a.p)))),
          h('td', { class: 'bl-hex', style: `white-space:pre-wrap;color:${a.s ? 'var(--orange)' : 'var(--fg, inherit)'}` }, a.v + (a.c > 1 ? '' : '')),
          h('td', { style: 'white-space:nowrap' }, ...acts)));
      }
      /* a Xiaomi with its auth service: the activation (bl_mi.c) */
      const mi = g.mi || {};
      let miBox = null;
      if (mi.st >= 1 && mi.st <= 5) miBox = h('p', { class: 'note' }, tr('Activando: %s...', appTr(mi.step)) + (mi.id ? ` (${mi.id})` : ''));
      else if (mi.st === 6) miBox = h('div', { class: 'note' }, h('b', { style: 'color:var(--green)' }, tr('Sensor activado.') + ' '),
        tr('Clave nueva (bindkey), ya guardada en la placa para leerlo:') + ' ', h('code', { style: 'user-select:all' }, mi.key || ''),
        mi.id ? h('div', { class: 'sub' }, mi.id) : null);
      else if (mi.st === 7) miBox = h('p', { class: 'note', style: 'color:var(--orange)' }, tr('No se activó: %s', appTr(mi.why)) + (mi.code ? ` (0x${mi.code.toString(16).toUpperCase()})` : ''));
      put(gattBox, h('h3', {}, 'GATT'), h('p', { class: 'note', style: g.state === 4 ? 'color:var(--orange)' : '' }, st),
        h('div', { class: 'btns' }, g.state === 3 ? btn(tr('Leer todo'), 'all', {}, 'btn pri') : null,
          mi.can && !(mi.st >= 1 && mi.st <= 6) ? btn(mi.st === 7 ? tr('Probar de nuevo la activación') : tr('Activar (Xiaomi)'), 'mi') : null,
          live ? btn(tr('Desconectar'), 'off') : btn(tr('Conectar de nuevo'), 'connect')),
        miBox,
        rows.length ? h('div', { class: 'bl-wrap' }, h('table', { class: 'bl-tbl' }, h('tbody', {}, ...rows))) : null,
        g.log.length ? h('details', {}, h('summary', { class: 'small muted' }, tr('Lo último')), h('div', { class: 'small muted', style: 'white-space:pre-line' }, g.log.join('\n'))) : null);
    }
    async function gattTick() {
      if (!alive || !sel) return;
      const d = devs.find(x => x.a === sel);
      if (!d || !(d.conn || (gatt && gatt.a === sel))) return;
      try { gatt = await P.api(LIVE + '&key=gatt'); } catch { /* none yet */ }
      drawGatt();
    }
    function drawKey(d) {
      if (!changed('key', [d.a, d.key].join('|'))) return;
      const inp = h('input', { placeholder: tr('32 cifras hexadecimales (la bindkey)'), style: 'width:340px;font-family:ui-monospace,Menlo,monospace', maxlength: 47 });
      const st = d.key === -2 ? tr('Sin clave: sus lecturas van cifradas.') : d.key === 0 ? tr('Clave correcta: las lecturas se descifran.')
        : d.key === 2 ? tr('La clave no coincide con lo que anuncia.') : d.key === 3 ? tr('Usa un cifrado viejo (MiBeacon v2/v3) que la app no descifra.')
        : tr('Clave cargada: esperando un paquete cifrado para probarla.');
      put(keyBox, h('h3', {}, tr('Clave')), h('p', { class: 'note', style: d.key === 0 ? 'color:var(--green)' : d.key >= 2 ? 'color:var(--orange)' : '' }, st),
        h('div', { class: 'btns' }, inp,
          h('button', { class: 'btn pri', onclick: () => {
            const k = inp.value.replace(/[\s:-]/g, '');
            if (!/^[0-9a-fA-F]{32}$/.test(k)) { P.toast(tr('Tienen que ser 32 cifras hexadecimales'), true); return; }
            /* the app confirms by the device's state in the next table:
             * until then, only that it was sent */
            send(`key=${d.a},${k}`).then(() => P.toast(tr('Clave enviada: la app la guarda y la prueba con el próximo paquete')));
          } }, tr('Guardar la clave')),
          d.key > -2 ? h('button', { class: 'btn', onclick: () => send(`key=${d.a},`) }, tr('Borrar la clave')) : null),
        h('p', { class: 'note small' }, tr('La bindkey de un sensor Xiaomi sale de su cuenta de Mi Home (por ejemplo con "Xiaomi Cloud Tokens Extractor"), o de Home Assistant si ya lo lee. BTHome cifrado usa la clave que le pusiste al configurarlo.')));
    }

    /* the app's explanation of a packet, its keys in the board's language */
    const explained = ls => (ls || []).map(([k, v]) => [appTr(k), appTr(v)]);
    function lines(ls) {
      return h('div', { class: 'bl-kv' }, ...ls.flatMap(([k, v]) => [h('div', { class: 'k' }, k), h('div', {}, v)]));
    }

    function drawDetail() {
      const d = devs.find(x => x.a === sel);
      if (!sel || !d) { detail.style.display = 'none'; return; }
      detail.style.display = '';
      if (changed('detailHead', [sel, d.fav, d.al, d.n].join('|'))) {
        const alias = h('input', { value: d.al || '', placeholder: tr('Un nombre propio'), maxlength: 27, style: 'width:220px' });
        put(detailHead, h('h2', {}, devName(d)),
          h('div', { class: 'btns' },
            h('button', { class: 'btn' + (d.fav ? ' pri' : ''), onclick: () => send(`fav=${d.a},${d.fav ? 0 : 1}`) }, d.fav ? tr('★ Favorito') : tr('☆ Marcar')),
            alias, h('button', { class: 'btn', onclick: () => send(`alias=${d.a},${alias.value.replace(/[\n|]/g, ' ')}`) }, tr('Guardar el nombre')),
            h('button', { class: 'btn', onclick: () => select(d.a) }, tr('Cerrar'))));
        put(detail, detailHead, detailBody);
      }
      drawKey(d);
      const sig = JSON.stringify([d.r, d.age, d.adv, d.key, d.sen && d.sen.encrypted, dev && dev.adv, dev && dev.rsp, dev && dev.hist && dev.hist.slice(-3)]);
      if (!changed('detail', sig)) return;
      const info = [[tr('Qué es'), appTr(d.cls) + (d.lbl ? ' · ' + appTr(d.lbl) : '')], [tr('Dirección'), `${d.a} (${appTr(d.k)})`]];
      if (d.co) info.push([tr('Fabricante'), `${d.co}` + (d.cid !== undefined ? ` (0x${d.cid.toString(16).toUpperCase().padStart(4, '0')})` : '')]);
      info.push([tr('Señal'), d.age < 10000 ? tr('%s dBm · prom. %s · mín. %s · máx. %s', d.r, nf(d.ra, 0), d.rmin, d.rmax) : '—'],
        [tr('Distancia'), tr('≈ %s m (estimada por la señal)', nf(d.dist))], [tr('Intervalo'), ivText(d.iv) || '?'],
        [tr('Paquetes'), tr('%s anuncios, %s respuestas, cambió %s veces', d.adv, d.rsp, d.chg)], [tr('Oído'), tr('hace %s · desde hace %s', age(d.age), age(d.seen))],
        [tr('Conectable'), d.conn ? tr('sí (abajo, GATT)') : tr('no')]);
      const parts = [lines(info), h('h3', {}, tr('Señal, dos minutos')), sigCanvas];
      if ((d.sen && d.sen.encrypted) || d.key > -2) parts.push(keyBox);
      if (d.conn || (gatt && gatt.a === sel)) parts.push(gattBox);
      if (d.sen) parts.push(h('h3', {}, tr('Sensor') + ' · ' + d.sen.format), lines(Object.entries(d.sen).filter(([k]) => k !== 'format').map(([k, v]) => [k, String(v)])));
      if (dev && dev.temps && dev.temps.some(v => v !== null)) parts.push(h('h3', {}, tr('Temperatura, dos horas')), tempCanvas);
      if (dev && dev.beacon) {
        const b = dev.beacon;
        const kinds = ['', 'iBeacon', 'AltBeacon', 'Eddystone UID', 'Eddystone URL', 'Eddystone TLM', 'Eddystone EID'];
        parts.push(h('h3', {}, tr('Baliza') + ' · ' + (kinds[b.kind] || '')), lines([['UUID', b.uuid], [tr('Mayor / menor'), `${b.major} / ${b.minor}`], [tr('Potencia de referencia'), b.tx1m + ' dBm']].concat(b.url ? [['URL', b.url]] : [])));
      }
      if (dev) {
        if (dev.adv) parts.push(h('h3', {}, tr('Anuncio · %s bytes', dev.adv.length / 2)), lines(explained(dev.adv_lines)), h('div', { class: 'bl-hex' }, dev.adv.replace(/(..)/g, '$1 ')));
        if (dev.rsp) parts.push(h('h3', {}, tr('Respuesta al escaneo · %s bytes', dev.rsp.length / 2)), lines(explained(dev.rsp_lines)), h('div', { class: 'bl-hex' }, dev.rsp.replace(/(..)/g, '$1 ')));
      } else parts.push(h('p', { class: 'note' }, tr('Pidiendo los paquetes a la placa...')));
      put(detailBody, ...parts);
      drawn.gatt = null;
      drawGatt();
      if (dev && dev.hist) drawLine(sigCanvas, dev.hist.map(v => v || null), -100, -30, 'var(--accent)', true);
      if (dev && dev.temps) {
        const t = dev.temps.filter(v => v !== null);
        if (t.length) { let lo = Math.min(...t), hi = Math.max(...t); if (hi - lo < 1) { lo -= .5; hi += .5; } drawLine(tempCanvas, dev.temps, lo, hi, '#FF9F0A', false); }
      }
    }

    async function loadFiles() {
      const fl = await P.fsList('/ble').catch(() => []);
      const csv = fl.filter(f => f.name.endsWith('.csv') && !f.name.startsWith('.')).sort((a, b) => b.name.localeCompare(a.name));
      if (!changed('files', csv.map(f => f.name + f.size).join())) return;
      put(files, h('h2', {}, tr('Lecturas guardadas (CSV)')),
        csv.length ? null : h('p', { class: 'note' }, tr('Todavía no hay. Se prende con "CSV de los sensores", arriba o en los ajustes de la app.')),
        ...csv.map(f => h('div', { class: 'row' }, h('div', { class: 'grow' }, f.name, h('span', { class: 'small muted' }, ' ' + P.fmtBytes(f.size))),
          h('a', { class: 'btn', href: P.fsUrl('/ble/' + f.name, true) }, tr('Bajar')),
          h('button', { class: 'btn red', onclick: async () => { if (!confirm(tr('¿Borrar %s?', f.name))) return; await P.fsDelete('/ble/' + f.name).catch(e => P.toast(e.message, true)); drawn.files = null; loadFiles(); } }, '✕'))));
    }

    /* ---- polling ---- */
    async function tick() {
      try {
        showState(await P.api(LIVE + '&key=state'));
      } catch { showMissing(true); return; }
      try { devs = await P.api(LIVE + '&key=devices'); } catch { /* not put yet */ }
      if (sel) { try { dev = await P.api(LIVE + '&key=dev'); if (dev.a !== sel) dev = null; } catch { dev = null; } }
      drawTable(false);
      drawSensors();
      drawDetail();
    }
    showMissing(true);
    let busy = false;
    const loop = async () => { if (busy || !alive) return; busy = true; await tick(); busy = false; };
    loop();
    loadFiles();
    const t1 = setInterval(loop, 1000), t2 = setInterval(loadFiles, 20000), t3 = setInterval(gattTick, 400);
    return () => { alive = false; clearInterval(t1); clearInterval(t2); clearInterval(t3); };
  },
});
