/* The exchange rates app's page in the board's portal (docs/PORTAL-PAGES.md):
 * which rates the board shows, with today's values beside them.
 *
 * The choice is one line in /data/cotiz.txt, the comma-separated keys the
 * app knows (cz_api.c), which it reads within three seconds if it is open.
 * The app draws them in its own fixed order, not the line's, so ticking one
 * more does not reorder the screen. The values come from dolarapi.com, asked
 * from the browser, as the app asks from the board. */
const P = window.P4OS.forPage ? window.P4OS.forPage('cotiz') : window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const LOC = { es: 'es-AR', en: 'en-US', de: 'de-DE', 'zh-Hans': 'zh-CN', 'zh-Hant': 'zh-TW' }[P.lang ? P.lang() : 'es'] || 'es-AR';
const FILE = '/data/cotiz.txt';
const APP_ID = 'aos.cotiz';
/* the app's CZ_ESPECIES, in its order; 'match' is dolarapi's casa or moneda */
const KINDS = [
  { key: 'oficial', name: N_('Oficial'), match: 'oficial', dollar: true },
  { key: 'blue', name: N_('Blue'), match: 'blue', dollar: true },
  { key: 'bolsa', name: N_('Bolsa (MEP)'), match: 'bolsa', dollar: true },
  { key: 'contadoconliqui', name: N_('Contado c/liqui'), match: 'contadoconliqui', dollar: true },
  { key: 'mayorista', name: N_('Mayorista'), match: 'mayorista', dollar: true },
  { key: 'cripto', name: N_('Cripto'), match: 'cripto', dollar: true },
  { key: 'tarjeta', name: N_('Tarjeta'), match: 'tarjeta', dollar: true },
  { key: 'eur', name: N_('Euro'), match: 'EUR', dollar: false },
  { key: 'brl', name: N_('Real'), match: 'BRL', dollar: false },
  { key: 'clp', name: N_('Peso chileno'), match: 'CLP', dollar: false },
  { key: 'uyu', name: N_('Peso uruguayo'), match: 'UYU', dollar: false },
];
const DEFAULT = 'oficial,blue,bolsa,contadoconliqui,cripto,tarjeta,eur';   /* the app's LISTA_DEF */

const money = v => v == null ? '–' : '$ ' + Number(v).toLocaleString(LOC, { minimumFractionDigits: 2, maximumFractionDigits: 2 });

P.registerPage({
  id: 'cotiz',
  name: tr('Cotizaciones'),
  icon: '$',
  render(main) {
    const { h, put } = P;
    const list = h('div', { class: 'card' }, h('div', { class: 'row muted' }, tr('Cargando…')));
    const when = h('p', { class: 'note' });
    const boxes = {};
    let values = {}, gone = false;

    const draw = chosen => {
      put(list, KINDS.map(k => {
        const box = boxes[k.key] = h('input', { type: 'checkbox' });
        box.checked = chosen.includes(k.key);
        const v = values[k.key];
        return h('label', { class: 'row click', style: 'gap:14px' }, box,
          h('div', { class: 'grow' }, k.dollar ? tr('Dólar %s', tr(k.name)) : tr(k.name)),
          h('div', { class: 'val' }, v ? `${money(v.compra)} / ${money(v.venta)}` : ''));
      }));
    };

    async function save() {
      const keys = KINDS.filter(k => boxes[k.key].checked).map(k => k.key);
      if (!keys.length) { P.toast(tr('Elegí al menos una'), true); return; }
      try {
        await P.fsMkdir('/data');
        await P.fsPut(FILE, keys.join(',') + '\n');
        P.toast(tr('Guardado: la placa lo toma en unos segundos'));
      } catch (e) { P.toast(e.message, true); }
    }

    async function load() {
      let line = null;
      try { line = await P.fsText(FILE); } catch (e) { P.toast(e.message, true); }
      const chosen = (line && line.trim() ? line : DEFAULT).split(/[,\s]+/).filter(Boolean);
      if (gone) return;
      draw(chosen);
      try {
        const [d, c] = await Promise.all([fetch('https://dolarapi.com/v1/dolares').then(r => r.json()),
          fetch('https://dolarapi.com/v1/cotizaciones').then(r => r.json())]);
        let latest = '';
        for (const k of KINDS) {
          const v = k.dollar ? d.find(x => x.casa === k.match) : c.find(x => x.moneda === k.match);
          if (v) { values[k.key] = v; if ((v.fechaActualizacion || '') > latest) latest = v.fechaActualizacion; }
        }
        if (gone) return;
        draw(KINDS.filter(k => boxes[k.key] && boxes[k.key].checked).map(k => k.key));
        when.textContent = latest
          ? tr('Compra / venta, de dolarapi.com, actualizado %s.', new Date(latest).toLocaleString(LOC, { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }))
          : tr('Compra / venta, de dolarapi.com.');
      } catch (e) { when.textContent = tr('No se pudieron traer los valores: %s', e.message); }
    }

    put(main, h('h1', {}, tr('Cotizaciones')),
      h('h2', {}, tr('Qué muestra la placa')), list, when,
      h('div', { class: 'btns', style: 'margin-top:10px' },
        h('button', { class: 'btn pri', onclick: save }, tr('Guardar')),
        h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir Cotizaciones en la placa'))),
      h('p', { class: 'note' }, tr('La placa las muestra siempre en este orden; si la app está abierta, cambia sola en unos segundos.')));
    load();
    return () => { gone = true; };
  },
});
