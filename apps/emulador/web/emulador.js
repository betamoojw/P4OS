/* The Emulador's page in the board's portal (docs/PORTAL-PAGES.md): the
 * games, BIOS files and battery saves of each console, on the card under
 * /emulador/<console>/ (apps/emulador/main/em_files.c has the layout).
 *
 *   - drop games (or pick them) to upload them to the console's folder;
 *   - the games already there, with the picture each was left on (the
 *     automatic state's thumbnail, "ETH1" + width + height + RGB565), the
 *     time played (recientes.txt) and its saves;
 *   - a battery save can be downloaded, or uploaded from another emulator
 *     (the usual raw .sav, named as the game);
 *   - the console's BIOS files, in its bios/ folder.
 *
 * The consoles listed here are the app's (emu_cores.c): a new core adds
 * its line to CONSOLES. */
const P = window.P4OS;
const { h, put, toast, fmtBytes, fsPut, fsList, fsDelete, fsMkdir, fsBytes, fsText, fsUrl, openApp, saveBlob } = P;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });

const ROOT = '/emulador';
/* The consoles: the app writes the ones it has to consolas.txt each time it
 * opens; these are the notes about their system files, and the list to use
 * before the app has ever run. */
const BIOS_NOTES = {
  nes: tr('Opcional: disksys.rom para el Famicom Disk System.'),
  gba: tr('Opcional: gba_bios.bin (la BIOS original mejora la compatibilidad; sin ella se usa una libre que trae el emulador).'),
};
let CONSOLES = [
  { id: 'nes', name: 'NES', exts: ['.nes', '.fds', '.nsf'] },
  { id: 'gba', name: 'Game Boy Advance', exts: ['.gba', '.agb', '.bin'] },
];
async function loadConsoles() {
  const txt = await fsText(ROOT + '/consolas.txt').catch(() => null);
  if (!txt) return;
  const list = txt.split('\n').map(l => l.split('\t')).filter(f => f.length >= 4 && f[0])
    .map(([id, name, short, exts]) => ({ id, name, short, exts: exts.trim().split(/\s+/) }));
  if (list.length) CONSOLES = list;
}

if (!document.getElementById('css-emulador')) {
  const st = document.createElement('style');
  st.id = 'css-emulador';
  st.textContent = `
.emtabs { display: flex; gap: 8px; flex-wrap: wrap; margin: 4px 0 14px; }
.emtabs .btn.on { background: var(--accent); color: #fff; border-color: var(--accent); }
.emdrop { border: 2px dashed var(--line); border-radius: 14px; padding: 22px; text-align: center; color: var(--dim); cursor: pointer; transition: border-color .15s, background .15s; }
.emdrop.over { border-color: var(--accent); background: var(--card2); color: var(--text); }
.emdrop b { color: var(--text); }
.emq { display: flex; flex-direction: column; gap: 6px; margin-top: 10px; }
.emq .row { gap: 10px; }
.emlist { display: grid; gap: 10px; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); }
.emgame { display: flex; gap: 12px; align-items: center; background: var(--card); border-radius: 14px; padding: 10px; min-width: 0; }
.emgame canvas, .emgame .emnopic { width: 96px; height: 72px; border-radius: 8px; background: #000; flex: none; image-rendering: pixelated; object-fit: contain; }
.emnopic { display: flex; align-items: center; justify-content: center; font-weight: 600; color: #fff; font-size: 13px; }
.emgame .grow { min-width: 0; }
.emgame .t { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.emgame .s { font-size: 12.5px; color: var(--dim); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.emgame .btns { flex-wrap: wrap; gap: 6px; margin-top: 6px; }
.emgame .btns .btn { padding: 3px 9px; font-size: 12px; }
.emsearch { width: 100%; max-width: 360px; }
`;
  document.head.append(st);
}

const lower = s => s.toLowerCase();
const extOf = n => { const i = n.lastIndexOf('.'); return i > 0 ? lower(n.slice(i)) : ''; };
const baseOf = n => { const i = n.lastIndexOf('.'); return i > 0 ? n.slice(0, i) : n; };
/* as em_files.c: "sub/Some Game.nes" -> "sub_Some Game" */
const flat = rel => baseOf(rel).replace(/\//g, '_');
const RESERVED = new Set(['bios', 'saves', 'states', 'covers']);

function cleanTitle(name) {
  let t = baseOf(name.split('/').pop());
  let tags = '';
  const m = t.search(/[([]/);
  if (m > 0) { tags = t.slice(m); t = t.slice(0, m).trim(); }
  const arts = [', The', ', A', ', El', ', La', ', Los', ', Las', ', Die', ', Der', ', Das'];
  for (const a of arts) if (t.endsWith(a)) { t = a.slice(2) + ' ' + t.slice(0, -a.length); break; }
  return { title: t.replace(/_/g, ' '), tags };
}

function played(s) {
  if (!s) return '';
  if (s < 60) return tr('menos de 1 min');
  if (s < 3600) return tr('%d min', Math.floor(s / 60));
  return tr('%d h %d min', Math.floor(s / 3600), Math.floor(s % 3600 / 60));
}

/* the games of a console: its folder and up to two levels of subfolders */
async function listGames(con) {
  const out = [];
  async function walk(sub, depth) {
    const dir = ROOT + '/' + con.id + (sub ? '/' + sub : '');
    const items = await fsList(dir);
    for (const it of items) {
      if (it.name.startsWith('.')) continue;
      const rel = sub ? sub + '/' + it.name : it.name;
      if (it.dir) {
        if (!sub && RESERVED.has(it.name)) continue;
        if (depth < 2) await walk(rel, depth + 1);
      } else if (con.exts.includes(extOf(it.name))) {
        out.push({ rel, size: it.size, mtime: it.mtime, ...cleanTitle(it.name) });
      }
    }
  }
  await walk('', 0);
  return out;
}

async function readRecents() {
  const txt = await fsText(ROOT + '/recientes.txt').catch(() => null);
  const map = new Map();
  if (!txt) return map;
  for (const line of txt.split('\n')) {
    const [order, secs, key] = line.split('\t');
    if (key) map.set(key.trim(), { order: +order, secs: +secs });
  }
  return map;
}

/* a state's thumbnail drawn into a canvas */
async function drawThumb(cv, path) {
  try {
    const buf = await fsBytes(path);
    const dv = new DataView(buf);
    if (dv.getUint32(0) !== 0x45544831) return false;     /* "ETH1" */
    const w = dv.getUint16(4, true), hh = dv.getUint16(6, true);
    if (!w || !hh || buf.byteLength < 8 + w * hh * 2) return false;
    cv.width = w; cv.height = hh;
    const ctx = cv.getContext('2d');
    const img = ctx.createImageData(w, hh);
    for (let i = 0; i < w * hh; i++) {
      const p = dv.getUint16(8 + i * 2, true);
      img.data[i * 4] = ((p >> 11) & 31) * 255 / 31;
      img.data[i * 4 + 1] = ((p >> 5) & 63) * 255 / 63;
      img.data[i * 4 + 2] = (p & 31) * 255 / 31;
      img.data[i * 4 + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
    return true;
  } catch (e) { return false; }
}

function render(main) {
  let con = CONSOLES[0];
  let alive = true;
  const tabs = h('div', { class: 'emtabs' });
  const body = h('div');
  put(main,
    h('div', { class: 'row' },
      h('h1', { class: 'grow' }, tr('Emulador')),
      h('button', { class: 'btn pri', onclick: () => openApp('demo.emulador').catch(e => toast(e.message, true)) }, tr('Abrir en la placa'))),
    h('p', { class: 'muted' }, tr('Los juegos van en la tarjeta, una carpeta por consola (%s). También pueden ir en subcarpetas.', ROOT + '/<consola>')),
    tabs, body);

  function drawTabs() {
    tabs.replaceChildren(...CONSOLES.map(c =>
      h('button', { class: 'btn' + (c === con ? ' on' : ''), onclick: () => { con = c; drawTabs(); load(); } }, c.short || c.name)));
  }

  /* the app makes these when it opens; the page too, for a card that has
   * never seen the app */
  async function ensureDirs() {
    const root = await fsList(ROOT);
    if (!root.length) await fsMkdir(ROOT).catch(() => {});
    const top = new Set(root.map(i => i.name));
    if (!top.has(con.id)) await fsMkdir(ROOT + '/' + con.id).catch(() => {});
    const have = new Set((await fsList(ROOT + '/' + con.id)).map(i => i.name));
    for (const d of ['bios', 'saves', 'states']) {
      if (!have.has(d)) await fsMkdir(ROOT + '/' + con.id + '/' + d).catch(() => {});
    }
  }

  async function upload(files, dest, okExts) {
    const q = h('div', { class: 'emq' });
    body.querySelector('.emqhost')?.replaceChildren(q);
    let n = 0;
    for (const f of files) {
      if (okExts && !okExts.includes(extOf(f.name))) {
        toast(tr('%s no es un juego de %s (%s)', f.name, con.name, okExts.join(' ')), true);
        continue;
      }
      const prog = h('progress', { class: 'prog grow', max: 1, value: 0 });
      q.append(h('div', { class: 'row' }, h('span', { class: 'small' }, f.name), prog));
      try {
        await fsPut(dest + '/' + f.name, f, p => { prog.value = p; });
        n++;
      } catch (e) {
        toast(f.name + ': ' + e.message, true);
      }
    }
    if (n) toast(tr('%d archivos subidos', n));
    if (alive) load();
  }

  function dropZone(label, dest, okExts) {
    const input = h('input', { type: 'file', multiple: true, style: 'display:none',
      onchange: () => upload([...input.files], dest, okExts) });
    const z = h('div', { class: 'emdrop', onclick: () => input.click() }, input, h('div', { html: label }));
    z.addEventListener('dragover', e => { e.preventDefault(); z.classList.add('over'); });
    z.addEventListener('dragleave', () => z.classList.remove('over'));
    z.addEventListener('drop', e => { e.preventDefault(); z.classList.remove('over'); upload([...e.dataTransfer.files], dest, okExts); });
    return z;
  }

  async function load() {
    const id = con.id;
    put(body, h('p', { class: 'muted' }, tr('Leyendo la tarjeta...')));
    await ensureDirs();
    const [games, recents, saves, states, bios] = await Promise.all([
      listGames(con), readRecents(), fsList(ROOT + '/' + id + '/saves'), fsList(ROOT + '/' + id + '/states'),
      fsList(ROOT + '/' + id + '/bios')]);
    if (!alive || con.id !== id) return;
    const saveSet = new Map(saves.filter(s => !s.dir).map(s => [s.name, s]));
    const stateSet = new Set(states.map(s => s.name));
    for (const g of games) {
      const r = recents.get(id + '/' + g.rel);
      g.order = r ? r.order : 0;
      g.secs = r ? r.secs : 0;
    }
    games.sort((a, b) => (b.order - a.order) || a.title.localeCompare(b.title));

    const search = h('input', { class: 'emsearch', type: 'search', placeholder: tr('Buscar') });
    const list = h('div', { class: 'emlist' });
    const exts = con.exts.join(' ');
    put(body,
      h('div', { class: 'card pad' },
        dropZone(tr('<b>Soltá acá juegos de %s</b> o tocá para elegirlos<br><span class="small">%s</span>', con.name, exts),
                 ROOT + '/' + id, con.exts),
        h('div', { class: 'emqhost' })),
      h('div', { class: 'row', style: 'margin:16px 0 8px' },
        h('h2', { class: 'grow' }, games.length === 1 ? tr('1 juego') : tr('%d juegos', games.length)), search),
      list,
      h('h2', { style: 'margin-top:22px' }, tr('BIOS y archivos de sistema')),
      h('div', { class: 'card pad' },
        h('p', { class: 'small muted' }, BIOS_NOTES[con.id] || tr('Los archivos de sistema que pida la consola (el emulador avisa cuáles).')),
        ...bios.filter(b => !b.dir && !b.name.startsWith('.')).map(b =>
          h('div', { class: 'row' }, h('span', { class: 'grow mono' }, b.name), h('span', { class: 'small muted' }, fmtBytes(b.size)),
            h('button', { class: 'btn red', onclick: () => confirm(tr('¿Borrar %s?', b.name)) &&
              fsDelete(ROOT + '/' + id + '/bios/' + b.name).then(load, e => toast(e.message, true)) }, tr('Borrar')))),
        dropZone(tr('Soltá acá los archivos de sistema'), ROOT + '/' + id + '/bios', null)));

    function card(g) {
      const base = flat(g.rel);
      const sav = saveSet.get(base + '.sav');
      const slots = [0, 1, 2, 3, 4].filter(i => stateSet.has(base + '.st' + i));
      const cv = h('canvas');
      const pic = h('div', { class: 'emnopic', style: 'background:var(--card2)' }, con.id.toUpperCase());
      const holder = h('div', {}, pic);
      if (stateSet.has(base + '.st0.thm')) {
        drawThumb(cv, ROOT + '/' + id + '/states/' + base + '.st0.thm').then(ok => { if (ok) holder.replaceChildren(cv); });
      }
      const info = [g.tags, fmtBytes(g.size), g.secs ? tr('jugado %s', played(g.secs)) : '',
        slots.length ? tr('%d estados', slots.length) : ''].filter(Boolean).join(' · ');
      const savInput = h('input', { type: 'file', accept: '.sav,.srm', style: 'display:none', onchange: async () => {
        const f = savInput.files[0];
        if (!f) return;
        try { await fsPut(ROOT + '/' + id + '/saves/' + base + '.sav', f); toast(tr('Partida guardada subida')); load(); }
        catch (e) { toast(e.message, true); }
      } });
      return h('div', { class: 'emgame', 'data-t': lower(g.title + ' ' + g.rel) },
        holder,
        h('div', { class: 'grow' },
          h('div', { class: 't', title: g.rel }, g.title),
          h('div', { class: 's' }, info),
          h('div', { class: 'btns row' },
            sav ? h('a', { class: 'btn', href: fsUrl(ROOT + '/' + id + '/saves/' + base + '.sav', true) }, tr('Bajar partida')) : null,
            h('button', { class: 'btn', onclick: () => savInput.click() }, tr('Subir partida')), savInput,
            slots.length ? h('button', { class: 'btn', onclick: async () => {
              if (!confirm(tr('¿Borrar los %d estados de %s?', slots.length, g.title))) return;
              for (const i of slots) {
                await fsDelete(ROOT + '/' + id + '/states/' + base + '.st' + i).catch(() => {});
                await fsDelete(ROOT + '/' + id + '/states/' + base + '.st' + i + '.thm').catch(() => {});
              }
              load();
            } }, tr('Borrar estados')) : null,
            h('button', { class: 'btn red', onclick: async () => {
              if (!confirm(tr('¿Borrar %s de la tarjeta? Las partidas guardadas quedan.', g.title))) return;
              try { await fsDelete(ROOT + '/' + id + '/' + g.rel); load(); } catch (e) { toast(e.message, true); }
            } }, tr('Borrar')))));
    }

    if (!games.length) {
      list.append(h('p', { class: 'muted' }, tr('Todavía no hay juegos de %s.', con.name)));
    } else {
      list.append(...games.map(card));
    }
    search.addEventListener('input', () => {
      const q = lower(search.value.trim());
      for (const c of list.children) c.style.display = !q || (c.dataset.t || '').includes(q) ? '' : 'none';
    });
  }

  loadConsoles().then(() => { con = CONSOLES[0]; drawTabs(); return load(); }).catch(e => toast(e.message, true));
  return () => { alive = false; };
}

P.registerPage({ id: 'emulador', name: tr('Emulador'), icon: '🕹', render });
