/* Impresoras 3D's page in the board's portal (apps/impresoras,
 * docs/PORTAL-PAGES.md).
 *
 * Two halves:
 *   - the machines now: what the app on the board publishes every second
 *     through the live channel (/api/live, key "status"), with pause,
 *     resume, cancel, preheat and G-code sent back the same way ("pause 0",
 *     "temp 0 2 60", "gcode 0 G28"; impresoras.c, live_take). The camera is
 *     shown straight from the printer by the browser, which is on the same
 *     LAN: the board does not relay it.
 *   - the list itself: /data/impresoras.txt (apps/impresoras/main/p3d_cfg.c),
 *     edited by fields and written back whole; the app sees the file's date
 *     change within two seconds and reconnects what changed.
 * The keys typed here are kept on the card in the clear, as the board says
 * too. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const { h, put, toast, fsText, fsPut, fsMkdir, openApp } = P;

const APP = 'aos.impresoras';
const FILE = '/data/impresoras.txt';

const KINDS = [
  { id: 'octoprint', label: 'OctoPrint', port: 80, key: N_('Clave de la API'), need: ['key'], tls: true,
    hint: N_('En OctoPrint: Ajustes, API (o la clave de una aplicación del usuario).') },
  { id: 'prusalink', label: 'Prusa (PrusaLink)', port: 80, key: N_('Contraseña de PrusaLink'), user: true, need: ['key'],
    hint: N_('MK4, MK3.9, MINI, XL, CORE One, MK3S con PrusaLink: las de Prusa Connect, por la red de la casa. En la impresora: Ajustes, Red, PrusaLink (usuario maker).') },
  { id: 'bambu', label: 'Bambu Lab', port: 8883, key: N_('Código de acceso'), serial: true, need: ['key', 'serial'],
    hint: N_('Modo LAN, y el modo desarrollador para manejarla. El código de acceso está en Ajustes, Red; el número de serie en Ajustes, Dispositivo.') },
  { id: 'moonraker', label: 'Klipper (Moonraker)', port: 7125, key: N_('Clave de la API (opcional)'), tls: true,
    hint: N_('Voron, Sovol, Elegoo Neptune 4, Qidi, RatRig: toda la que tenga Mainsail o Fluidd. Puerto 7125, o 80 si va por nginx.') },
  { id: 'duet', label: 'Duet (RepRapFirmware)', port: 80, key: N_('Contraseña (si tiene)'),
    hint: N_('Duet 2 y 3 con RepRapFirmware 3, sola o con SBC.') },
  { id: 'elegoo', label: 'Elegoo (SDCP)', port: 3030, serial: true, serialLabel: N_('MainboardID (opcional)'),
    hint: N_('Centauri Carbon y las de resina con Wi-Fi.') },
  { id: 'flashforge', label: 'FlashForge', port: 8898, key: N_('Código de verificación (check code)'), serial: true,
    hint: N_('Adventurer 5M, 5M Pro, AD5X: número de serie y código de Ajustes, Red, Sólo LAN. Sin código: las viejas, por el puerto 8899.') },
  { id: 'creality', label: 'Creality', port: 9999,
    hint: N_('K1, K1C, K1 Max, K2 Plus, Ender-3 V3, Hi, sin root. Con Moonraker, mejor como Klipper.') },
];
const KIND = Object.fromEntries(KINDS.map(k => [k.id, k]));

const STATES = [N_('Sin conexión'), N_('Conectando'), N_('Lista'), N_('Preparando'), N_('Imprimiendo'), N_('En pausa'),
  N_('Terminó'), N_('Cancelada'), N_('Error')];
const STATE_COLOR = ['var(--dim)', 'var(--dim)', 'var(--dim)', 'var(--accent)', 'var(--accent)', 'var(--orange)',
  'var(--green)', 'var(--orange)', 'var(--red)'];
const MATERIALS = [['PLA', 215, 60], ['PETG', 240, 80], ['ABS', 255, 100], ['ASA', 260, 105], ['TPU', 225, 50]];
const CAP = { PAUSE: 1, CANCEL: 2, TEMP: 4, GCODE: 1 << 10, LIGHT: 1 << 12 };

const CSS = `
.p3grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:12px}
.p3card{background:var(--card);border-radius:14px;padding:14px;display:flex;flex-direction:column;gap:8px;min-width:0}
.p3head{display:flex;align-items:baseline;gap:8px}
.p3name{font-weight:600;font-size:1.05em;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.p3state{font-size:.9em;white-space:nowrap}
.p3sub{color:var(--dim);font-size:.85em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.p3temps{display:flex;gap:14px;flex-wrap:wrap;font-variant-numeric:tabular-nums}
.p3bar{height:8px;border-radius:4px;background:var(--card2);overflow:hidden}
.p3bar>div{height:100%;border-radius:4px}
.p3job{display:flex;justify-content:space-between;gap:8px;font-size:.85em;color:var(--dim)}
.p3job span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.p3cam{width:100%;border-radius:10px;background:#000;aspect-ratio:4/3;object-fit:contain}
.p3btns{display:flex;flex-wrap:wrap;gap:6px}
.p3btns .btn{padding:6px 10px}
.p3g{display:flex;gap:6px}.p3g input{flex:1;min-width:0;font-family:monospace}
.p3ed{background:var(--card);border-radius:14px;padding:14px;margin-bottom:10px}
.p3ed .p3f{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:8px 12px;margin-top:8px}
.p3ed label{display:flex;flex-direction:column;font-size:.85em;color:var(--dim);gap:3px}
.p3ed input,.p3ed select{font-size:1rem}
.p3ed .p3chk{flex-direction:row;align-items:center;gap:8px}
`;

function style() {
  if (document.getElementById('css-impresoras')) return;
  document.head.appendChild(h('style', { id: 'css-impresoras' }, CSS));
}

/* ---- impresoras.txt -------------------------------------------------------------- */

function parse(text) {
  const out = [];
  let cur = null;
  for (const raw of (text || '').split('\n')) {
    const s = raw.trim();
    if (!s || s[0] === '#' || s[0] === ';') continue;
    if (s[0] === '[') {
      cur = { name: s.slice(1, s.indexOf(']') > 0 ? s.indexOf(']') : undefined).trim(), type: 'octoprint', host: '',
        port: '', tls: false, key: '', user: '', serial: '', camera: '' };
      out.push(cur);
      continue;
    }
    const eq = s.indexOf('=');
    if (eq < 0 || !cur) continue;
    const k = s.slice(0, eq).trim(), v = s.slice(eq + 1).trim();
    if (k === 'tls') cur.tls = v !== '0';
    else if (k in cur) cur[k] = v;
  }
  return out;
}

function serialize(list) {
  let t = '# 3D printers for P4OS (apps/impresoras). One block each:\n' +
    '# [Name]  type (octoprint prusalink bambu moonraker duet elegoo flashforge creality),\n' +
    '# host, port, tls (1: https), key (API key, access code, password or check code),\n' +
    '# user (PrusaLink), serial (Bambu, FlashForge, Elegoo), camera (off or a URL).\n' +
    '# The keys are kept here in the clear.\n';
  for (const m of list) {
    t += `\n[${m.name}]\ntype = ${m.type}\nhost = ${m.host}\n`;
    if (m.port) t += `port = ${m.port}\n`;
    if (m.tls) t += 'tls = 1\n';
    for (const k of ['key', 'user', 'serial', 'camera']) if (m[k]) t += `${k} = ${m[k]}\n`;
  }
  return t;
}

function check(list) {
  const seen = new Set();
  for (const [i, m] of list.entries()) {
    const n = m.name || tr('impresora %d', i + 1);
    if (!m.name || m.name.length > 47 || /[\[\]]/.test(m.name)) return tr('%s: el nombre, de 1 a 47 caracteres y sin corchetes', n);
    if (seen.has(m.name.toLowerCase())) return tr('%s: hay dos con ese nombre', m.name);
    seen.add(m.name.toLowerCase());
    if (!m.host || /\s/.test(m.host)) return tr('%s: falta la dirección (IP o nombre, sin espacios)', m.name);
    if (m.port && !(/^\d+$/.test(m.port) && +m.port > 0 && +m.port < 65536)) return tr('%s: el puerto va de 1 a 65535', m.name);
    const k = KIND[m.type];
    for (const f of (k && k.need) || []) if (!m[f]) return tr('%s: falta %s', m.name, f === 'key' ? tr(k.key) : tr('el número de serie'));
  }
  return null;
}

/* ---- the live half ------------------------------------------------------------------ */

function dur(s) {
  if (s == null || s < 0) return '--';
  const hh = Math.floor(s / 3600), mm = Math.floor(s % 3600 / 60);
  if (hh) return tr('%d h %s min', hh, String(mm).padStart(2, '0'));
  if (mm) return tr('%d min', mm);
  return tr('%d s', s);
}

function eta(s) {
  if (s == null || s <= 0) return '';
  const d = new Date(Date.now() + s * 1000);
  return d.toLocaleTimeString(P.lang ? P.lang() : 'es', { hour: '2-digit', minute: '2-digit' });
}

function temp(t) {
  if (!t || !t[2]) return '--';
  return t[1] > 0.5 ? `${Math.round(t[0])} / ${Math.round(t[1])}°` : `${Math.round(t[0])}°`;
}

async function send(line) {
  const r = await fetch('/api/live?app=' + APP, { method: 'POST', body: line });
  if (!r.ok) throw new Error(tr('La placa no tomó la orden (%d)', r.status));
}

/* what a card is made of: when this changes it is built again, otherwise
 * only its texts are updated (a redraw every poll would eat clicks and what
 * is being typed in its G-code field) */
function shape(m, camOn) {
  const active = m.state === 3 || m.state === 4 || m.state === 5;
  return [m.state, m.caps, m.light, camOn, m.cam, active || m.state === 6, !!(m.tool1 && m.tool1[2]),
    !!(m.chamber && m.chamber[2])].join('|');
}

function liveCard(m, camOn, toggleCam) {
  const st = m.state;
  const active = st === 3 || st === 4 || st === 5;
  const job = active || st === 6;
  const go = (line, ok) => send(line.replace('#', m.i)).then(() => ok && toast(ok)).catch(e => toast(e.message, true));
  const btns = [];
  if (active && (m.caps & CAP.PAUSE) && st !== 3) {
    btns.push(st === 5
      ? h('button', { class: 'btn pri', onclick: () => go('resume #', tr('Reanudando')) }, tr('Reanudar'))
      : h('button', { class: 'btn', onclick: () => go('pause #', tr('Pausando')) }, tr('Pausar')));
  }
  if (active && (m.caps & CAP.CANCEL)) {
    btns.push(h('button', { class: 'btn red', onclick: () => {
      if (confirm(tr('¿Cancelar la impresión de %s?', m.name))) go('cancel #', tr('Cancelando'));
    } }, tr('Cancelar')));
  }
  if (m.caps & CAP.TEMP) {
    for (const [name, t, b] of MATERIALS.slice(0, 3)) {
      btns.push(h('button', { class: 'btn', onclick: () => go(`temp # 0 ${t}`).then(() => go(`temp # 2 ${b}`, tr('Precalentando para %s', name))) }, name));
    }
    btns.push(h('button', { class: 'btn', onclick: () => go('temp # 0 0').then(() => go('temp # 2 0', tr('Enfriando'))) }, tr('Enfriar')));
  }
  if ((m.caps & CAP.LIGHT) && m.light >= 0) {
    btns.push(h('button', { class: 'btn', onclick: () => go(`light # ${m.light ? 0 : 1}`) }, m.light ? tr('Apagar la luz') : tr('Prender la luz')));
  }
  if (m.cam) btns.push(h('button', { class: 'btn', onclick: toggleCam }, camOn ? tr('Ocultar cámara') : tr('Ver cámara')));
  const gin = h('input', { placeholder: 'G28', onkeydown: e => { if (e.key === 'Enter') gsend(); } });
  const gsend = () => { const v = gin.value.trim(); if (v) go('gcode # ' + v, tr('Enviado: %s', v)); gin.value = ''; };
  let img = null, snap = 0;
  if (camOn) {
    /* a stream is shown as it comes; a snapshot is asked again each second */
    const stream = /action=stream|\/video|mjpe?g/i.test(m.cam);
    const url = () => m.cam + (m.cam.includes('?') ? '&' : '?') + '_=' + Date.now();
    img = h('img', { class: 'p3cam', alt: '', src: stream ? m.cam : url() });
    if (!stream) {
      img.onload = () => { snap = setTimeout(() => { if (img.isConnected) img.src = url(); }, 1000); };
      img.onerror = () => { snap = setTimeout(() => { if (img.isConnected) img.src = url(); }, 3000); };
    }
  }
  const r = {
    state: h('div', { class: 'p3state' }),
    sub: h('div', { class: 'p3sub' }),
    temps: h('div', { class: 'p3temps' }),
    bar: job ? h('div', {}) : null,
    file: h('span', {}),
    left: h('span', {}),
  };
  const el = h('div', { class: 'p3card' },
    h('div', { class: 'p3head' }, h('div', { class: 'p3name' }, m.name), r.state),
    r.sub, r.temps,
    r.bar ? h('div', { class: 'p3bar' }, r.bar) : null,
    job ? h('div', { class: 'p3job' }, r.file, r.left) : null,
    img,
    btns.length ? h('div', { class: 'p3btns' }, btns) : null,
    (m.caps & CAP.GCODE) ? h('div', { class: 'p3g' }, gin, h('button', { class: 'btn', onclick: gsend }, tr('Enviar'))) : null);
  el._r = r;
  el._shape = shape(m, camOn);
  el._stop = () => clearTimeout(snap);
  fill(el, m);
  return el;
}

function fill(el, m) {
  const r = el._r, st = m.state;
  const active = st === 3 || st === 4 || st === 5;
  const k = KIND[m.type] || { label: m.type };
  r.state.textContent = tr(STATES[st] || STATES[0]) + (st === 4 && m.progress >= 0 ? ` ${Math.floor(m.progress)} %` : '');
  r.state.style.color = STATE_COLOR[st] || 'var(--dim)';
  r.sub.textContent = (st === 0 || st === 8) && m.msg ? m.msg : [k.label, m.model].filter(Boolean).join(' · ');
  r.sub.style.color = st === 8 ? 'var(--red)' : st === 0 && m.msg ? 'var(--orange)' : '';
  const temps = [['🔥', m.tool], ['▭', m.bed]];
  if (m.tool1 && m.tool1[2]) temps.splice(1, 0, ['🔥2', m.tool1]);
  if (m.chamber && m.chamber[2]) temps.push(['◫', m.chamber]);
  r.temps.textContent = temps.map(([g, t]) => `${g} ${temp(t)}`).join('   ');
  if (r.bar) {
    r.bar.style.width = (m.progress >= 0 ? Math.min(100, m.progress) : 0) + '%';
    r.bar.style.background = STATE_COLOR[st];
  }
  r.file.textContent = m.file || '';
  let left = '';
  if (active && m.remaining >= 0) left = eta(m.remaining) ? tr('faltan %s · %s', dur(m.remaining), eta(m.remaining)) : tr('faltan %s', dur(m.remaining));
  else if (active && m.layers > 0) left = tr('capa %d de %d', m.layer, m.layers);
  r.left.textContent = left;
}

/* ---- the editor half -------------------------------------------------------------------- */

function editor(list, onChange) {
  const box = h('div', {});
  const draw = () => {
    put(box, list.map((m, i) => {
      const k = KIND[m.type] || KINDS[0];
      const field = (key, label, attrs = {}) => h('label', {}, tr(label),
        h('input', { value: m[key] || '', ...attrs, oninput: e => { m[key] = e.target.value.trim(); onChange(); } }));
      const sel = h('select', { onchange: e => { m.type = e.target.value; m.port = ''; onChange(); draw(); } },
        KINDS.map(x => h('option', { value: x.id, selected: x.id === m.type ? '' : null }, x.label)));
      return h('div', { class: 'p3ed' },
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, m.name || tr('Sin nombre'))),
          h('button', { class: 'btn red', onclick: () => { if (confirm(tr('¿Sacar %s de la lista?', m.name || tr('esta impresora')))) { list.splice(i, 1); onChange(); draw(); } } }, tr('Borrar'))),
        h('p', { class: 'note small' }, tr(k.hint)),
        h('div', { class: 'p3f' },
          h('label', {}, tr('Qué es'), sel),
          field('name', N_('Nombre'), { maxlength: 47 }),
          field('host', N_('Dirección (IP o nombre)'), { placeholder: '192.168.0.50' }),
          field('port', N_('Puerto (vacío: el de siempre)'), { placeholder: String(k.port), inputmode: 'numeric' }),
          k.user ? field('user', N_('Usuario'), { placeholder: 'maker' }) : null,
          k.key ? field('key', k.key, { type: 'password', autocomplete: 'off' }) : null,
          k.serial ? field('serial', k.serialLabel || N_('Número de serie')) : null,
          field('camera', N_('Cámara (vacío: la que diga la impresora; off: ninguna)'), { placeholder: 'http://…/?action=snapshot' }),
          k.tls ? h('label', { class: 'p3chk' }, h('input', { type: 'checkbox', checked: m.tls ? '' : null,
            onchange: e => { m.tls = e.target.checked; onChange(); } }), tr('HTTPS (con certificado válido)')) : null));
    }));
  };
  draw();
  return { box, draw };
}

/* ---- the page ---------------------------------------------------------------------------- */

P.registerPage({
  id: 'impresoras',
  name: tr('Impresoras 3D'),
  icon: '⎙',
  render(main) {
    style();
    let list = [];
    let dirty = false;
    let timer = 0, gone = false;
    const cams = new Set();
    let last = null;
    const grid = h('div', { class: 'p3grid' });
    const status = h('p', { class: 'note' }, tr('Leyendo...'));
    const saveBtn = h('button', { class: 'btn pri', disabled: '' }, tr('Guardar en la tarjeta'));
    const ed = editor(list, () => { dirty = true; saveBtn.removeAttribute('disabled'); });

    const cards = new Map();
    const draw = () => {
      if (!last) return;
      const seen = new Set();
      const order = last.machines.map(m => {
        seen.add(m.i);
        const camOn = cams.has(m.i);
        let el = cards.get(m.i);
        if (el && el._shape === shape(m, camOn) && el._name === m.name) {
          fill(el, m);
        } else {
          if (el) el._stop();
          el = liveCard(m, camOn, () => { cams.has(m.i) ? cams.delete(m.i) : cams.add(m.i); draw(); });
          el._name = m.name;
          cards.set(m.i, el);
        }
        return el;
      });
      for (const [i, el] of cards) if (!seen.has(i)) { el._stop(); cards.delete(i); }
      order.forEach((el, n) => { if (grid.children[n] !== el) grid.insertBefore(el, grid.children[n] || null); });
      while (grid.children.length > order.length) grid.lastChild.remove();
    };
    const poll = async () => {
      if (gone) return;
      try {
        const r = await fetch(`/api/live?app=${APP}&key=status`, { cache: 'no-store' });
        if (r.status === 404) {
          last = null;
          put(status, tr('La app Impresoras 3D no está abierta en la placa. '),
            h('button', { class: 'btn', onclick: () => openApp(APP).catch(e => toast(e.message, true)) }, tr('Abrirla')));
          for (const el of cards.values()) el._stop();
          cards.clear();
          put(grid);
        } else if (r.ok) {
          last = await r.json();
          put(status, last.machines.length ? '' : tr('Ninguna impresora todavía: agregalas abajo.'));
          draw();
        }
      } catch (e) {
        put(status, tr('No se puede leer el estado: %s', e.message));
      }
      timer = setTimeout(poll, 1500);
    };

    saveBtn.onclick = async () => {
      const bad = check(list);
      if (bad) return toast(bad, true);
      try {
        await fsMkdir('/data').catch(() => {});
        await fsPut(FILE, serialize(list));
        send('reload').catch(() => {});
        dirty = false;
        saveBtn.setAttribute('disabled', '');
        toast(tr('Guardado: la placa se reconecta sola'));
      } catch (e) {
        toast(tr('No se pudo guardar: %s', e.message), true);
      }
    };
    const addBtn = h('button', { class: 'btn', onclick: () => {
      list.push({ name: '', type: 'octoprint', host: '', port: '', tls: false, key: '', user: '', serial: '', camera: '' });
      dirty = true;
      saveBtn.removeAttribute('disabled');
      ed.draw();
    } }, tr('+ Agregar una impresora'));

    put(main,
      h('h1', {}, tr('Impresoras 3D')),
      status, grid,
      h('h2', { style: 'margin-top:24px' }, tr('Las impresoras')),
      h('p', { class: 'note' }, tr('OctoPrint, PrusaLink (las de Prusa Connect), Bambu Lab en modo LAN, Klipper, Duet, Elegoo, FlashForge y Creality. Las claves quedan guardadas en la tarjeta (%s) y se pueden leer.', FILE)),
      ed.box,
      h('div', { class: 'btns' }, addBtn, saveBtn));

    fsText(FILE).then(t => {
      list.splice(0, list.length, ...parse(t));
      ed.draw();
    }).catch(e => toast(e.message, true));
    poll();
    const leave = e => { if (dirty) { e.preventDefault(); e.returnValue = ''; } };
    window.addEventListener('beforeunload', leave);
    return () => {
      gone = true;
      clearTimeout(timer);
      for (const el of cards.values()) el._stop();
      window.removeEventListener('beforeunload', leave);
    };
  },
});
