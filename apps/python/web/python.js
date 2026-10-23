/* Python's page in the board's portal (docs/PORTAL-PAGES.md): the console
 * on the board from the browser, the card's programs, apps and libraries
 * with an editor, and mip. tools/install_apps.sh puts this file in the
 * card's /web; window.P4OS (version 1) is all it uses.
 *
 * It talks to the console app (aos.python) on the live channel: it posts
 * "line <text>", "run <path>", "stop" and "clear", and reads the console's
 * text (key "con") and what it is doing (key "st": idle, busy, input). An
 * app (py.<name>) puts what it printed under its own id, its error between
 * the bytes 0x01 and 0x02. Everything else is files on the card. Texts in
 * Spanish through tr() (web.python.lang); the code samples stay as they are. */
const P = window.P4OS.forPage ? window.P4OS.forPage('python') : window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const { main, h, put, api, toast, fmtBytes, fmtDate, fsPut, fsList, fsDelete, fsMkdir, fsUrl, fsText, openApp } = P;

if (!document.getElementById('css-python')) {
  const st = document.createElement('style');
  st.id = 'css-python';
  st.textContent = `
.pycols { display: grid; grid-template-columns: minmax(0, 1fr) minmax(300px, 400px); gap: 18px; align-items: start; }
.pyta { width: 100%; min-height: 440px; background: var(--card); border: 1px solid var(--line); border-radius: 12px; padding: 12px 14px; outline: none;
  font: 14px/1.5 ui-monospace, "JetBrains Mono", Menlo, monospace; resize: vertical; tab-size: 4; }
.pyta:focus { border-color: var(--accent); }
.pycon { background: #0f1216; color: #e8eaed; border: 1px solid var(--line); border-radius: 12px; padding: 10px 14px; height: 300px; overflow: auto;
  white-space: pre-wrap; word-break: break-word; font: 13px/1.45 ui-monospace, "JetBrains Mono", Menlo, monospace; margin: 0; }
.pycon.short { height: 180px; }
.pycon .err { color: #ff6b6b; }
.pycon .dim { color: #8e8e93; }
.pyin { display: flex; gap: 8px; align-items: center; margin-top: 8px; }
.pyin span { font: 13px ui-monospace, Menlo, monospace; color: var(--dim); }
.pyin input { flex: 1; font: 14px ui-monospace, "JetBrains Mono", Menlo, monospace; }
.pyst { font-size: 12px; padding: 2px 9px; border-radius: 99px; background: var(--card2); color: var(--dim); }
.pyst.busy { background: #3a2f10; color: #ffd43b; } .pyst.input { background: #10283a; color: #64d2ff; } .pyst.idle { background: #10301a; color: var(--green); }
.pyhelp { font-size: 13px; color: var(--dim); position: sticky; top: 16px; }
.pyhelp h2 { margin: 14px 0 6px; } .pyhelp h2:first-child { margin-top: 0; }
.pyhelp dl { margin: 0; display: grid; grid-template-columns: minmax(0, 1.1fr) minmax(0, 1fr); gap: 3px 10px; }
.pyhelp dt { color: var(--accent); font-family: ui-monospace, Menlo, monospace; font-size: 12.5px; overflow-wrap: anywhere; }
.pyhelp dd { margin: 0; }
.pyhead { display: flex; align-items: baseline; gap: 10px; margin: 18px 0 8px; } .pyhead h2 { margin: 0; }
@media (max-width: 1000px) { .pycols { grid-template-columns: 1fr; } .pyhelp { position: static; } }
`;
  document.head.append(st);
}

const CONSOLE = 'aos.python';
const LIVE = id => 'live?app=' + encodeURIComponent(id);
const KINDS = [
  { dir: '/python', name: N_('Programas'), note: N_('Corren en la consola de la placa.'), ext: /\.py$/i },
  { dir: '/python/apps', name: N_('Apps'), note: N_('Cada .py es una app del inicio (desde el próximo arranque) que dibuja con aos.'), ext: /\.(py|aic)$/i },
  { dir: '/python/lib', name: N_('Bibliotecas'), note: N_('Lo que import encuentra; mip instala acá.'), ext: /\.(py|mpy)$/i },
];
const PROGRAM = `# Un programa para la consola de Python: «Correr» lo ejecuta ahí.
import time

for i in range(5):
    print("Hola", i)
    time.sleep(0.5)
`;
const APP = `# @name Mi app
# Una app del inicio: dibuja con aos. Las funciones son opcionales.
import aos

x, y = aos.W // 2, aos.H // 2


def touch(tx, ty, ev):
    global x, y
    x, y = tx, ty
    if ev == "down":
        aos.beep(880, 30)


def draw():
    aos.clear(0x101820)
    aos.disc(x, y, 20, 0xFFD43B)
    aos.text(8, 8, "Hola", 0xF4EFE6, 2)
`;
const HELP_APP = [
  [N_('Dos maneras'), [['def tick(dt), draw()', N_('la app las llama en cada cuadro; también init(), touch(x, y, ev), gesture(ev, x, y, …), resize(w, h)')],
    ['while True: … aos.frame()', N_('un bucle propio: frame() muestra lo dibujado y espera el cuadro siguiente')]]],
  [N_('El módulo aos'), [['aos.W, aos.H', N_('el lienzo: 240 × 426 parado, a ×3')], ['clear(c)', N_('colores 0xRRGGBB')], ['pixel(x, y, c)', ''],
    ['rect / box(x, y, w, h, c)', N_('relleno / contorno')], ['round(x, y, w, h, c, r)', N_('esquinas mordidas')], ['line(x0, y0, x1, y1, c)', ''],
    ['disc / ring(cx, cy, r, c)', N_('relleno / contorno')], ['ellipse(cx, cy, rx, ry, c)', ''], ['shade(x, y, w, h, f)', N_('oscurece (f<0) o aclara')],
    ['text(x, y, s, c, esc)', N_('8×8, ASCII; devuelve el ancho')], ['blit(x, y, w, h, datos, clave)', N_('píxeles RGB565')],
    ['background()', N_('lo dibujado queda de fondo')], ['events()', N_('lo que llegó con el cuadro')], ['touch()', N_('x, y, apoyado')],
    ['fingers()', '[(id, x, y), …]'], ['pad()', 'held, pressed, x, y, on; aos.PAD_A…'], ['ms(), beep(hz, ms), stats()', ''],
    ['aos.fb', N_('el framebuf de MicroPython sobre el lienzo; aos.rgb565(c)')]]],
  [N_('En las primeras líneas'), [['# @name Nombre', N_('el del inicio')], ['# @canvas WxH', N_('otro lienzo, a la escala entera que entre')],
    ['# @orientation portrait', N_('o landscape: gira la pantalla')]]]];
const HELP_PROGRAM = [
  [N_('La consola'), [['print(), input()', N_('salen y entran por la consola de la placa')], ['open(), os, import', N_('la tarjeta es /, se arranca en /python')],
    ['requests, socket', N_('la red de la placa, https con sus certificados')], ['import mip', N_("mip.install('nombre') baja de micropython-lib a /python/lib")],
    ['time, json, re, math, random', N_("y help('modules') para el resto")], [N_('Detener'), N_('KeyboardInterrupt en lo que corre')]]]];

/* ---- the console on the board ---- */

/* The terminal of the console app, with a line to type into. Polls one
 * request at a time: the board's portal goes deaf if hammered. */
function consoleBox() {
  const out = h('pre', { class: 'pycon' });
  const st = h('span', { class: 'pyst' }, '…');
  const inp = h('input', { type: 'text', placeholder: tr('una línea de Python, y Enter'), spellcheck: 'false', autocapitalize: 'off', autocorrect: 'off' });
  let state = '', text = null, misses = 0, alive = true, hist = [], hpos = 0, queue = [], sending = false;

  const say = s => {
    state = s;
    st.className = 'pyst ' + s;
    st.textContent = { idle: tr('lista'), busy: tr('corriendo…'), input: tr('esperando input()'), off: tr('sin intérprete'), closed: tr('cerrada') }[s] || s;
  };
  async function poll() {
    if (!alive) return;
    /* off the page while the editor shows an app's output instead */
    if (!document.hidden && out.isConnected) {
      try {
        const s = String(await api(LIVE(CONSOLE) + '&key=st')).trim();
        const t = String(await api(LIVE(CONSOLE) + '&key=con'));
        misses = 0;
        say(s);
        if (t !== text) {
          text = t;
          const end = out.scrollTop + out.clientHeight >= out.scrollHeight - 8;
          out.textContent = t;
          if (end) out.scrollTop = out.scrollHeight;
        }
      } catch {
        /* the first asks make the app publish: closed only if it stays so */
        if (++misses >= 3) { say('closed'); if (text !== '') { text = ''; put(out, h('span', { class: 'dim' }, tr('La consola no está abierta en la placa. «Abrir en la placa», o escribí una línea: se abre sola.'))); } }
      }
    }
    setTimeout(poll, 700);
  }
  async function ensureOpen() {
    if (state !== 'closed') return;
    await openApp(CONSOLE);
    say('off');
    await new Promise(r => setTimeout(r, 1800));
  }
  async function send(msg) {
    await ensureOpen();
    await api(LIVE(CONSOLE), { method: 'POST', body: msg });
  }
  /* lines one after the other, waiting for the console to take each */
  async function pump() {
    if (sending) return;
    sending = true;
    try {
      while (queue.length) {
        if (state === 'busy') { await new Promise(r => setTimeout(r, 300)); continue; }
        await send('line ' + queue.shift());
        await new Promise(r => setTimeout(r, 250));
      }
    } catch (e) { toast(e.message, true); queue = []; }
    sending = false;
  }
  const typeLines = lines => { queue.push(...lines); pump(); };

  inp.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
      e.preventDefault();
      const v = inp.value;
      if (state === 'busy') { toast(tr('Está corriendo: Detener, o esperá a que termine'), true); return; }
      if (v.trim()) { hist.push(v); hpos = hist.length; }
      inp.value = '';
      typeLines([v]);
    } else if (e.key === 'ArrowUp' && hpos > 0) { e.preventDefault(); inp.value = hist[--hpos]; }
    else if (e.key === 'ArrowDown' && hpos < hist.length) { e.preventDefault(); hpos++; inp.value = hist[hpos] || ''; }
  });
  /* several lines pasted: typed one by one, and an empty one to close a block */
  inp.addEventListener('paste', e => {
    const t = (e.clipboardData || window.clipboardData).getData('text');
    if (!t.includes('\n')) return;
    e.preventDefault();
    const lines = t.replace(/\r/g, '').replace(/\n+$/, '').split('\n');
    typeLines([...lines, '']);
  });

  const box = h('div', {},
    h('div', { class: 'btns', style: 'margin-bottom:8px;align-items:center' }, st,
      h('div', { style: 'flex:1' }),
      h('button', { class: 'btn', onclick: () => send('stop').catch(e => toast(e.message, true)) }, tr('Detener')),
      h('button', { class: 'btn', onclick: () => send('clear').catch(e => toast(e.message, true)) }, tr('Limpiar')),
      h('button', { class: 'btn', onclick: () => openApp(CONSOLE).then(() => toast(tr('Abriendo la consola en la placa')), e => toast(e.message, true)) }, tr('Abrir en la placa'))),
    out, h('div', { class: 'pyin' }, h('span', {}, '>>>'), inp));
  setTimeout(poll, 0);
  return {
    el: box,
    run: async path => {
      if (state === 'busy') { toast(tr('Hay algo corriendo en la consola: Detener primero'), true); return; }
      try { await send('run ' + path); toast(tr('Corriendo %s en la consola', path.split('/').pop())); } catch (e) { toast(e.message, true); }
    },
    typeLines,
    busy: () => state === 'busy',
    stop: () => { alive = false; },
  };
}

/* What an app printed, and its error in red (between 0x01 and 0x02). */
function appOutput(id) {
  const out = h('pre', { class: 'pycon short' });
  let alive = true, last = null, misses = 0;
  async function poll() {
    if (!alive) return;
    if (!document.hidden && out.isConnected) {
      try {
        const t = String(await api(LIVE(id) + '&key=con'));
        misses = 0;
        if (t !== last) {
          last = t;
          const parts = [];
          let rest = t;
          while (rest) {
            const a = rest.indexOf('\x01');
            if (a < 0) { parts.push(rest); break; }
            parts.push(rest.slice(0, a));
            const b = rest.indexOf('\x02', a);
            parts.push(h('span', { class: 'err' }, rest.slice(a + 1, b < 0 ? undefined : b)));
            rest = b < 0 ? '' : rest.slice(b + 1);
          }
          put(out, ...(parts.length && t.trim() ? parts : [h('span', { class: 'dim' }, tr('La app está abierta y no imprimió nada.'))]));
          out.scrollTop = out.scrollHeight;
        }
      } catch {
        if (++misses >= 3 && last !== '') { last = ''; put(out, h('span', { class: 'dim' }, tr('La app no está abierta en la placa. Lo que imprima con print() y sus errores salen acá.'))); }
      }
    }
    setTimeout(poll, 1000);
  }
  setTimeout(poll, 0);
  return { el: out, stop: () => { alive = false; } };
}

/* ---- the files ---- */

const kindOf = path => KINDS.slice().reverse().find(k => path.startsWith(k.dir + '/')) || KINDS[0];
const appId = name => 'py.' + name.replace(/\.py$/i, '');

async function deleteTree(path) {
  for (const f of await fsList(path)) {
    if (f.dir) await deleteTree(path + '/' + f.name);
    else await fsDelete(path + '/' + f.name);
  }
  await fsDelete(path);
}

function pagePython() {
  let view = 'list', path = '', saved = null, appIds = new Set(), outBox = null;
  const con = consoleBox();
  const lists = h('div', {});
  const status = h('span', { class: 'muted small' });
  const nameIn = h('input', { type: 'text', maxlength: 40, class: 'mono', placeholder: tr('nombre.py') });
  const ta = h('textarea', { class: 'pyta', spellcheck: 'false', autocapitalize: 'off', autocorrect: 'off' });
  const where = h('span', { class: 'muted small' });
  const help = h('div', { class: 'card pad pyhelp' });
  const below = h('div', { style: 'margin-top:12px' });
  const listSec = h('div', {}), edSec = h('div', { style: 'display:none' });
  const say = (t, cls) => { status.textContent = t; status.className = 'small ' + (cls || 'muted'); };

  async function load() {
    try { appIds = new Set((await api('apps')).map(a => a.id)); } catch { /* the lists still help */ }
    const cards = [];
    for (const k of KINDS) {
      let files = [];
      try { files = (await fsList(k.dir)).filter(f => f.name[0] !== '.' && (f.dir ? k.dir !== '/python' : k.ext.test(f.name))); } catch { }
      files.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
      const rows = files.map(f => {
        const full = k.dir + '/' + f.name;
        const isApp = k.dir === '/python/apps' && /\.py$/i.test(f.name) && f.name[0] !== '_';
        const known = isApp && appIds.has(appId(f.name));
        const info = f.dir ? tr('carpeta') : fmtBytes(f.size) + (f.mtime ? ' · ' + fmtDate(f.mtime) : '') + (isApp && !known ? ' · ' + tr('nueva: en el inicio después de reiniciar') : '');
        const editable = !f.dir && /\.py$/i.test(f.name);
        return h('div', { class: 'row' },
          h('div', { class: 'grow' }, h('div', { class: 'mono ell' }, f.name + (f.dir ? '/' : '')), h('div', { class: 'muted small' }, info)),
          h('div', { class: 'btns' },
            k.dir === '/python' && editable ? h('button', { class: 'btn', onclick: () => con.run(full) }, tr('Correr')) : null,
            isApp ? h('button', { class: 'btn', disabled: !known, onclick: () => runApp(f.name) }, tr('Abrir')) : null,
            f.dir ? null : h('a', { class: 'btn', href: fsUrl(full, true) }, tr('Bajar')),
            editable ? h('button', { class: 'btn pri', onclick: () => edit(full) }, tr('Editar')) :
              h('button', { class: 'btn red', onclick: () => removePath(full, f.dir) }, tr('Borrar'))));
      });
      cards.push(h('div', { class: 'pyhead' }, h('h2', {}, tr(k.name)), h('span', { class: 'muted small mono' }, k.dir), h('span', { class: 'muted small' }, tr(k.note))),
        h('div', { class: 'card' }, rows.length ? rows : h('div', { class: 'row muted' }, tr('Vacía.'))));
    }
    put(lists, ...cards);
  }

  async function removePath(full, dir) {
    if (!confirm(dir ? tr('¿Borrar %s y todo lo que tiene de la placa?', full) : tr('¿Borrar %s de la placa?', full))) return;
    try { if (dir) await deleteTree(full); else await fsDelete(full); toast(tr('Borrado %s', full)); load(); } catch (e) { toast(e.message, true); }
  }

  async function runApp(name) {
    try {
      if (!appIds.size) appIds = new Set((await api('apps')).map(a => a.id));
      if (!appIds.has(appId(name))) { toast(tr('%s es nueva: aparece en el inicio de la placa después de reiniciarla', name), true); return; }
      await openApp(appId(name));
      toast(tr('Abriendo %s en la placa', name));
    } catch (e) { toast(e.message, true); }
  }

  function setHelp(kind) {
    const rows = kind.dir === '/python/apps' ? HELP_APP : HELP_PROGRAM;
    put(help, ...rows.flatMap(([t, r]) => [h('h2', {}, tr(t)), h('dl', {}, r.flatMap(([k, v]) => [h('dt', {}, tr(k)), h('dd', {}, v && tr(v))]))]));
  }
  function openEditor(full, text, isNew) {
    path = full;
    const k = kindOf(full);
    nameIn.value = full.split('/').pop();
    where.textContent = k.dir + '/';
    ta.value = text;
    saved = isNew ? null : text;
    say('');
    setHelp(k);
    if (outBox) outBox.stop();
    outBox = null;
    if (k.dir === '/python/apps') {
      outBox = appOutput(appId(nameIn.value));
      put(below, h('div', { class: 'muted small', style: 'margin-bottom:6px' }, tr('Lo que imprime la app en la placa')), outBox.el);
    } else {
      put(below, con.el);
    }
    show('editor');
  }
  async function edit(full) {
    let text;
    try { text = await fsText(full); } catch (e) { toast(e.message, true); return; }
    if (text == null) { toast(tr('No existe %s', full), true); return; }
    openEditor(full, text, false);
  }
  const fresh = (dir, tmpl, name) => { openEditor(dir + '/' + name, tmpl, true); nameIn.select(); nameIn.focus(); };

  function show(v) {
    view = v;
    listSec.style.display = v === 'list' ? '' : 'none';
    edSec.style.display = v === 'editor' ? '' : 'none';
    if (v === 'list') {
      put(conSlot, con.el);
      if (outBox) { outBox.stop(); outBox = null; }
      load();
    }
  }
  const dirty = () => view === 'editor' && ta.value !== saved;
  function back() {
    if (dirty() && !confirm(tr('Hay cambios sin guardar. ¿Volver igual?'))) return;
    show('list');
  }

  async function save() {
    let name = nameIn.value.trim();
    if (name && !/\.py$/i.test(name)) name += '.py';
    const k = kindOf(path);
    if (!name || !/^[A-Za-z0-9_.-]+\.py$/.test(name) || name[0] === '.') { say(tr('Un nombre con letras sin acento, dígitos, _ - y punto, terminado en .py'), 'bad'); return false; }
    if (k.dir === '/python/apps' && name[0] === '_') say(tr('Empieza con _: es un módulo para las otras apps, no una app'), 'muted');
    const full = k.dir + '/' + name;
    if (saved !== null && full !== path && !confirm(tr('¿Guardarlo como %s? (el viejo se borra)', full))) return false;
    if (full !== path || saved === null) {
      const there = (await fsList(k.dir).catch(() => [])).some(f => f.name === name);
      if (there && !confirm(tr('Ya hay un %s en %s. ¿Reemplazarlo?', name, k.dir))) return false;
    }
    say(tr('Guardando…'));
    try {
      await fsMkdir('/python').catch(() => { });
      await fsMkdir(k.dir).catch(() => { });
      const body = ta.value;
      await fsPut(full, body);
      if (saved !== null && full !== path) await fsDelete(path).catch(() => { });
      path = full; nameIn.value = name; saved = body;
      say(k.dir === '/python/apps' ? tr('Guardado: si la app está abierta, la placa la vuelve a correr') : tr('Guardado en la placa'), 'ok');
      return true;
    } catch (e) { say(tr('No se pudo guardar: %s', e.message), 'bad'); return false; }
  }
  async function saveRun() {
    if (dirty() || saved === null) { if (!await save()) return; }
    if (kindOf(path).dir === '/python/apps') runApp(path.split('/').pop());
    else con.run(path);
  }
  async function del() {
    if (saved === null) { show('list'); return; }
    if (!confirm(tr('¿Borrar %s de la placa?', path))) return;
    try { await fsDelete(path); toast(tr('Borrado %s', path)); saved = ta.value; show('list'); } catch (e) { toast(e.message, true); }
  }

  ta.addEventListener('keydown', e => {
    if ((e.metaKey || e.ctrlKey) && e.key === 's') { e.preventDefault(); save(); }
    else if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') { e.preventDefault(); saveRun(); }
    else if (e.key === 'Tab' && !e.metaKey && !e.ctrlKey) {
      e.preventDefault();
      const s = ta.selectionStart, ln = ta.value.lastIndexOf('\n', s - 1) + 1;
      if (e.shiftKey) {                         /* back four, at the start of the line */
        const n = ta.value.slice(ln, ln + 4).match(/^ */)[0].length;
        if (n) { ta.setRangeText('', ln, ln + n, 'preserve'); ta.selectionStart = ta.selectionEnd = Math.max(ln, s - n); }
      } else ta.setRangeText('    ', s, ta.selectionEnd, 'end');
    } else if (e.key === 'Enter' && !e.shiftKey && !e.metaKey && !e.ctrlKey) {
      /* the same indent, and four more after a colon */
      e.preventDefault();
      const s = ta.selectionStart, ln = ta.value.lastIndexOf('\n', s - 1) + 1;
      const line = ta.value.slice(ln, s);
      let ind = line.match(/^ */)[0];
      if (/:\s*$/.test(line)) ind += '    ';
      ta.setRangeText('\n' + ind, s, ta.selectionEnd, 'end');
    }
  });
  /* a "saved" line goes away on typing; it is the only one with class ok */
  ta.addEventListener('input', () => { if (status.classList.contains('ok')) say(''); });

  /* uploads: .py and .mpy to the folder picked, an .aic to the apps' */
  const upTo = h('select', {}, KINDS.map(k => h('option', { value: k.dir }, tr(k.name) + ' (' + k.dir + ')')));
  function upload(fl) {
    const list = [...fl].filter(f => /\.(py|mpy|aic)$/i.test(f.name));
    if (!list.length) { toast(tr('Sólo archivos .py, .mpy y .aic'), true); return; }
    (async () => {
      for (const f of list) {
        const dir = /\.aic$/i.test(f.name) ? '/python/apps' : upTo.value;
        try { await fsMkdir('/python').catch(() => { }); await fsMkdir(dir).catch(() => { }); await fsPut(dir + '/' + f.name, f); } catch (e) { toast(f.name + ': ' + e.message, true); return; }
      }
      toast(list.length === 1 ? tr('Subido %s', list[0].name) : tr('Subidos %s archivos', list.length));
      load();
    })();
  }
  const fileIn = h('input', { type: 'file', multiple: true, accept: '.py,.mpy,.aic', style: 'display:none', onchange: e => upload(e.target.files) });
  const drop = h('div', { class: 'drop' }, tr('Soltá acá archivos .py, .mpy o .aic para subirlos'));
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); upload(e.dataTransfer.files); };

  /* mip, on the board: typed into its console */
  const pkg = h('input', { type: 'text', class: 'mono', placeholder: 'umqtt.simple', spellcheck: 'false' });
  const install = () => {
    const n = pkg.value.trim();
    if (!/^[A-Za-z0-9_.\-/:]+$/.test(n)) { toast(tr('Un nombre de paquete de micropython-lib, o github:usuario/repo'), true); return; }
    if (con.busy()) { toast(tr('La consola está ocupada'), true); return; }
    con.typeLines([`__import__('mip').install(${JSON.stringify(n)})`]);
    toast(tr('Instalando %s: mirá la consola', n));
    pkg.value = '';
    setTimeout(load, 8000);
  };
  pkg.addEventListener('keydown', e => { if (e.key === 'Enter') install(); });

  const conSlot = h('div', {});
  put(listSec,
    h('div', { class: 'pyhead' }, h('h2', {}, tr('Consola')), h('span', { class: 'muted small' }, tr('la de la placa: lo que escribas acá corre allá'))),
    conSlot,
    h('div', { class: 'btns', style: 'margin:18px 0 0' },
      h('button', { class: 'btn pri', onclick: () => fresh('/python', PROGRAM, 'programa.py') }, tr('Nuevo programa')),
      h('button', { class: 'btn pri', onclick: () => fresh('/python/apps', APP, 'mi_app.py') }, tr('Nueva app')),
      h('div', { style: 'flex:1' }),
      h('span', { class: 'muted small' }, tr('Subir a')), upTo,
      h('button', { class: 'btn', onclick: () => fileIn.click() }, tr('Subir…')), fileIn),
    lists,
    h('div', { class: 'pyhead' }, h('h2', {}, tr('Instalar una biblioteca')), h('span', { class: 'muted small' }, tr('con mip, desde micropython-lib, a /python/lib'))),
    h('div', { class: 'card pad' }, h('div', { class: 'btns' }, h('div', { style: 'flex:1' }, pkg), h('button', { class: 'btn pri', onclick: install }, tr('Instalar')))),
    drop);
  put(edSec,
    h('div', { class: 'btns', style: 'margin-bottom:14px;flex-wrap:wrap;align-items:center' },
      h('button', { class: 'btn', onclick: back }, '← Python'), where,
      h('div', { style: 'flex:1 1 200px' }, nameIn), status),
    h('div', { class: 'pycols' },
      h('div', { style: 'min-width:0' }, ta,
        h('div', { class: 'btns', style: 'margin:12px 0' },
          h('button', { class: 'btn pri', onclick: save }, tr('Guardar en la placa')),
          h('button', { class: 'btn', onclick: saveRun }, tr('Guardar y correr')),
          h('button', { class: 'btn red', onclick: del }, tr('Borrar de la placa')),
          h('span', { class: 'muted small' }, tr('⌘S guarda · ⌘↩ guarda y corre'))),
        below),
      help));
  put(main, h('h1', {}, 'Python'), listSec, edSec,
    h('p', { class: 'note' }, tr('MicroPython 1.29 en la placa. Los programas viven en /python de la tarjeta y corren en su consola; las apps, en /python/apps, y cada una es una app del inicio que dibuja con el módulo aos (aparece después del próximo arranque). Mientras una app está abierta, la placa vigila su archivo y la vuelve a correr cuando cambia: guardar acá alcanza. Un error no reinicia nada: sale en la pantalla y acá, con su línea.')));
  window.onbeforeunload = () => dirty() && location.hash === '#python' ? tr('Hay cambios sin guardar') : undefined;
  show('list');
  return () => { con.stop(); if (outBox) outBox.stop(); };
}

P.registerPage({ id: 'python', name: 'Python', icon: '»', render: () => pagePython() });
