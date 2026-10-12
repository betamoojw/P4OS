/* The CAS app's page in the board's portal (docs/PORTAL-PAGES.md): the
 * calculator from the computer's keyboard, the grapher's functions, the
 * statistics lists in and out as CSV, and what the board shows saved as a
 * PNG.
 *
 * While the app is open they talk on the live channel (docs/PORTAL-PAGES.md
 * "Live data", cas_app.c "the portal's page"): the page sends lines
 * ("calc <expr>", "graph <n> <expr>", "stats", "view <n>") and reads
 * "state" (the view, the engine, where the plot on screen is), "hist" and
 * "graph". With the app closed it reads the same files the app keeps in
 * /data. The texts are in Spanish, like the rest of the portal. */
const P = window.P4OS;
const APP = 'aos.cas';
const LIVE = 'live?app=' + APP;
const V_GRAPH = 1;

const css = `
.cs-hist{max-height:52vh;overflow-y:auto;display:flex;flex-direction:column;gap:6px;margin-bottom:10px}
.cs-e{border-bottom:1px solid var(--line);padding:6px 2px}
.cs-in{color:var(--dim);cursor:pointer;font-family:ui-monospace,Menlo,monospace;font-size:.92em;word-break:break-all}
.cs-out{text-align:right;font-family:ui-monospace,Menlo,monospace;font-size:1.15em;word-break:break-all;cursor:pointer}
.cs-out.err{color:var(--red)}
.cs-out.wait{color:var(--dim)}
.cs-ms{color:var(--dim);font-size:.75em;margin-left:8px}
.cs-line{display:flex;gap:8px}
.cs-line input{flex:1;font-family:ui-monospace,Menlo,monospace;font-size:1.1em}
.cs-fn{display:flex;align-items:center;gap:8px;margin:4px 0}
.cs-fn label{flex:none;white-space:nowrap;font-family:ui-monospace,Menlo,monospace}
.cs-fn input{flex:1;font-family:ui-monospace,Menlo,monospace}
.cs-fn .cs-err{color:var(--red);font-size:.85em}
.cs-tab{border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:.9em;margin:8px 0}
.cs-tab td,.cs-tab th{padding:2px 10px;border-bottom:1px solid var(--line);text-align:right}
.cs-tab th{color:var(--dim);font-weight:500}
.cs-shot{max-width:100%;border-radius:10px;margin-top:8px}
`;

/* --- the lists: CSV in and out ----------------------------------------------------- */

/* columns of numbers from a CSV or a pasted table: the separator is guessed
 * (tab, semicolon or comma), and with tab or semicolon a comma is a decimal
 * point; a first row with words is the header */
function parseTable(text) {
  const lines = text.split(/\r?\n/).map(l => l.trim()).filter(l => l.length);
  if (!lines.length) return { cols: [], names: [] };
  const first = lines[0];
  const count = c => first.split(c).length - 1;
  const sep = count('\t') ? '\t' : count(';') ? ';' : count(',') ? ',' : /\s+/;
  const decComma = sep === '\t' || sep === ';' || (sep instanceof RegExp);
  const num = s => {
    s = s.trim().replace(/^"|"$/g, '');
    if (decComma && s.includes(',')) s = s.replace(/\./g, '').replace(',', '.');   /* 1.234,5 */
    if (!/^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/.test(s)) return NaN;
    return parseFloat(s);
  };
  const rows = lines.map(l => l.split(sep));
  let names = [];
  if (rows[0].some(c => c.trim() !== '' && isNaN(num(c)))) names = rows.shift().map(c => c.trim().replace(/^"|"$/g, ''));
  const width = Math.max(0, ...rows.map(r => r.length));
  const cols = [];
  for (let c = 0; c < width; c++) {
    const v = rows.map(r => (c < r.length ? num(r[c]) : NaN)).filter(x => !isNaN(x));
    if (v.length) cols.push({ name: names[c] || 'columna ' + (c + 1), v });
  }
  return { cols };
}

function listsText(cols) {
  let s = '';
  for (let i = 0; i < 4; i++) s += 'L' + (i + 1) + (cols[i] ? ' ' + cols[i].v.slice(0, 500).join(' ') : '') + '\n';
  return s;
}

function readLists(text) {
  const L = [[], [], [], []];
  for (const line of (text || '').split('\n')) {
    const m = /^L([1-4])(.*)$/.exec(line.trim());
    if (m) L[m[1] - 1] = m[2].trim().split(/\s+/).filter(x => x).map(Number);
  }
  return L;
}

/* --- the screen as a PNG -------------------------------------------------------------- */

/* the portal's screenshot: BMP, RGB565, rows top-down (aos_portal.c) */
async function screenCanvas(crop) {
  const r = await fetch('/api/screen.bmp', { cache: 'no-store' });
  if (!r.ok) throw new Error('No se pudo leer la pantalla (' + r.status + ')');
  const b = await r.arrayBuffer(), dv = new DataView(b);
  const off = dv.getUint32(10, true), w = dv.getInt32(18, true), hRaw = dv.getInt32(22, true);
  const h = Math.abs(hRaw), stride = ((w * 2 + 3) & ~3), topDown = hRaw < 0;
  const x0 = crop ? Math.max(0, crop.x) : 0, y0 = crop ? Math.max(0, crop.y) : 0;
  const cw = crop ? Math.min(crop.w, w - x0) : w, ch = crop ? Math.min(crop.h, h - y0) : h;
  const cv = document.createElement('canvas');
  cv.width = cw;
  cv.height = ch;
  const ctx = cv.getContext('2d'), img = ctx.createImageData(cw, ch), d = img.data;
  for (let y = 0; y < ch; y++) {
    const sy = topDown ? y0 + y : h - 1 - (y0 + y);
    for (let x = 0; x < cw; x++) {
      const p = dv.getUint16(off + sy * stride + (x0 + x) * 2, true), o = (y * cw + x) * 4;
      d[o] = ((p >> 11) & 31) * 255 / 31;
      d[o + 1] = ((p >> 5) & 63) * 255 / 63;
      d[o + 2] = (p & 31) * 255 / 31;
      d[o + 3] = 255;
    }
  }
  ctx.putImageData(img, 0, 0);
  return cv;
}

/* --- the page ------------------------------------------------------------------------ */

P.registerPage({
  id: 'cas',
  name: 'CAS',
  icon: '∑',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-cas')) document.head.append(h('style', { id: 'css-cas' }, css));
    let alive = true, open = false, state = null, histSeq = '', graphSeq = '', table = null, recall = [], ri = -1;

    const pill = h('span', { class: 'pill' }, '…');
    const openBtn = h('button', { class: 'btn', onclick: () => P.openApp(APP).then(() => P.toast('CAS abierta en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir CAS en la placa');
    const send = text => P.api(LIVE, { method: 'POST', body: text });

    /* the console */
    const hist = h('div', { class: 'cs-hist' });
    const input = h('input', { type: 'text', placeholder: 'factor(x^4-1)', autocomplete: 'off', spellcheck: 'false' });
    const calc = async () => {
      const t = input.value.trim();
      if (!t) return;
      try {
        if (!open) {
          await P.openApp(APP);
          await new Promise(r => setTimeout(r, 1500));
        }
        await send('calc ' + t);
        recall.push(t);
        ri = -1;
        input.value = '';
      } catch (e) { P.toast(e.message, true); }
    };
    input.addEventListener('keydown', e => {
      if (e.key === 'Enter') calc();
      else if (e.key === 'ArrowUp' && recall.length) {
        ri = ri < 0 ? recall.length - 1 : Math.max(0, ri - 1);
        input.value = recall[ri];
        e.preventDefault();
      } else if (e.key === 'ArrowDown' && ri >= 0) {
        ri = Math.min(recall.length, ri + 1);
        input.value = ri < recall.length ? recall[ri] : '';
        e.preventDefault();
      }
    });
    const showHist = list => {
      put(hist, ...(list.length ? list.map(e => h('div', { class: 'cs-e' },
        h('div', { class: 'cs-in', title: 'Copiar abajo', onclick: () => { input.value = e.in; input.focus(); } }, e.in),
        h('div', { class: 'cs-out' + (e.out == null ? ' wait' : e.err ? ' err' : ''), title: 'Sumar al final de lo que escribís',
          onclick: () => { if (e.out) { input.value += e.out; input.focus(); } } },
          e.out == null ? 'calculando…' : e.out, e.ms ? h('span', { class: 'cs-ms' }, e.ms + ' ms') : null)))
        : [h('p', { class: 'note' }, 'Todavía no hay cuentas.')]));
      hist.scrollTop = hist.scrollHeight;
    };

    /* the functions */
    /* what was typed here, against what the app keeps (its own spelling:
     * x^2/4 comes back as ((x^(2))/(4))): the field keeps the typed one
     * while the app's does not change */
    const fin = [], ferr = [], typed = [], canon = [];
    const fnRows = [0, 1, 2, 3, 4, 5].map(i => {
      fin[i] = h('input', { type: 'text', autocomplete: 'off', spellcheck: 'false', placeholder: i ? '' : 'sin(x)' });
      ferr[i] = h('span', { class: 'cs-err' });
      fin[i].dataset.sent = '';
      fin[i].addEventListener('keydown', e => { if (e.key === 'Enter') applyFns(); });
      return h('div', { class: 'cs-fn' }, h('label', {}, 'f' + (i + 1) + '(x) ='), fin[i], ferr[i]);
    });
    const showFns = list => list.forEach((f, i) => {
      if (typed[i] != null && canon[i] == null) canon[i] = f.f;        /* the answer to what was sent */
      else if (canon[i] != null && canon[i] !== f.f) typed[i] = canon[i] = null;   /* changed on the board */
      const shown = typed[i] != null ? typed[i] : f.f;
      if (document.activeElement !== fin[i]) fin[i].value = shown;
      fin[i].dataset.sent = shown;
      ferr[i].textContent = f.err || '';
    });
    const applyFns = async () => {
      if (!open) { P.toast('Abrí CAS en la placa primero', true); return; }
      try {
        for (let i = 0; i < 6; i++) {
          const v = fin[i].value.trim();
          if (v === fin[i].dataset.sent) continue;
          typed[i] = v;
          canon[i] = null;
          await send('graph ' + (i + 1) + ' ' + v);
        }
        P.toast('Funciones enviadas');
      } catch (e) { P.toast(e.message, true); }
    };

    /* the lists */
    const csvText = h('textarea', { rows: 5, style: 'width:100%', placeholder: 'Pegá una tabla (de una planilla, separada por comas, punto y coma o tabulaciones) o elegí un archivo .csv' });
    const csvFile = h('input', { type: 'file', accept: '.csv,.txt,.tsv,text/csv,text/plain' });
    const preview = h('div');
    const showPreview = () => {
      table = parseTable(csvText.value);
      if (!table.cols.length) { put(preview, csvText.value.trim() ? h('p', { class: 'note' }, 'No encontré columnas de números.') : null); return; }
      const cols = table.cols.slice(0, 4), n = Math.max(...cols.map(c => c.v.length));
      const rows = [];
      for (let r = 0; r < Math.min(n, 6); r++) rows.push(h('tr', {}, ...cols.map(c => h('td', {}, r < c.v.length ? String(c.v[r]) : ''))));
      put(preview,
        h('table', { class: 'cs-tab' },
          h('tr', {}, ...cols.map((c, i) => h('th', {}, 'L' + (i + 1) + ' · ' + c.name + ' (' + c.v.length + ')'))),
          ...rows,
          n > 6 ? h('tr', {}, ...cols.map(() => h('td', {}, '…'))) : null),
        table.cols.length > 4 ? h('p', { class: 'note' }, 'Hay ' + table.cols.length + ' columnas: van las cuatro primeras.') : null,
        cols.some(c => c.v.length > 500) ? h('p', { class: 'note' }, 'Cada lista toma hasta 500 valores.') : null);
    };
    csvText.addEventListener('input', showPreview);
    csvFile.addEventListener('change', () => {
      const f = csvFile.files[0];
      if (!f) return;
      f.text().then(t => { csvText.value = t; showPreview(); });
    });
    const loadLists = async () => {
      if (!table || !table.cols.length) { P.toast('No hay columnas de números para cargar', true); return; }
      try {
        await P.fsMkdir('/data').catch(() => {});
        await P.fsPut('/data/cas_stats.txt', listsText(table.cols));
        if (open) await send('stats');
        P.toast(open ? 'Listas cargadas en la placa' : 'Listas guardadas: las ve CAS cuando abra Estadística');
      } catch (e) { P.toast(e.message, true); }
    };
    const downloadLists = async () => {
      try {
        const L = readLists(await P.fsText('/data/cas_stats.txt'));
        const n = Math.max(0, ...L.map(l => l.length));
        if (!n) { P.toast('Las listas están vacías', true); return; }
        let s = 'L1,L2,L3,L4\n';
        for (let r = 0; r < n; r++) s += L.map(l => (r < l.length ? l[r] : '')).join(',') + '\n';
        P.saveBlob(new Blob([s], { type: 'text/csv' }), 'listas.csv');
      } catch (e) { P.toast(e.message, true); }
    };

    /* the picture */
    const shotImg = h('div');
    const plotBtn = h('button', { class: 'btn pri', onclick: () => shot(true) }, 'Guardar el gráfico como PNG');
    const shot = async onlyPlot => {
      try {
        const crop = onlyPlot && state && state.plot ? state.plot : null;
        if (onlyPlot && !crop) { P.toast('La placa no muestra ningún gráfico ahora', true); return; }
        /* the buttons that float over the plot hidden for the picture */
        let cleaned = false;
        if (crop && open) {
          await send('clean 1');
          for (let i = 0; i < 15 && !cleaned; i++) {
            await new Promise(r => setTimeout(r, 100));
            try { cleaned = (await P.api(LIVE + '&key=state')).clean; } catch { break; }
          }
          await new Promise(r => setTimeout(r, 150));      /* a frame drawn without them */
        }
        let cv;
        try { cv = await screenCanvas(crop); } finally { if (cleaned) send('clean 0').catch(() => {}); }
        cv.toBlob(b => {
          const stamp = new Date().toISOString().slice(0, 19).replace(/[-:]/g, '').replace('T', '-');
          P.saveBlob(b, (onlyPlot ? 'cas-grafico-' : 'cas-pantalla-') + stamp + '.png');
          put(shotImg, h('img', { class: 'cs-shot', src: URL.createObjectURL(b) }));
        }, 'image/png');
      } catch (e) { P.toast(e.message, true); }
    };

    put(main,
      h('h1', {}, 'CAS'),
      h('div', { class: 'card' },
        h('div', { class: 'row' }, h('div', { class: 'grow' }, 'En la placa'), pill),
        h('div', { class: 'btns' }, openBtn)),
      h('div', { class: 'card' }, h('h2', {}, 'Calculadora'),
        hist,
        h('div', { class: 'cs-line' }, input, h('button', { class: 'btn pri', onclick: calc }, 'Calcular')),
        h('p', { class: 'note' }, 'Sintaxis de Xcas: factor(x^4-1), int(sin(x)^2,x), solve(x^2=2,x), diff(f1(x),x), limit(sin(x)/x,x,0). ',
          'La cuenta se hace en la placa y queda en su historial. ↑ y ↓ traen lo que escribiste antes; ',
          'tocar una cuenta la copia abajo, tocar un resultado lo suma.')),
      h('div', { class: 'card' }, h('h2', {}, 'Funciones del gráfico'),
        ...fnRows,
        h('div', { class: 'btns' },
          h('button', { class: 'btn pri', onclick: applyFns }, 'Enviar'),
          h('button', { class: 'btn', onclick: () => send('view ' + V_GRAPH).catch(e => P.toast(e.message, true)) }, 'Mostrar el gráfico en la placa'))),
      h('div', { class: 'card' }, h('h2', {}, 'Listas de Estadística'),
        csvFile, csvText, preview,
        h('div', { class: 'btns' },
          h('button', { class: 'btn pri', onclick: loadLists }, 'Cargar en L1…L4'),
          h('button', { class: 'btn', onclick: downloadLists }, 'Bajar las listas (CSV)')),
        h('p', { class: 'note' }, 'Cargar reemplaza las cuatro listas de la placa.')),
      h('div', { class: 'card' }, h('h2', {}, 'Imagen'),
        h('p', { class: 'note' }, 'Lo que la placa muestra ahora: el gráfico solo (de Gráfico, Geometría, Estadística, Finanzas o Ciencias) o la pantalla entera.'),
        h('div', { class: 'btns' }, plotBtn,
          h('button', { class: 'btn', onclick: () => shot(false) }, 'Guardar la pantalla')),
        shotImg));

    /* with the app closed: its files */
    const fromFiles = () => {
      P.fsText('/data/cas_hist.txt').then(t => {
        if (open || !alive) return;
        const list = (t || '').split('\n').filter(l => l).map(l => {
          const [i, o, e] = l.split('\t');
          return { in: i, out: o || '', err: e === '1', ms: 0 };
        });
        showHist(list);
      }).catch(() => {});
      P.fsText('/data/cas_graph.txt').then(t => {
        if (open || !alive) return;
        const list = [0, 1, 2, 3, 4, 5].map(() => ({ f: '', on: true, err: null }));
        for (const l of (t || '').split('\n')) {
          const m = /^F (\d) (\d) (.*)$/.exec(l);
          if (m && +m[1] < 6) list[+m[1]] = { f: m[3], on: m[2] === '1', err: null };
        }
        showFns(list);
      }).catch(() => {});
    };

    const tick = async () => {
      try {
        state = await P.api(LIVE + '&key=state');
        if (!open) { open = true; histSeq = graphSeq = ''; }
      } catch {
        if (open || pill.textContent === '…') { open = false; state = null; fromFiles(); }
      }
      pill.textContent = open ? (state && !state.ready ? 'Abierta, arrancando el motor' : state && state.busy ? 'Abierta, calculando' : 'Abierta') : 'Cerrada';
      pill.style.color = open ? 'var(--green)' : 'var(--dim)';
      openBtn.style.display = open ? 'none' : '';
      plotBtn.disabled = !(state && state.plot);
      if (!open) return;
      /* hist and graph only when they changed: the seq header says so */
      for (const [key, seqRef, show] of [['hist', () => histSeq, v => { histSeq = v; }], ['graph', () => graphSeq, v => { graphSeq = v; }]]) {
        try {
          const r = await fetch('/api/' + LIVE + '&key=' + key, { cache: 'no-store' });
          if (!r.ok) continue;
          const seq = r.headers.get('X-Live-Seq') || '';
          if (seq && seq === seqRef()) continue;
          show(seq);
          const data = await r.json();
          if (key === 'hist') showHist(data); else showFns(data);
        } catch { /* the next tick */ }
      }
    };
    tick();
    const t = setInterval(() => { if (alive) tick(); }, 700);
    return () => { alive = false; clearInterval(t); };
  },
});
