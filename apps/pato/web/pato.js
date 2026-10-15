/* Pato goma's page in the board's portal (docs/PORTAL-PAGES.md).
 *
 * The keyboard-and-mouse scripts the board can play over USB (or the Bluetooth
 * keyboard) onto the computer. They live on the card as .pato text files in
 * /pato; the app on the board reads, picks and confirms them, and can also
 * edit them now. This page is the roomy editor: a list of scripts, a builder
 * (one row per step) and a raw-text view, saved to the card.
 *
 * The .pato format is the contract shared with the app (apps/pato/main/pato.c);
 * both sides read and write the same text. The page's texts are in Spanish,
 * like the rest of the portal, and tr() puts them in the board's language
 * (web.pato.lang); the script's commands (STRING, KEY, DELAY...) never. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const DIR = '/pato';

/* a style sheet of our own, added once; classes prefixed 'pt' */
function ensureCss() {
  if (document.getElementById('css-pato')) return;
  const s = document.createElement('style');
  s.id = 'css-pato';
  s.textContent = `
    .ptlist { display:flex; flex-direction:column; gap:10px; margin-top:6px; }
    .ptrow { background:var(--card); border-radius:12px; padding:12px 14px;
             display:flex; align-items:center; gap:12px; }
    .ptrow b { font-weight:500; flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .ptrow .meta { color:var(--dim); font-size:13px; }
    .pthead { display:flex; gap:10px; align-items:center; flex-wrap:wrap; margin-bottom:12px; }
    .pthead input { flex:1 1 200px; padding:10px 12px; }
    .ptstatus { flex-basis:100%; color:var(--dim); font-size:13px; min-height:18px; }
    .ptstatus.ok { color:var(--green); } .ptstatus.bad { color:var(--red); }
    .pttabs { display:flex; gap:8px; margin-bottom:14px; }
    .pttabs button { background:var(--card2); }
    .pttabs button.on { background:var(--accent); color:#fff; }
    .ptbody { display:flex; gap:18px; align-items:flex-start; flex-wrap:wrap; }
    .ptmain { flex:1 1 340px; min-width:280px; }
    .pthelp { flex:1 1 220px; min-width:200px; background:var(--card); border-radius:12px;
              padding:14px 16px; font-size:13px; color:var(--dim); }
    .pthelp h3 { margin:0 0 8px; font-size:12px; text-transform:uppercase; letter-spacing:.05em; }
    .pthelp dl { margin:0; display:grid; grid-template-columns:auto 1fr; gap:4px 10px; }
    .pthelp dt { color:var(--accent); font-family:ui-monospace,Menlo,monospace; white-space:nowrap; }
    .pthelp dd { margin:0; }
    .ptsteps { display:flex; flex-direction:column; gap:8px; }
    .ptstep { background:var(--card); border-radius:11px; padding:8px 10px;
              display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
    .ptstep select, .ptstep input { padding:7px 8px; font-size:13.5px; }
    .ptstep .campo { flex:1 1 120px; min-width:90px; }
    .ptstep .num { flex:0 0 90px; width:90px; }
    .ptstep .mv, .ptstep .rm { background:var(--card2); padding:6px 10px; border-radius:8px; }
    .ptstep .rm { color:var(--red); }
    .ptadd { display:flex; gap:8px; margin-top:12px; flex-wrap:wrap; }
    .ptadd select { flex:1 1 160px; padding:9px 10px; }
    #pttext { width:100%; min-height:300px; box-sizing:border-box; padding:12px 14px;
              font-family:ui-monospace,Menlo,monospace; font-size:14px; line-height:1.5;
              background:var(--card); color:var(--text); border:1px solid var(--line);
              border-radius:12px; resize:vertical; }
    .ptfoot { margin-top:22px; font-size:13px; color:var(--dim); }`;
  document.head.appendChild(s);
}

/* ---- the model: a step is { op, v, v2 } ---------------------------------- */
const OPS = ['string', 'key', 'delay', 'mouse', 'scroll', 'click', 'repeat', 'com'];
const OP_NAME = {
  string: N_('Escribir'), key: N_('Tecla'), delay: N_('Esperar'), mouse: N_('Mouse'),
  scroll: N_('Rueda'), click: N_('Clic'), repeat: N_('Repetir'), com: N_('Comentario'), raw: N_('Otro'),
};

function parseText(txt) {
  const steps = [];
  txt.split('\n').forEach(line => {
    const s = line.replace(/\s+$/, '');
    if (s.trim() === '') { steps.push({ op: 'blank' }); return; }
    if (s.trim()[0] === '#') { steps.push({ op: 'com', v: s.trim().replace(/^#\s?/, '') }); return; }
    const m = s.trim().match(/^(\S+)\s*(.*)$/);
    const verb = m[1].toUpperCase(), arg = m[2];
    if (verb === 'STRING' || verb === 'TYPE') steps.push({ op: 'string', v: arg });
    else if (verb === 'KEY' || verb === 'PRESS') steps.push({ op: 'key', v: arg });
    else if (verb === 'DELAY' || verb === 'WAIT') steps.push({ op: 'delay', v: parseInt(arg) || 0 });
    else if (verb === 'MOUSE' || verb === 'MOVE') {
      const p = arg.split(/\s+/);
      steps.push({ op: 'mouse', v: parseInt(p[0]) || 0, v2: parseInt(p[1]) || 0 });
    } else if (verb === 'SCROLL') steps.push({ op: 'scroll', v: parseInt(arg) || 0 });
    else if (verb === 'CLICK') steps.push({ op: 'click', v: (parseInt(arg) === 2) ? 2 : 1 });
    else if (verb === 'REPEAT') steps.push({ op: 'repeat', v: parseInt(arg) || 1 });
    else steps.push({ op: 'raw', v: s.trim() });
  });
  return steps;
}

function serialize(steps) {
  return steps.map(p => {
    switch (p.op) {
      case 'blank': return '';
      case 'com': return '# ' + (p.v || '');
      case 'raw': return p.v || '';
      case 'string': return 'STRING ' + (p.v || '');
      case 'key': return 'KEY ' + (p.v || '');
      case 'delay': return 'DELAY ' + (parseInt(p.v) || 0);
      case 'mouse': return 'MOUSE ' + (parseInt(p.v) || 0) + ' ' + (parseInt(p.v2) || 0);
      case 'scroll': return 'SCROLL ' + (parseInt(p.v) || 0);
      case 'click': return 'CLICK ' + ((parseInt(p.v) === 2) ? 2 : 1);
      case 'repeat': return 'REPEAT ' + (parseInt(p.v) || 1);
      default: return '';
    }
  }).join('\n');
}

function countSteps(steps) {
  let n = 0;
  steps.forEach(p => {
    if (p.op === 'blank' || p.op === 'com' || p.op === 'raw') return;
    if (p.op === 'repeat') { n += (parseInt(p.v) || 0); return; }
    n += 1;
  });
  return n;
}

function cleanName(n) {
  return n.replace(/[\\/:*?"<>|]/g, '').replace(/\s+/g, ' ').trim().slice(0, 40);
}

P.registerPage({
  id: 'pato',
  name: tr('Pato goma'),
  icon: '\u{1F986}',   /* 🦆 */
  render(main) {
    ensureCss();
    const { h, put } = P;

    let steps = [];
    let original = null;     /* file name being edited (for rename/delete) */
    let files = [];
    let textView = false;

    /* ---------- elements ---------- */
    const listWrap = h('div', { class: 'ptlist' });
    const emptyMsg = h('p', { class: 'muted', style: 'margin-top:10px' },
      tr('Todavía no hay scripts. Creá el primero con «Nuevo script».'));
    const listView = h('div', {},
      h('div', { class: 'row' },
        h('h2', { class: 'grow', style: 'margin:0' }, tr('Scripts')),
        h('button', { class: 'btn pri', onclick: () => newScript() }, tr('Nuevo script'))),
      listWrap, emptyMsg);

    const nameInput = h('input', { placeholder: tr('Nombre del script'), maxlength: 40 });
    const statusEl = h('div', { class: 'ptstatus' });
    const stepsEl = h('div', { class: 'ptsteps' });
    const typeSel = h('select', {}, ...OPS.map(op => h('option', { value: op }, tr(OP_NAME[op]))));
    const textEl = h('textarea', { id: 'pttext', spellcheck: 'false' });

    const tabBuilder = h('button', { class: 'btn on', onclick: () => toBuilder() }, tr('Constructor'));
    const tabText = h('button', { class: 'btn', onclick: () => toText() }, tr('Texto'));

    const builderBox = h('div', { id: 'ptvb' },
      stepsEl,
      h('div', { class: 'ptadd' }, typeSel,
        h('button', { class: 'btn', onclick: () => addStep() }, tr('Agregar paso'))));
    const textBox = h('div', { id: 'ptvt', style: 'display:none' }, textEl);

    const help = h('aside', { class: 'pthelp' },
      h('h3', {}, tr('Comandos')),
      h('dl', {},
        h('dt', {}, tr(OP_NAME.string)), h('dd', {}, tr('teclea el texto')),
        h('dt', {}, tr(OP_NAME.key)), h('dd', { html: tr('una tecla o combo: <code>enter</code>, <code>cmd+space</code>, <code>f5</code>') }),
        h('dt', {}, tr(OP_NAME.delay)), h('dd', {}, tr('milisegundos')),
        h('dt', {}, tr(OP_NAME.mouse)), h('dd', { html: tr('mueve el puntero <code>dx dy</code>') }),
        h('dt', {}, tr(OP_NAME.scroll)), h('dd', {}, tr('+ arriba, − abajo')),
        h('dt', {}, tr(OP_NAME.click)), h('dd', {}, tr('izquierdo o derecho')),
        h('dt', {}, tr(OP_NAME.repeat)), h('dd', {}, tr('repite el paso anterior n veces')),
        h('dt', {}, tr(OP_NAME.com)), h('dd', {}, tr('no se ejecuta'))),
      h('p', { class: 'small', style: 'margin-top:12px' },
        tr('Prefijos de combo: cmd+ ctrl+ alt+ shift+ sobre una tecla.')));

    const editView = h('div', { style: 'display:none' },
      h('div', { class: 'pthead' },
        h('button', { class: 'btn', onclick: () => showList() }, tr('← Scripts')),
        nameInput, statusEl),
      h('div', { class: 'pttabs' }, tabBuilder, tabText),
      h('div', { class: 'ptbody' }, h('div', { class: 'ptmain' }, builderBox, textBox), help),
      h('div', { class: 'btns', style: 'margin-top:16px' },
        h('button', { class: 'btn pri', onclick: () => save() }, tr('Guardar en la placa')),
        h('button', { class: 'btn red', onclick: () => deleteCurrent() }, tr('Borrar de la placa'))));

    put(main,
      h('h1', {}, tr('Pato goma')),
      h('p', { class: 'note' },
        tr('Los scripts de teclado y mouse que la placa puede ejecutar por USB (o como teclado Bluetooth) en la computadora. Se guardan en la tarjeta; en la placa los elegís y confirmás antes de que se envíe nada. También podés editarlos ahí.')),
      listView, editView,
      h('p', { class: 'ptfoot' },
        tr('Los archivos .pato viven en /pato de la tarjeta. No pongas contraseñas: cualquier computadora lee lo que escribe un teclado USB.')),
      h('p', { class: 'ptfoot' },
        tr('Esta app es sólo con fines educativos y demostrativos de las capacidades HID de la placa. No nos hacemos responsables de los scripts que ejecuten terceros ni de los malos usos que le puedan dar.')));

    /* ---------- navigation ---------- */
    function showList() { listView.style.display = ''; editView.style.display = 'none'; loadList(); }
    function showEdit() { listView.style.display = 'none'; editView.style.display = ''; }

    function status(msg, cls) { statusEl.textContent = msg || ''; statusEl.className = 'ptstatus' + (cls ? ' ' + cls : ''); }

    /* ---------- the list ---------- */
    async function loadList() {
      try {
        files = (await P.fsList(DIR)).filter(f => !f.dir && /\.pato$/i.test(f.name));
      } catch (e) { files = []; P.toast(e.message, true); }
      put(listWrap, ...files.map(f => {
        const base = f.name.replace(/\.pato$/i, '');
        return h('div', { class: 'ptrow' },
          h('b', {}, base),
          h('span', { class: 'meta' }, P.fmtBytes(f.size)),
          h('button', { class: 'btn', onclick: () => open(f.name) }, tr('Editar')),
          h('button', { class: 'btn red', onclick: () => removeFile(f.name) }, '✕'));
      }));
      emptyMsg.style.display = files.length ? 'none' : '';
    }

    /* ---------- the builder ---------- */
    function syncFromDom() {
      stepsEl.querySelectorAll('.ptstep').forEach(row => {
        const i = parseInt(row.dataset.i);
        row.querySelectorAll('[data-f]').forEach(inp => { if (steps[i]) steps[i][inp.dataset.f] = inp.value; });
      });
    }

    function paintSteps() {
      put(stepsEl, ...steps.map((p, i) => {
        if (p.op === 'blank') return false;
        const opSel = h('select', { class: 'op' },
          ...OPS.map(op => h('option', { value: op, selected: (op === (p.op === 'raw' ? 'com' : p.op)) }, tr(OP_NAME[op]))));
        opSel.onchange = () => { syncFromDom(); steps[i].op = opSel.value; paintSteps(); };

        let field;
        if (p.op === 'string') field = h('input', { class: 'campo', 'data-f': 'v', placeholder: tr('texto a escribir'), value: p.v || '' });
        else if (p.op === 'key') field = h('input', { class: 'campo', 'data-f': 'v', placeholder: 'enter, cmd+space...', value: p.v || '' });
        else if (p.op === 'delay') field = h('span', {}, h('input', { class: 'num', type: 'number', min: 0, max: 60000, 'data-f': 'v', value: parseInt(p.v) || 0 }), ' ms');
        else if (p.op === 'mouse') field = h('span', {}, 'dx ', h('input', { class: 'num', type: 'number', 'data-f': 'v', value: parseInt(p.v) || 0 }), ' dy ', h('input', { class: 'num', type: 'number', 'data-f': 'v2', value: parseInt(p.v2) || 0 }));
        else if (p.op === 'scroll') field = h('input', { class: 'num', type: 'number', 'data-f': 'v', value: parseInt(p.v) || 0 });
        else if (p.op === 'click') field = h('select', { 'data-f': 'v' }, h('option', { value: '1', selected: p.v !== 2 }, tr('izquierdo')), h('option', { value: '2', selected: p.v === 2 }, tr('derecho')));
        else if (p.op === 'repeat') field = h('input', { class: 'num', type: 'number', min: 1, max: 500, 'data-f': 'v', value: parseInt(p.v) || 1 });
        else field = h('input', { class: 'campo', 'data-f': 'v', placeholder: tr('nota'), value: p.v || '' });

        field.querySelectorAll ? field.querySelectorAll('[data-f]').forEach(inp => inp.oninput = () => { steps[i][inp.dataset.f] = inp.value; }) : null;
        if (field.dataset && field.dataset.f) field.oninput = () => { steps[i][field.dataset.f] = field.value; };

        const up = h('button', { class: 'mv', onclick: () => move(i, -1) }, '↑');
        const dn = h('button', { class: 'mv', onclick: () => move(i, 1) }, '↓');
        const rm = h('button', { class: 'rm', onclick: () => { syncFromDom(); steps.splice(i, 1); paintSteps(); } }, '✕');
        return h('div', { class: 'ptstep', 'data-i': i }, opSel, field, up, dn, rm);
      }).filter(Boolean));
    }

    function move(i, d) {
      syncFromDom();
      const j = i + d;
      if (j < 0 || j >= steps.length) return;
      const t = steps[i]; steps[i] = steps[j]; steps[j] = t;
      paintSteps();
    }

    function addStep() {
      syncFromDom();
      const op = typeSel.value;
      const st = { op };
      if (op === 'click') st.v = 1;
      if (op === 'delay') st.v = 500;
      if (op === 'repeat') st.v = 2;
      if (op === 'mouse') { st.v = 0; st.v2 = 0; }
      steps.push(st);
      paintSteps();
    }

    /* ---------- the two views ---------- */
    function toText() {
      if (textView) return;
      syncFromDom();
      textEl.value = serialize(steps);
      textView = true;
      builderBox.style.display = 'none'; textBox.style.display = '';
      tabBuilder.classList.remove('on'); tabText.classList.add('on');
    }
    function toBuilder() {
      if (!textView && stepsEl.childElementCount) { return; }
      if (textView) steps = parseText(textEl.value);
      textView = false;
      textBox.style.display = 'none'; builderBox.style.display = '';
      tabText.classList.remove('on'); tabBuilder.classList.add('on');
      paintSteps();
    }

    /* ---------- open / new / save / delete ---------- */
    function newScript() {
      steps = [{ op: 'string', v: '' }];
      original = null; nameInput.value = ''; status('');
      textView = false; builderBox.style.display = ''; textBox.style.display = 'none';
      tabText.classList.remove('on'); tabBuilder.classList.add('on');
      paintSteps(); showEdit();
    }

    async function open(fileName) {
      try {
        const txt = await P.fsText(DIR + '/' + fileName);
        steps = parseText(txt || '');
        original = fileName;
        nameInput.value = fileName.replace(/\.pato$/i, '');
        status('');
        textView = false; builderBox.style.display = ''; textBox.style.display = 'none';
        tabText.classList.remove('on'); tabBuilder.classList.add('on');
        paintSteps(); showEdit();
      } catch (e) { P.toast(tr('No se pudo leer el script: %s', e.message), true); }
    }

    async function save() {
      if (textView) steps = parseText(textEl.value); else syncFromDom();
      const name = cleanName(nameInput.value);
      if (!name) { status(tr('Ponele un nombre al script'), 'bad'); return; }
      const fileName = name + '.pato';
      if ((!original || original.toLowerCase() !== fileName.toLowerCase()) &&
          files.some(f => f.name.toLowerCase() === fileName.toLowerCase())) {
        status(tr('Ya hay un script con ese nombre'), 'bad'); return;
      }
      status(tr('Guardando…'));
      try {
        const body = serialize(steps).replace(/\n+$/, '') + '\n';
        await P.fsPut(DIR + '/' + fileName, body);
        if (original && original.toLowerCase() !== fileName.toLowerCase()) {
          await P.fsDelete(DIR + '/' + original);
        }
        original = fileName;
        status(tr('Guardado · %d pasos', countSteps(steps)), 'ok');
        files = (await P.fsList(DIR)).filter(f => !f.dir && /\.pato$/i.test(f.name));
      } catch (e) { status(tr('No se pudo guardar: %s', e.message), 'bad'); }
    }

    async function removeFile(fileName) {
      if (!confirm(tr('¿Borrar este script de la placa?'))) return;
      try { await P.fsDelete(DIR + '/' + fileName); P.toast(tr('Borrado')); loadList(); }
      catch (e) { P.toast(e.message, true); }
    }
    async function deleteCurrent() {
      if (!original) { showList(); return; }
      if (!confirm(tr('¿Borrar este script de la placa?'))) return;
      try { await P.fsDelete(DIR + '/' + original); P.toast(tr('Borrado')); showList(); }
      catch (e) { P.toast(e.message, true); }
    }

    /* ---------- go ---------- */
    P.fsMkdir(DIR).catch(() => {});
    showList();
  },
});
