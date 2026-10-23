/* The Recorder's page in the board's portal (docs/PORTAL-PAGES.md): the
 * recordings on the card, to listen to, download or delete.
 *
 * They are the WAVs in /recordings, as the app writes them. Each one's
 * duration comes from its header, read with a Range request of its first
 * bytes (the firmware serves ranges since 0.8.0); the player streams the
 * file the same way, so it seeks without downloading it all. The app looks
 * at its folder every two seconds, so a recording deleted here leaves its
 * list too. The list here follows the card every five seconds, while
 * nothing plays. Texts in Spanish, through tr() (web.recorder.lang). */
const P = window.P4OS.forPage ? window.P4OS.forPage('recorder') : window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const LOC = { es: 'es-AR', en: 'en-US', de: 'de-DE', 'zh-Hans': 'zh-CN', 'zh-Hant': 'zh-TW' }[P.lang ? P.lang() : 'es'] || 'es-AR';
const DIR = '/recordings';
const APP_ID = 'app.recorder';

if (!document.getElementById('css-recorder')) {
  const st = document.createElement('style');
  st.id = 'css-recorder';
  st.textContent = `
.rcsum { display: flex; gap: 28px; flex-wrap: wrap; align-items: baseline; }
.rcsum b { font-size: 28px; font-variant-numeric: tabular-nums; }
.rcrow { padding: 14px 18px; border-top: 1px solid var(--line); }
.rcrow:first-child { border-top: 0; }
.rchead { display: flex; gap: 12px; align-items: baseline; flex-wrap: wrap; }
.rchead .nm { font-weight: 600; flex: 1; min-width: 140px; }
.rcrow audio { width: 100%; margin-top: 10px; height: 40px; }
.rcrow .btns { margin-top: 8px; }
`;
  document.head.append(st);
}

/* "grabacion_0007.wav" -> "Grabación 0007", as the app shows it (the word
 * in the board's language, like rec_files_pretty) */
const pretty = n => {
  let s = n.replace(/\.wav$/i, '').replace(/_/g, ' ');
  s = s.charAt(0).toUpperCase() + s.slice(1);
  return s.replace(/^Grabacion /, () => tr('Grabación') + ' ');
};
const when = t => t ? new Date(t * 1000).toLocaleString(LOC, { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }) : '';
const mmss = s => s == null ? '…' : `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

/* The WAV's duration from its first bytes: 'fmt ' gives the byte rate,
 * 'data' where the audio starts. A recording cut short may say 0 bytes of
 * data: then whatever is after the header counts. */
async function duration(path, size) {
  const r = await fetch(P.fsUrl(path), { headers: { Range: 'bytes=0-511' } });
  const b = new DataView(await r.arrayBuffer());
  const tag = o => String.fromCharCode(b.getUint8(o), b.getUint8(o + 1), b.getUint8(o + 2), b.getUint8(o + 3));
  if (b.byteLength < 12 || tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return null;
  let o = 12, rate = 0;
  while (o + 8 <= b.byteLength) {
    const id = tag(o), len = b.getUint32(o + 4, true);
    if (id === 'fmt ' && o + 20 <= b.byteLength) rate = b.getUint32(o + 16, true);
    if (id === 'data') {
      const data = len && len <= size - o - 8 ? len : size - o - 8;
      return rate ? data / rate : null;
    }
    o += 8 + len + (len & 1);
  }
  return null;
}

P.registerPage({
  id: 'grabadora',
  name: tr('Grabadora'),
  icon: '●',
  render(main) {
    const { h, put } = P;
    const sum = h('div', { class: 'card pad' });
    const list = h('div', { class: 'card' });
    const durs = new Map();          /* name + size -> seconds, so a refresh does not ask again */
    let files = [], sig = '', gone = false;

    const summary = () => {
      const total = files.reduce((a, f) => a + (durs.get(f.name + f.size) || 0), 0);
      const bytes = files.reduce((a, f) => a + f.size, 0);
      put(sum,
        h('div', { class: 'rcsum' },
          h('div', {}, h('b', {}, String(files.length)), ' ', h('span', { class: 'muted small' }, files.length === 1 ? tr('grabación') : tr('grabaciones'))),
          h('div', {}, h('b', {}, mmss(total)), ' ', h('span', { class: 'muted small' }, tr('en total'))),
          h('div', {}, h('b', {}, P.fmtBytes(bytes)), ' ', h('span', { class: 'muted small' }, tr('en la tarjeta')))),
        h('div', { class: 'btns', style: 'margin-top:12px' },
          h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir la Grabadora en la placa')),
          h('button', { class: 'btn', onclick: () => load(true) }, tr('Actualizar'))),
        h('p', { class: 'note' }, tr('Se graba desde la placa. Acá se escuchan, se bajan como WAV y se borran; la app ve los cambios sola.')));
    };

    const remove = async f => {
      if (!confirm(tr('¿Borrar %s? No se puede deshacer.', pretty(f.name)))) return;
      try { await P.fsDelete(`${DIR}/${f.name}`); P.toast(tr('Borrada')); load(true); }
      catch (e) { P.toast(e.message, true); }
    };

    const draw = () => {
      if (!files.length) {
        put(list, h('div', { class: 'row muted' }, tr('Todavía no hay grabaciones. Se graban desde la app en la placa.')));
        summary();
        return;
      }
      put(list, files.map(f => {
        const path = `${DIR}/${f.name}`;
        const dur = h('span', { class: 'val' }, mmss(durs.get(f.name + f.size)));
        if (!durs.has(f.name + f.size))
          duration(path, f.size).then(s => { durs.set(f.name + f.size, s); dur.textContent = mmss(s); summary(); }).catch(() => { dur.textContent = '?'; });
        return h('div', { class: 'rcrow' },
          h('div', { class: 'rchead' },
            h('span', { class: 'nm' }, pretty(f.name)),
            h('span', { class: 'muted small' }, `${when(f.mtime)} · ${P.fmtBytes(f.size)}`), dur),
          h('audio', { controls: true, preload: 'none', src: P.fsUrl(path) }),
          h('div', { class: 'btns' },
            h('a', { class: 'btn', href: P.fsUrl(path, true) }, tr('Bajar')),
            h('button', { class: 'btn red', onclick: () => remove(f) }, tr('Borrar'))));
      }));
      summary();
    };

    /* newest first, like the app; redrawn only when the folder changed, and
     * never under a player that is playing */
    async function load(force) {
      let fl;
      try { fl = await P.fsList(DIR); } catch (e) { put(list, h('div', { class: 'row bad' }, e.message)); return; }
      if (gone) return;
      const now = fl.filter(f => !f.dir && !f.name.startsWith('.') && /\.wav$/i.test(f.name))
        .sort((a, b) => (b.mtime || 0) - (a.mtime || 0) || b.name.localeCompare(a.name));
      const s = now.map(f => f.name + f.size + f.mtime).join('|');
      if (!force && s === sig) return;
      if (!force && [...list.querySelectorAll('audio')].some(a => !a.paused)) return;
      sig = s;
      files = now;
      draw();
    }

    put(main, h('h1', {}, tr('Grabadora')), sum, h('h2', {}, tr('Grabaciones')), list);
    summary();
    load(true);
    const t = setInterval(load, 5000);
    return () => { gone = true; clearInterval(t); };
  },
});
