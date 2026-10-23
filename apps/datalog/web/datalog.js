/* The Datalog app's page in the board's portal (docs/PORTAL-PAGES.md):
 * the lines and their latest readings, pause and resume, and each CSV
 * charted over a range, to download or delete.
 *
 * The latest readings come through the live channel (/api/live, key
 * "state"), which the app puts every second while someone looks; the page
 * sends "run=0|1" and "csv=comma|semicolon" the same way. The CSVs are
 * read straight from the card (/datalog/<id>.csv): a big one only by its
 * last 8 MB, with a Range request. The lines themselves are made on the
 * board; datalog.txt in /data is plain text too. */
const P = window.P4OS.forPage ? window.P4OS.forPage('datalog') : window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const LOC = { es: 'es-AR', en: 'en-US', de: 'de-DE', 'zh-Hans': 'zh-CN', 'zh-Hant': 'zh-TW' }[P.lang ? P.lang() : 'es'] || 'es-AR';
const APP = 'aos.datalog';
const LIVE = 'live?app=' + APP;
const DIR = '/datalog';
const TAIL = 8 * 1024 * 1024;
const num = (v, d) => (P.num ? P.num(v, d) : v.toFixed(d));

if (!document.getElementById('css-datalog')) {
  const st = document.createElement('style');
  st.id = 'css-datalog';
  st.textContent = `
.dlhead { display: flex; gap: 16px; align-items: center; flex-wrap: wrap; }
.dlhead .grow { flex: 1; }
.dlhead select { width: auto; }
.dldot { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; background: var(--dim); }
.dltbl { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
.dltbl th { text-align: left; font-weight: 500; color: var(--dim); font-size: 13px; padding: 8px 12px; }
.dltbl td { padding: 8px 12px; border-top: 1px solid var(--line); vertical-align: top; }
.dltbl tr.sel td { background: var(--card2); }
.dltbl tr { cursor: pointer; }
.dltbl .v { font-size: 20px; white-space: nowrap; }
.dltbl .hi { color: var(--red); }
.dltbl .lo { color: #64d2ff; }
.dltbl .err { color: var(--orange); }
.dlscroll { overflow-x: auto; }
.dlchart { position: relative; }
.dlchart canvas { width: 100%; height: 340px; display: block; touch-action: none; }
.dltip { position: absolute; pointer-events: none; background: var(--card2); border: 1px solid var(--line); border-radius: 8px; padding: 4px 8px; font-size: 13px; white-space: nowrap; display: none; }
.dlranges { display: flex; gap: 6px; flex-wrap: wrap; }
.dlranges .btn.on { background: var(--accent); color: #fff; }
.dlstats { display: flex; gap: 22px; flex-wrap: wrap; }
.dlstats b { font-size: 18px; font-variant-numeric: tabular-nums; }
`;
  document.head.append(st);
}

const RANGES = [[N_('1 h'), 3600], [N_('6 h'), 6 * 3600], [N_('24 h'), 86400], [N_('7 días'), 7 * 86400], [N_('30 días'), 30 * 86400], [N_('Todo'), 0]];

/* A CSV of the app: fecha,hora,valor,nota (or with ';' and decimal commas).
 * Times are the board's local time; Date.UTC keeps them as they are. */
function parseCsv(text) {
  const out = [];
  const lines = text.split('\n');
  let sep = ',';
  if (lines.length && lines[0].includes(';')) sep = ';';
  for (const ln of lines) {
    const m = /^(\d{4})-(\d\d)-(\d\d)[,;](\d\d):(\d\d):(\d\d)[,;]([^,;\r\n]*)[,;]?(.*)$/.exec(ln);
    if (!m) continue;
    const t = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) / 1000;
    let v = m[7] === '' ? NaN : parseFloat(sep === ';' ? m[7].replace(',', '.') : m[7]);
    out.push({ t, v, note: m[8].replace(/^"|"$/g, '').replace(/""/g, '"') });
  }
  return out;
}

/* a time of the CSV, for a span: seconds within an hour, the day beyond one */
const fmtT = (t, span) => new Date(t * 1000).toLocaleString(LOC, Object.assign({ timeZone: 'UTC', hourCycle: 'h23', hour: '2-digit', minute: '2-digit' },
  span === true || span > 86400 ? { day: '2-digit', month: 'short' } : span <= 3600 ? { second: '2-digit' } : {}));

function draw(cv, tip, pts, t0, t1, line) {
  const css = getComputedStyle(cv);
  const col = n => css.getPropertyValue(n).trim() || '#888';
  const dpr = window.devicePixelRatio || 1;
  const W = cv.clientWidth, H = cv.clientHeight;
  cv.width = W * dpr; cv.height = H * dpr;
  const g = cv.getContext('2d');
  g.scale(dpr, dpr);
  g.clearRect(0, 0, W, H);
  const L = 58, R = 12, T = 12, B = 28;
  const vals = pts.filter(p => !isNaN(p.v));
  g.font = '12px system-ui, sans-serif';
  g.fillStyle = col('--dim');
  if (!vals.length) { g.fillText(tr('No hay lecturas en este rango.'), L, H / 2); return null; }
  let lo = Math.min(...vals.map(p => p.v)), hi = Math.max(...vals.map(p => p.v));
  for (const a of [line && line.hi, line && line.lo]) if (a != null && a >= lo - (hi - lo) && a <= hi + (hi - lo) * 2) { lo = Math.min(lo, a); hi = Math.max(hi, a); }
  if (hi - lo < 1e-6) { lo -= 0.5; hi += 0.5; } else { const p = (hi - lo) * 0.08; lo -= p; hi += p; }
  const X = t => L + (t - t0) / Math.max(1, t1 - t0) * (W - L - R);
  const Y = v => T + (hi - v) / (hi - lo) * (H - T - B);
  /* the grid */
  g.strokeStyle = col('--line');
  g.lineWidth = 1;
  const dec = Math.max(0, Math.min(4, 2 - Math.floor(Math.log10(hi - lo))));
  for (let i = 0; i <= 4; i++) {
    const v = lo + (hi - lo) * i / 4, y = Y(v);
    g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke();
    g.fillText(num(v, dec), 4, y + 4);
  }
  for (let i = 0; i <= 4; i++) {
    const t = t0 + (t1 - t0) * i / 4, x = X(t);
    const s = fmtT(t, t1 - t0);
    g.fillText(s, Math.min(Math.max(x - g.measureText(s).width / 2, L), W - R - g.measureText(s).width), H - 8);
  }
  /* the alarms */
  for (const [a, c] of [[line && line.hi, col('--red')], [line && line.lo, '#64d2ff']]) {
    if (a == null || a < lo || a > hi) continue;
    g.strokeStyle = c; g.setLineDash([6, 4]);
    g.beginPath(); g.moveTo(L, Y(a)); g.lineTo(W - R, Y(a)); g.stroke();
    g.setLineDash([]);
  }
  /* the readings: a gap longer than five of the usual ones is a hole */
  const steps = [];
  for (let i = 1; i < vals.length; i++) steps.push(vals[i].t - vals[i - 1].t);
  steps.sort((a, b) => a - b);
  const usual = steps.length ? steps[steps.length >> 1] : 1;
  g.strokeStyle = col('--accent');
  g.lineWidth = 2;
  g.beginPath();
  let prev = null;
  for (const p of vals) {
    if (!prev || p.t - prev.t > usual * 5) g.moveTo(X(p.t), Y(p.v)); else g.lineTo(X(p.t), Y(p.v));
    prev = p;
  }
  g.stroke();
  if (vals.length < 60) { g.fillStyle = col('--accent'); for (const p of vals) { g.beginPath(); g.arc(X(p.t), Y(p.v), 3, 0, 7); g.fill(); } }
  return { X, Y, vals };
}

P.registerPage({
  id: 'datalog',
  name: tr('Datalog'),
  icon: '∿',
  render(main) {
    const { h, put } = P;
    const head = h('div', { class: 'card pad' });
    const table = h('div', { class: 'card dlscroll' });
    const chartCard = h('div', { class: 'card pad' });
    const files = h('div', { class: 'card' });
    put(main, h('h1', {}, tr('Datalog')), head, table, chartCard, files);
    let state = null, sel = null, range = 2, gone = false, timer = 0, data = null, missing = false;
    let headKey = '', tableKey = '', cells = {};

    const send = text => P.api(LIVE, { method: 'POST', body: text }).then(() => { headKey = ''; setTimeout(tick, 400); }).catch(e => P.toast(e.message, true));

    function showHead() {
      /* drawn again only when it changes: a select that is open would close */
      const key = state ? [state.running, state.semicolon, state.clock, state.lines.length].join() : 'none';
      if (key === headKey) return;
      headKey = key;
      if (!state) {
        put(head, h('div', { class: 'dlhead' },
          h('div', { class: 'grow' }, h('b', {}, tr('La app no está abierta en la placa')),
            h('div', { class: 'muted small' }, tr('Los archivos se ven igual; los valores en vivo, cuando la abras.'))),
          h('button', { class: 'btn pri', onclick: () => P.openApp(APP).then(() => setTimeout(tick, 1500)).catch(e => P.toast(e.message, true)) }, tr('Abrir en la placa'))));
        return;
      }
      const run = state.running;
      put(head, h('div', { class: 'dlhead' },
        h('div', { class: 'grow' },
          h('span', { class: 'dldot', style: `background: var(${run ? '--green' : '--orange'})` }),
          h('b', {}, run ? tr('Registrando') : tr('En pausa')),
          h('div', { class: 'muted small' }, state.clock ? tr('%s lecturas configuradas', state.lines.length) : tr('La placa todavía no tiene hora de la red: anota el tiempo desde el arranque.'))),
        h('select', { onchange: e => send('csv=' + e.target.value) },
          h('option', { value: 'comma', selected: !state.semicolon }, tr('CSV con coma')),
          h('option', { value: 'semicolon', selected: state.semicolon }, tr('CSV con punto y coma'))),
        h('button', { class: 'btn ' + (run ? '' : 'pri'), onclick: () => send(run ? 'run=0' : 'run=1') }, run ? tr('Pausar') : tr('Registrar'))));
    }

    /* the rows are made when the lines change; each tick only writes their cells */
    function showTable() {
      if (!state) { put(table); table.style.display = 'none'; tableKey = ''; return; }
      table.style.display = '';
      const key = state.lines.map(l => l.id + '/' + l.name + '/' + l.src + '/' + l.sched).join('|') + '#' + sel;
      if (key !== tableKey) {
        tableKey = key;
        cells = {};
        if (!state.lines.length) { put(table, h('div', { class: 'pad muted' }, tr('No hay lecturas: se agregan en la placa, con el + de la app.'))); return; }
        const rows = state.lines.map(l => {
          const c = cells[l.id] = { v: h('td', { class: 'v' }), when: h('div', { class: 'muted small' }), note: h('td', { class: 'muted small' }) };
          return h('tr', { class: sel === l.id ? 'sel' : '', onclick: () => pick(l.id) },
            h('td', {}, h('b', {}, l.name), h('div', { class: 'muted small' }, l.src)),
            c.v,
            h('td', {}, l.sched, c.when),
            c.note);
        });
        put(table, h('table', { class: 'dltbl' },
          h('tr', {}, h('th', {}, tr('Lectura')), h('th', {}, tr('Valor')), h('th', {}, tr('Cuándo')), h('th', {}, tr('Alarmas y errores'))),
          ...rows));
      }
      for (const l of state.lines) {
        const c = cells[l.id];
        if (!c) continue;
        c.v.textContent = l.v == null ? '--' : num(l.v, Math.min(l.dec, 3)) + (l.unit ? ' ' + l.unit : '');
        c.v.className = l.alarm === 2 ? 'v hi' : l.alarm === 1 ? 'v lo' : 'v';
        c.when.textContent = l.when;
        const lim = [l.lo != null ? tr('baja < %s', num(l.lo, 2)) : null, l.hi != null ? tr('alta > %s', num(l.hi, 2)) : null].filter(Boolean).join(' · ');
        c.note.textContent = l.err || lim || '';
        c.note.className = l.err ? 'err small' : 'muted small';
      }
    }

    async function tick() {
      if (gone) return;
      clearTimeout(timer);
      try {
        state = await P.api(LIVE + '&key=state');
        missing = false;
      } catch (e) {
        state = null;
        missing = true;
      }
      showHead();
      showTable();
      if (!gone) timer = setTimeout(tick, 1500);
    }

    /* ---- the files ---- */
    async function showFiles() {
      const list = (await P.fsList(DIR)).filter(f => !f.dir && f.name.endsWith('.csv') && !f.name.startsWith('.'));
      list.sort((a, b) => a.name.localeCompare(b.name));
      if (!sel && list.length) pick(list[0].name.replace(/\.csv$/, ''));
      put(files, h('div', { class: 'pad' }, h('b', {}, tr('Archivos en %s', DIR))),
        ...list.map(f => {
          const id = f.name.replace(/\.csv$/, '');
          return P.row(h('span', { style: 'cursor: pointer', onclick: () => pick(id) }, f.name),
            h('span', { class: 'btns' },
              h('span', { class: 'muted small' }, P.fmtBytes(f.size) + ' · ' + P.fmtDate(f.mtime)),
              h('a', { class: 'btn', href: P.fsUrl(DIR + '/' + f.name, true) }, tr('Bajar')),
              h('button', { class: 'btn red', onclick: () => del(f.name) }, tr('Borrar'))));
        }),
        list.length ? null : h('div', { class: 'pad muted' }, tr('Todavía no hay archivos.')));
    }

    async function del(name) {
      if (!confirm(tr('¿Borrar %s con todas sus lecturas? La lectura sigue y empieza un archivo nuevo.', name))) return;
      try { await P.fsDelete(DIR + '/' + name); P.toast(tr('Borrado')); if (sel + '.csv' === name) { sel = null; data = null; } showFiles(); showChart(); }
      catch (e) { P.toast(e.message, true); }
    }

    /* ---- the chart ---- */
    async function load(id) {
      const path = DIR + '/' + id + '.csv';
      const list = await P.fsList(DIR);
      const f = list.find(x => x.name === id + '.csv');
      if (!f) return { pts: [], cut: false };
      let text, cut = false;
      if (f.size > TAIL) {
        const r = await fetch(P.fsUrl(path), { headers: { Range: `bytes=${f.size - TAIL}-` } });
        text = await r.text();
        cut = true;
      } else {
        text = await P.fsText(path) || '';
      }
      return { pts: parseCsv(text), cut };
    }

    function pick(id) {
      sel = id;
      showTable();
      chartCard.textContent = tr('Cargando…');
      load(id).then(d => {
        if (sel !== id) return;
        data = d;
        /* the shortest range that has all of it */
        const span = d.pts.length ? d.pts[d.pts.length - 1].t - d.pts[0].t : 0;
        range = RANGES.findIndex(r => r[1] >= span);
        if (range < 0) range = RANGES.length - 1;
        showChart();
      }).catch(e => P.toast(e.message, true));
    }

    function showChart() {
      if (!sel || !data) { put(chartCard, h('div', { class: 'muted' }, tr('Elegí una lectura para ver su gráfico.'))); return; }
      const line = state && state.lines.find(l => l.id === sel);
      const pts = data.pts;
      const last = pts.length ? pts[pts.length - 1].t : 0;
      const span = RANGES[range][1];
      const t1 = last, t0 = span ? t1 - span : (pts.length ? pts[0].t : t1 - 60);
      const inr = pts.filter(p => p.t >= t0 && p.t <= t1);
      const vals = inr.filter(p => !isNaN(p.v)).map(p => p.v);
      const unit = line && line.unit ? ' ' + line.unit : '';
      const d = line ? Math.min(line.dec, 3) : 2;
      const cv = h('canvas');
      const tip = h('div', { class: 'dltip' });
      const notes = inr.filter(p => p.note && !/^(alta|baja|high|low|hoch|niedrig)$/i.test(p.note)).slice(-6).reverse();
      put(chartCard,
        h('div', { class: 'dlhead' }, h('b', { class: 'grow' }, line ? line.name : sel + '.csv'),
          h('div', { class: 'dlranges' }, ...RANGES.map((r, i) => h('button', { class: 'btn' + (i === range ? ' on' : ''), onclick: () => { range = i; showChart(); } }, tr(r[0]))))),
        h('div', { class: 'dlchart' }, cv, tip),
        h('div', { class: 'dlstats' },
          vals.length ? [
            h('span', {}, tr('mín'), ' ', h('b', {}, num(Math.min(...vals), d) + unit)),
            h('span', {}, tr('máx'), ' ', h('b', {}, num(Math.max(...vals), d) + unit)),
            h('span', {}, tr('promedio'), ' ', h('b', {}, num(vals.reduce((a, b) => a + b, 0) / vals.length, d) + unit)),
            h('span', {}, h('b', {}, String(vals.length)), ' ', tr('lecturas')),
          ] : null,
          data.cut ? h('span', { class: 'muted small' }, tr('Archivo grande: se leyeron los últimos 8 MB.')) : null),
        notes.length ? h('div', { class: 'small', style: 'margin-top: 10px' }, h('div', { class: 'muted' }, tr('Notas')),
          ...notes.map(n => h('div', {}, fmtT(n.t, true), ' · ', n.note))) : null);
      requestAnimationFrame(() => {
        const m = draw(cv, tip, inr, t0, t1, line);
        if (!m) return;
        const move = e => {
          const r = cv.getBoundingClientRect(), x = e.clientX - r.left;
          let best = null, bd = 1e9;
          for (const p of m.vals) { const dd = Math.abs(m.X(p.t) - x); if (dd < bd) { bd = dd; best = p; } }
          if (!best) return;
          tip.style.display = 'block';
          tip.textContent = fmtT(best.t, true) + ' · ' + num(best.v, d) + unit;
          tip.style.left = Math.min(Math.max(0, m.X(best.t) - 60), r.width - 160) + 'px';
          tip.style.top = Math.max(0, m.Y(best.v) - 34) + 'px';
        };
        cv.onpointermove = move;
        cv.onpointerdown = move;
        cv.onpointerleave = () => { tip.style.display = 'none'; };
      });
    }

    const onResize = () => showChart();
    window.addEventListener('resize', onResize);
    /* the open chart follows new readings, every 15 s */
    const reload = setInterval(() => { if (sel && !missing) load(sel).then(d => { data = d; showChart(); }).catch(() => {}); }, 15000);
    tick();
    showFiles().catch(e => P.toast(e.message, true));
    showChart();
    return () => { gone = true; clearTimeout(timer); clearInterval(reload); window.removeEventListener('resize', onResize); };
  },
});
