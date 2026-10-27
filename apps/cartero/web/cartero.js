/* Cartero's page in the board's portal (apps/cartero, docs/PORTAL-PAGES.md):
 * an API client in the manner of Postman, with the board as its agent.
 *
 * Everything lives on the card, in Postman's own formats (apps/cartero/README.md
 * is the contract): the collections in /data/cartero/colecciones, the
 * environments in /data/cartero/entornos, the globals, the active environment
 * (estado.json, shared with the board), the history and the runner's results.
 * Both the board and this page read a file again before writing it and change
 * only what they edited, so neither undoes the other.
 *
 * A request is prepared here whole, the way Postman does it: the collection's,
 * the folders' and the request's pre-request scripts (the browser's own
 * JavaScript, with the same `pm` the board's interpreter offers), the
 * variables, the inherited auth, the body. Then it goes either straight from
 * the browser (fetch: CORS permitting) or through the board: the final request
 * is written to /data/cartero/web/pedido.json, "send <n>" is posted on the
 * live channel and the app's answer comes back as the live key "respuesta".
 * The tests run here afterwards, and the variables they change are merged
 * back into their files.
 *
 * Sections: styles, helpers, the Postman model, the card, variables, the
 * script runtime, building and sending a request, cURL, JSON and XML, the
 * widgets, the sidebar, the editors, the response, the runner, import and
 * export, the page. */
const P = window.P4OS.forPage ? window.P4OS.forPage('cartero') : window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const { h, put, toast } = P;

const APP = 'aos.cartero';
const DIR = '/data/cartero';
const D_COLS = DIR + '/colecciones', D_ENVS = DIR + '/entornos', D_RUNS = DIR + '/corridas';
const D_RESP = DIR + '/respuestas', D_WEB = DIR + '/web';
const F_GLOB = DIR + '/globales.json', F_STATE = DIR + '/estado.json', F_HIST = DIR + '/historial.json';
const F_ASK = D_WEB + '/pedido.json', F_BIN = D_WEB + '/respuesta.bin';
const SCHEMA = 'https://schema.getpostman.com/json/collection/v2.1.0/collection.json';
const METHODS = ['GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD', 'OPTIONS'];
const SHORT = { DELETE: 'DEL', OPTIONS: 'OPT', PATCH: 'PATCH' };
const HIST_MAX = 50;
const LIVE = '/api/live?app=' + APP;

const CSS = `
.ct{container-type:inline-size}
.ct .mono,.ct textarea.ctcode,.ct .ctmono{font-family:ui-monospace,"JetBrains Mono",Menlo,monospace}
.cttop{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:14px}
.cttop h1{margin:0;flex:1;min-width:120px}
.cttop select{width:auto;max-width:220px}
.ctseg{display:inline-flex;background:var(--card2);border-radius:999px;padding:3px;gap:2px}
.ctseg button{border:0;background:transparent;padding:6px 12px;border-radius:999px;cursor:pointer;font-size:13px;white-space:nowrap}
.ctseg button.on{background:var(--accent);color:#fff}
.ctlay{display:grid;grid-template-columns:300px minmax(0,1fr);gap:14px;align-items:start}
.ctside{position:sticky;top:12px;max-height:calc(100vh - 24px);display:flex;flex-direction:column;min-width:0}
.ctsidehead{display:flex;align-items:center;gap:4px;padding:8px 8px 0}
.ctsidehead .cttabs{flex:1;flex-wrap:nowrap;overflow-x:auto;scrollbar-width:none;min-width:0}
.ctsidehead .cttabs::-webkit-scrollbar{display:none}
.ctsidehead .cttabs button{padding:8px 6px;font-size:13px}
.ctsidetoggle{display:none}
.ctsidebody{overflow:auto;padding:6px 6px 10px;min-height:120px}
.ctsidebar{display:flex;gap:6px;padding:4px 4px 8px;flex-wrap:wrap;align-items:center}
.ctsidebar input{flex:1;min-width:90px;padding:6px 10px;font-size:13px}
.cttabs{display:flex;gap:2px;flex-wrap:wrap;border-bottom:1px solid var(--line);margin-bottom:8px}
.cttabs button{border:0;background:transparent;padding:8px 9px;cursor:pointer;font-size:13.5px;color:var(--dim);border-bottom:2px solid transparent;margin-bottom:-1px;white-space:nowrap}
.cttabs button.on{color:var(--text);border-bottom-color:var(--accent)}
.cttabs button .ctcnt{font-size:11px;color:var(--dim);margin-left:4px}
.cttabs button .ctdot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--green);margin-left:5px;vertical-align:middle}
.ctnode{display:flex;align-items:center;gap:6px;padding:5px 6px;border-radius:8px;cursor:pointer;font-size:14px;min-width:0;user-select:none}
.ctnode:hover{background:var(--card2)}
.ctnode.sel{background:color-mix(in srgb,var(--accent) 22%,transparent)}
.ctnode .ctcar{width:14px;flex:none;color:var(--dim);font-size:11px;text-align:center}
.ctnode .ctnm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ctnode .ctmore{flex:none;border:0;background:transparent;color:var(--dim);cursor:pointer;padding:0 6px;border-radius:6px;font-size:16px;line-height:20px;opacity:.6}
.ctnode:hover .ctmore,.ctnode .ctmore:focus{opacity:1}
.ctnode .ctmore:hover{background:var(--line)}
.ctnode .ctsub{color:var(--dim);font-size:12px;flex:none}
.ctm{font:700 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.2px;flex:none;min-width:38px;text-align:right}
.ctm-GET{color:color-mix(in srgb,var(--green) 78%,var(--text))}
.ctm-POST{color:color-mix(in srgb,var(--orange) 85%,var(--text))}
.ctm-PUT{color:var(--accent)}
.ctm-PATCH{color:color-mix(in srgb,#bf5af2 85%,var(--text))}
.ctm-DELETE{color:var(--red)}
.ctm-HEAD,.ctm-OPTIONS{color:var(--dim)}
.ctmenu{position:fixed;z-index:50;background:var(--card2);border-radius:12px;box-shadow:0 10px 30px rgba(0,0,0,.35);padding:5px;min-width:190px;border:1px solid var(--line)}
.ctmenu button{display:block;width:100%;text-align:left;border:0;background:transparent;padding:8px 12px;border-radius:8px;cursor:pointer;font-size:14px}
.ctmenu button:hover{background:var(--line)}
.ctmenu button.bad{color:var(--red)}
.ctmenu hr{border:0;border-top:1px solid var(--line);margin:4px 0}
.ctcard{background:var(--card);border-radius:var(--radius);padding:14px 16px;margin-bottom:14px;min-width:0}
.cthead{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-bottom:10px}
.cthead .ctcrumb{color:var(--dim);font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:40%}
.cthead input.ctname{flex:1;min-width:140px;background:transparent;border:1px solid transparent;font-size:18px;font-weight:650;padding:4px 8px}
.cthead input.ctname:hover{border-color:var(--line)}
.ctdirty{width:9px;height:9px;border-radius:50%;background:var(--orange);flex:none;visibility:hidden}
.ctdirty.on{visibility:visible}
.ctline{display:flex;gap:8px;align-items:stretch;flex-wrap:wrap}
.ctline select.ctmeth{width:auto;font-weight:700;font-family:ui-monospace,Menlo,monospace;font-size:14px}
.cturl{position:relative;flex:1;min-width:200px}
.cturl input[type=text],.cturlback{font:14px/20px ui-monospace,"JetBrains Mono",Menlo,monospace;letter-spacing:0;padding:9px 12px;border-radius:10px;border:1px solid transparent;white-space:pre}
.cturl input[type=text]{position:relative;z-index:1;background:transparent;color:transparent;caret-color:var(--text);width:100%}
.cturl input[type=text]::selection{color:transparent;background:color-mix(in srgb,var(--accent) 35%,transparent)}
.cturl input[type=text]::placeholder{color:var(--dim)}
.cturlback{position:absolute;inset:0;overflow:hidden;background:var(--card2);color:var(--text);pointer-events:none}
.ctv-ok{color:var(--accent);font-weight:600}
.ctv-dyn{color:color-mix(in srgb,#bf5af2 85%,var(--text));font-weight:600}
.ctv-bad{color:var(--orange);font-weight:600}
.ctprev{font-size:12.5px;color:var(--dim);margin:6px 2px 0;word-break:break-all}
.ctprev span{cursor:help}
.ctbody{margin-top:12px}
.ctkv{display:grid;grid-template-columns:22px minmax(0,1fr) minmax(0,1.4fr) auto;gap:4px 6px;align-items:center}
.ctkv.desc{grid-template-columns:22px minmax(0,1fr) minmax(0,1.4fr) minmax(0,.8fr) auto}
.ctkv .ctkvh{font-size:12px;color:var(--dim);padding:0 4px}
.ctkv input[type=text],.ctkv input[type=password]{padding:6px 9px;font-size:13.5px;font-family:ui-monospace,Menlo,monospace}
.ctkv input[type=checkbox]{width:16px;height:16px;accent-color:var(--accent);margin:0 auto;display:block}
.ctkv .ctkva{display:flex;gap:2px}
.ctkv .ctkva button{border:0;background:transparent;color:var(--dim);cursor:pointer;padding:3px 6px;border-radius:6px;font-size:13px}
.ctkv .ctkva button:hover{background:var(--card2);color:var(--text)}
.ctkv .ctkva button.on{color:var(--accent)}
.ctkv .off{opacity:.5}
.ctkv .ctghost input{opacity:.7}
.ctauto{margin-top:12px;font-size:12.5px;color:var(--dim)}
.ctauto .mono{font-size:12.5px}
.ctauto div{padding:2px 0}
.ctrow{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:6px 0}
.ctrow label{display:flex;align-items:center;gap:6px;font-size:13.5px;cursor:pointer}
.ctrow select{width:auto}
.ctfield{display:grid;grid-template-columns:140px minmax(0,1fr);gap:8px 10px;align-items:center;margin:8px 0;max-width:640px}
.ctfield>label{font-size:13px;color:var(--dim)}
.ctfield .ctpw{display:flex;gap:6px}
textarea.ctcode{width:100%;min-height:180px;font-size:13.5px;line-height:1.5;background:var(--card2);border:1px solid transparent;border-radius:10px;padding:10px 12px;resize:vertical;tab-size:2;white-space:pre;overflow-wrap:normal;overflow-x:auto}
.ctsnips{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0}
.ctsnips .btn{padding:5px 10px;font-size:12.5px}
.ctjs{font-size:12.5px;margin-left:auto}
.ctsend{min-width:96px}
.ctres{min-height:120px}
.ctmeta{display:flex;align-items:center;gap:12px;flex-wrap:wrap;font-size:13.5px;margin-bottom:8px}
.ctmeta b{font-variant-numeric:tabular-nums}
.ctst{display:inline-block;padding:3px 10px;border-radius:999px;font-weight:700;font-size:13px;color:#fff;background:var(--dim)}
.ctst.s2{background:color-mix(in srgb,var(--green) 85%,#000)}
.ctst.s3{background:var(--accent)}
.ctst.s4{background:color-mix(in srgb,var(--orange) 90%,#000)}
.ctst.s5,.ctst.s0{background:var(--red)}
.cttime{display:flex;height:8px;width:160px;border-radius:4px;overflow:hidden;background:var(--card2);cursor:help}
.cttime>div{height:100%}
.ctlegend{display:flex;gap:10px;flex-wrap:wrap;font-size:12px;color:var(--dim)}
.ctlegend i{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:4px;vertical-align:-1px}
.ctacts{display:flex;gap:6px;flex-wrap:wrap;margin-left:auto}
.ctacts .btn,.ctsmall{padding:5px 11px;font-size:12.5px}
.ctpre{background:var(--card2);border-radius:10px;padding:10px 12px;font:12.5px/1.5 ui-monospace,Menlo,monospace;white-space:pre-wrap;word-break:break-all;max-height:70vh;overflow:auto;margin:0}
.ctframe{width:100%;height:60vh;border:1px solid var(--line);border-radius:10px;background:#fff}
.ctimg{max-width:100%;max-height:60vh;border-radius:8px;background:repeating-conic-gradient(var(--card2) 0 25%,transparent 0 50%) 0 0/16px 16px}
.ctjt{font:12.5px/1.55 ui-monospace,Menlo,monospace;background:var(--card2);border-radius:10px;padding:8px 10px;max-height:70vh;overflow:auto}
.ctjn{padding-left:16px}
.ctjr{display:flex;align-items:baseline;gap:4px;border-radius:4px;padding:0 3px;white-space:nowrap}
.ctjr:hover{background:color-mix(in srgb,var(--accent) 12%,transparent)}
.ctjr .ctcar{width:12px;flex:none;color:var(--dim);cursor:pointer;font-size:10px;text-align:center}
.ctjk{color:var(--text);opacity:.85}
.ctjv{cursor:copy;white-space:pre-wrap;word-break:break-all}
.ctj-s{color:color-mix(in srgb,var(--green) 70%,var(--text))}
.ctj-n{color:color-mix(in srgb,var(--orange) 80%,var(--text))}
.ctj-b{color:var(--accent)}
.ctj-z{color:var(--dim);font-style:italic}
.ctj-m{color:var(--dim)}
.ctjr .ctjvar{visibility:hidden;border:0;background:var(--card);color:var(--accent);cursor:pointer;font-size:11px;padding:0 6px;border-radius:6px;margin-left:6px}
.ctjr:hover .ctjvar{visibility:visible}
.ctjmore{border:0;background:transparent;color:var(--accent);cursor:pointer;font:inherit;padding:0 3px}
.ctjhit{background:color-mix(in srgb,var(--yellow) 45%,transparent);border-radius:2px}
.cttests{display:flex;flex-direction:column;gap:2px}
.cttest{display:flex;gap:8px;align-items:baseline;padding:4px 2px;font-size:14px;border-top:1px solid var(--line)}
.cttest:first-child{border-top:0}
.cttest .ctok{color:var(--green);font-weight:700;width:16px;flex:none}
.cttest .ctko{color:var(--red);font-weight:700;width:16px;flex:none}
.cttest .ctsk{color:var(--dim);width:16px;flex:none}
.cttest .ctterr{color:var(--red);font-size:12.5px;font-family:ui-monospace,Menlo,monospace;word-break:break-word}
.ctcons{margin-top:10px;font:12.5px/1.5 ui-monospace,Menlo,monospace;background:var(--card2);border-radius:10px;padding:8px 10px;max-height:40vh;overflow:auto}
.ctcons div{white-space:pre-wrap;word-break:break-all}
.ctcons .ctl-warn{color:var(--orange)}
.ctcons .ctl-error{color:var(--red)}
.ctcons .ctwh{color:var(--dim);font-size:11px;margin-right:6px}
.ctscripterr{color:var(--red);font-size:13px;margin:6px 0;font-family:ui-monospace,Menlo,monospace;word-break:break-word}
.cthdrs{display:grid;grid-template-columns:minmax(120px,max-content) minmax(0,1fr);gap:3px 14px;font-size:13px}
.cthdrs div:nth-child(odd){font-weight:600;color:var(--dim);word-break:break-all}
.cthdrs div:nth-child(even){word-break:break-all;font-family:ui-monospace,Menlo,monospace;font-size:12.5px}
.ctnote{padding:10px 12px;border-radius:10px;background:var(--card2);font-size:13.5px;margin:8px 0}
.ctnote.bad{background:color-mix(in srgb,var(--red) 16%,var(--card2));color:var(--text)}
.ctnote.warn{background:color-mix(in srgb,var(--orange) 16%,var(--card2));color:var(--text)}
.ctnote .btn{margin:6px 6px 0 0}
.ctspin{display:inline-block;width:12px;height:12px;border:2px solid var(--line);border-top-color:var(--accent);border-radius:50%;animation:ctspin .8s linear infinite;vertical-align:-1px;margin-right:6px}
@keyframes ctspin{to{transform:rotate(360deg)}}
.ctrun{width:100%;border-collapse:collapse;font-size:13.5px}
.ctrun th{text-align:left;font-size:12px;color:var(--dim);font-weight:600;padding:6px;border-bottom:1px solid var(--line)}
.ctrun td{padding:6px;border-top:1px solid var(--line);vertical-align:top}
.ctrun tr.ctrr{cursor:pointer}
.ctrun tr.ctrr:hover td{background:var(--card2)}
.ctrun td.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.ctrun .ctrdet td{background:var(--card2);font-size:12.5px}
.ctsum{display:flex;gap:18px;flex-wrap:wrap;margin:8px 0}
.ctsum div{display:flex;flex-direction:column}
.ctsum b{font-size:22px;font-variant-numeric:tabular-nums}
.ctsum span{font-size:12px;color:var(--dim)}
.ctcfg{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));gap:10px 14px;align-items:end}
.ctcfg label{display:flex;flex-direction:column;gap:4px;font-size:12.5px;color:var(--dim)}
.ctcfg label.chk{flex-direction:row;align-items:center;gap:8px;color:var(--text);font-size:13.5px}
.ctcfg select,.ctcfg input{color:var(--text)}
.ctcfg input[type=number]{background:var(--card2);border:1px solid transparent;border-radius:10px;padding:9px 12px;width:100%}
.ctlist .ctnode{padding:7px 6px}
.ctlist .ctnm small{color:var(--dim);display:block;font-size:12px}
.ctmodal{position:fixed;inset:0;z-index:60;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;padding:16px}
.ctmodal>div{background:var(--card);border-radius:16px;padding:18px;width:min(460px,100%);max-height:90vh;overflow:auto;box-shadow:0 20px 60px rgba(0,0,0,.4)}
.ctmodal h3{margin:0 0 12px;font-size:18px}
.ctmodal label{display:block;font-size:13px;color:var(--dim);margin:10px 0 4px}
.ctmodal .btns{justify-content:flex-end;margin-top:16px}
.ctempty{color:var(--dim);font-size:13.5px;padding:10px 8px}
.ctwide{width:100%}
@container (max-width:820px){
  .ctlay{grid-template-columns:minmax(0,1fr)}
  .ctside{position:static;max-height:none}
  .ctsidebody{max-height:50vh}
  .ctside.fold .ctsidebody,.ctside.fold .ctsidebar{display:none}
  .ctsidetoggle{display:inline-block}
  .cturl{min-width:100%;order:3}
  .ctline .ctsend{flex:1}
  .ctfield{grid-template-columns:minmax(0,1fr)}
  .ctkv,.ctkv.desc{grid-template-columns:22px minmax(0,1fr) minmax(0,1.2fr) auto}
  .ctkv.desc .ctkvd{display:none}
  .cthead .ctcrumb{max-width:100%}
  .ctrun .hide-n{display:none}
}
`;

function style() {
  if (document.getElementById('css-cartero')) return;
  document.head.appendChild(h('style', { id: 'css-cartero' }, CSS));
}

/* ---- helpers ------------------------------------------------------------------ */

const clone = v => v === undefined ? undefined : JSON.parse(JSON.stringify(v));
const sleep = ms => new Promise(r => setTimeout(r, ms));
const isObj = v => v !== null && typeof v === 'object' && !Array.isArray(v);
const lsGet = (k, d) => { try { const v = localStorage.getItem('cartero.' + k); return v == null ? d : JSON.parse(v); } catch { return d; } };
const lsSet = (k, v) => { try { localStorage.setItem('cartero.' + k, JSON.stringify(v)); } catch {} };
const num = (v, d) => P.num ? P.num(v, d) : v.toFixed(d);
const fmtMs = ms => ms == null ? '--' : ms < 1000 ? Math.round(ms) + ' ms' : num(ms / 1000, ms < 10000 ? 2 : 1) + ' s';
const fmtSize = n => P.fmtBytes ? P.fmtBytes(n || 0) : (n || 0) + ' B';
const errText = e => e == null ? '' : e.name === 'AssertionError' ? e.message
  : e instanceof Error ? (e.name && e.name !== 'Error' ? e.name + ': ' : '') + e.message : String(e);

/* crypto.randomUUID is only there over HTTPS; the board's portal is plain HTTP */
function uuid() {
  const b = new Uint8Array(16);
  crypto.getRandomValues(b);
  b[6] = b[6] & 15 | 64;
  b[8] = b[8] & 63 | 128;
  const x = [...b].map(v => v.toString(16).padStart(2, '0')).join('');
  return `${x.slice(0, 8)}-${x.slice(8, 12)}-${x.slice(12, 16)}-${x.slice(16, 20)}-${x.slice(20)}`;
}

function b64(s) {
  let bin = '';
  for (const c of new TextEncoder().encode(s)) bin += String.fromCharCode(c);
  return btoa(bin);
}

/* yyyymmdd-hhmmss, local time: the runner's file names */
function stamp(d = new Date()) {
  const p = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

function ago(t) {
  if (!t) return '';
  const s = Math.max(0, Date.now() / 1000 - t);
  if (s < 60) return tr('recién');
  if (s < 3600) return tr('hace %d min', Math.floor(s / 60));
  if (s < 86400) return tr('hace %d h', Math.floor(s / 3600));
  return P.fmtDate ? P.fmtDate(t) : new Date(t * 1000).toLocaleDateString();
}

/* the clipboard API also needs HTTPS: the old way works on the board's HTTP */
function copyText(text, what) {
  const done = () => toast(what || tr('Copiado'));
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(done, () => fallback());
    return;
  }
  fallback();
  function fallback() {
    const ta = h('textarea', { style: 'position:fixed;top:-1000px;opacity:0' });
    ta.value = text;
    document.body.append(ta);
    ta.select();
    let ok = false;
    try { ok = document.execCommand('copy'); } catch {}
    ta.remove();
    if (ok) done(); else toast(tr('El navegador no dejó copiar'), true);
  }
}

const REASON = { 100: 'Continue', 101: 'Switching Protocols', 200: 'OK', 201: 'Created', 202: 'Accepted', 203: 'Non-Authoritative Information',
  204: 'No Content', 205: 'Reset Content', 206: 'Partial Content', 301: 'Moved Permanently', 302: 'Found', 303: 'See Other',
  304: 'Not Modified', 307: 'Temporary Redirect', 308: 'Permanent Redirect', 400: 'Bad Request', 401: 'Unauthorized',
  402: 'Payment Required', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed', 406: 'Not Acceptable',
  408: 'Request Timeout', 409: 'Conflict', 410: 'Gone', 411: 'Length Required', 412: 'Precondition Failed',
  413: 'Payload Too Large', 414: 'URI Too Long', 415: 'Unsupported Media Type', 418: "I'm a teapot",
  422: 'Unprocessable Entity', 425: 'Too Early', 428: 'Precondition Required', 429: 'Too Many Requests',
  500: 'Internal Server Error', 501: 'Not Implemented', 502: 'Bad Gateway', 503: 'Service Unavailable',
  504: 'Gateway Timeout', 505: 'HTTP Version Not Supported' };

/* a header of a [[name, value], ...] list, by name in any case */
const hget = (hs, k) => {
  k = String(k).toLowerCase();
  const f = (hs || []).find(x => String(x[0]).toLowerCase() === k);
  return f ? f[1] : undefined;
};

/* what a body is, for the viewer: by its Content-Type, or by a look at it */
function kindOf(ct, text) {
  ct = (ct || '').toLowerCase();
  if (/[/+]json|^application\/json|json$/.test(ct)) return 'json';
  if (/^image\//.test(ct)) return 'image';
  if (/html/.test(ct)) return 'html';
  if (/xml/.test(ct)) return 'xml';
  if (/javascript|ecmascript/.test(ct)) return 'js';
  if (/^text\//.test(ct)) return 'text';
  const s = (text || '').trimStart();
  if (s[0] === '{' || s[0] === '[') { try { JSON.parse(text); return 'json'; } catch {} }
  if (/^<\?xml/.test(s)) return 'xml';
  if (/^<!doctype html|^<html/i.test(s)) return 'html';
  return 'text';
}

function looksBinary(ct, bytes) {
  ct = (ct || '').toLowerCase();
  if (/^(image|audio|video|font)\/|octet-stream|application\/(pdf|zip|gzip|x-tar|x-7z|vnd\.)|protobuf|msgpack/.test(ct)) return true;
  if (/^text\/|json|xml|javascript|html|x-www-form-urlencoded/.test(ct)) return false;
  const n = Math.min(bytes.length, 1024);
  for (let i = 0; i < n; i++) if (bytes[i] === 0) return true;
  return false;
}

function decodeText(bytes, ct) {
  const m = /charset=([^;\s]+)/i.exec(ct || '');
  try { return new TextDecoder(m ? m[1].replace(/"/g, '') : 'utf-8').decode(bytes); } catch { return new TextDecoder().decode(bytes); }
}

/* the live channel with the app on the board */
async function liveGet(key) {
  const r = await fetch(LIVE + (key ? '&key=' + encodeURIComponent(key) : '') + '&_=' + Date.now(), { cache: 'no-store' });
  return r;
}
async function livePost(line) {
  const r = await fetch(LIVE, { method: 'POST', body: line });
  if (!r.ok) throw new Error(tr('La placa no tomó el mensaje (%d)', r.status));
}
/* "reload": the collections, environments or the active one changed here */
const tellBoard = () => livePost('reload').catch(() => {});

/* ---- the Postman model --------------------------------------------------------------
 * A collection is v2.1: items with "item" are folders, the others requests.
 * A place in it is the path of indices from the root's "item" (the root
 * itself is []). A request's url is a string or an object; the editor keeps
 * the object (raw plus the parts made again from it), whose "query" holds
 * the disabled parameters too while "raw" has only the enabled ones. */

const isFolder = it => !!it && Array.isArray(it.item) && !it.request;
const nameOf = n => n ? (n.info ? n.info.name : n.name) || '' : '';

function nodeAt(col, path) {
  let n = col;
  for (const i of path) { n = n && Array.isArray(n.item) ? n.item[i] : null; }
  return n || null;
}

/* the collection, the folders and the node itself */
function chainOf(col, path) {
  const out = [col];
  let n = col;
  for (const i of path) { n = n && n.item ? n.item[i] : null; if (!n) break; out.push(n); }
  return out;
}

/* every node, depth first: fn(node, path) */
function walk(node, fn, path = []) {
  (node.item || []).forEach((it, i) => {
    const p = path.concat(i);
    fn(it, p);
    if (Array.isArray(it.item)) walk(it, fn, p);
  });
}

/* a node again in a collection just read from the card: where it was, if it
 * is still that one; otherwise the only one with that name and kind */
function locate(data, path, name, folder) {
  if (!path.length) return { node: data, path: [] };
  const n = nodeAt(data, path);
  if (n && nameOf(n) === name && isFolder(n) === folder) return { node: n, path };
  const found = [];
  walk(data, (it, p) => { if (nameOf(it) === name && isFolder(it) === folder) found.push(p); });
  if (found.length === 1) return { node: nodeAt(data, found[0]), path: found[0] };
  return null;
}

/* after inserting (delta 1) or removing (-1) the child "index" of "parent",
 * a path elsewhere in the tree moves too; null when it was removed */
function shiftPath(p, parent, index, delta) {
  if (!p || p.length <= parent.length) return p;
  for (let i = 0; i < parent.length; i++) if (p[i] !== parent[i]) return p;
  const q = p.slice(), k = parent.length;
  if (delta > 0) { if (q[k] >= index) q[k]++; } else { if (q[k] === index) return null; if (q[k] > index) q[k]--; }
  return q;
}

/* a duplicate must not carry the original's ids */
function stripIds(n) {
  delete n.id;
  delete n._postman_id;
  (n.item || []).forEach(stripIds);
  return n;
}

const qPair = q => q.value == null ? q.key : q.key + '=' + q.value;

function splitUrl(raw) {
  let s = raw || '', hash = null, query = null;
  const hi = s.indexOf('#');
  if (hi >= 0) { hash = s.slice(hi + 1); s = s.slice(0, hi); }
  const qi = s.indexOf('?');
  if (qi >= 0) { query = s.slice(qi + 1); s = s.slice(0, qi); }
  return { base: s, query, hash };
}

/* the query as it is written: no decoding, Postman keeps the text */
function parseQuery(q) {
  if (!q) return [];
  return q.split('&').filter(x => x !== '').map(x => {
    const e = x.indexOf('=');
    return e < 0 ? { key: x, value: null } : { key: x.slice(0, e), value: x.slice(e + 1) };
  });
}

function urlRaw(u) {
  if (u == null) return '';
  if (typeof u === 'string') return u;
  if (typeof u.raw === 'string') return u.raw;
  let s = '';
  if (u.protocol) s += String(u.protocol).replace(/:?(\/\/)?$/, '') + '://';
  if (u.host) s += Array.isArray(u.host) ? u.host.join('.') : u.host;
  if (u.port) s += ':' + u.port;
  if (u.path != null) {
    const p = Array.isArray(u.path) ? u.path.map(x => typeof x === 'object' && x ? x.value : x).join('/') : String(u.path).replace(/^\//, '');
    if (p) s += '/' + p;
  }
  const q = (u.query || []).filter(x => !x.disabled);
  if (q.length) s += '?' + q.map(qPair).join('&');
  if (u.hash) s += '#' + u.hash;
  return s;
}

/* the parts made again from raw, keeping whatever else the object had */
function fillParts(u) {
  for (const k of ['protocol', 'host', 'port', 'path', 'hash']) delete u[k];
  const { base, hash } = splitUrl(u.raw);
  let rest = base;
  const m = /^([A-Za-z][\w+.-]*):\/\//.exec(rest);
  if (m) { u.protocol = m[1]; rest = rest.slice(m[0].length); }
  const si = rest.indexOf('/');
  let host = si >= 0 ? rest.slice(0, si) : rest;
  const pm = /^(.*[^:]):(\d+)$/.exec(host);
  if (pm) { host = pm[1]; u.port = pm[2]; }
  if (host) u.host = host.split('.');
  if (si >= 0) u.path = rest.slice(si + 1).split('/');
  if (hash != null) u.hash = hash;
  if (!Array.isArray(u.query)) u.query = [];
  return u;
}

/* an editable url object from whatever the file has */
function urlObject(src) {
  const raw = urlRaw(src);
  const u = isObj(src) ? clone(src) : {};
  u.raw = raw;
  const enabled = parseQuery(splitUrl(raw).query);
  const old = Array.isArray(u.query) ? u.query : [];
  const oldOn = old.filter(q => !q.disabled);
  const same = oldOn.length === enabled.length && oldOn.every((q, i) => q.key === enabled[i].key && (q.value ?? null) === enabled[i].value);
  u.query = same ? old : enabled.concat(old.filter(q => q.disabled));
  return fillParts(u);
}

/* the URL typed: the query table follows it, keeping its disabled rows */
function setUrlRaw(u, raw) {
  u.raw = raw;
  const enabled = parseQuery(splitUrl(raw).query);
  const old = u.query || [];
  const oldOn = old.filter(q => !q.disabled);
  /* keep the descriptions of the rows that are still there */
  enabled.forEach((q, i) => { const o = oldOn[i]; if (o && o.key === q.key && o.description) q.description = o.description; });
  const keep = old.filter(q => q.disabled);
  if (!Array.isArray(u.query)) u.query = [];
  u.query.splice(0, u.query.length, ...enabled, ...keep);
  fillParts(u);
}

/* the table changed: raw follows it */
function setUrlQuery(u) {
  const { base, query, hash } = splitUrl(u.raw);
  const on = (u.query || []).filter(q => !q.disabled && (q.key || q.value));
  u.raw = base + (on.length ? '?' + on.map(qPair).join('&') : query === '' ? '?' : '') + (hash != null ? '#' + hash : '');
  fillParts(u);
}

/* v2.1 keeps an auth's fields as [{key, value}], v2.0 as an object */
function authParams(auth) {
  const o = {};
  if (!auth || !auth.type) return o;
  const p = auth[auth.type];
  if (Array.isArray(p)) { for (const x of p) if (x && x.key != null) o[x.key] = x.value; } else if (isObj(p)) Object.assign(o, p);
  return o;
}

function setAuthParams(auth, obj) {
  const old = Array.isArray(auth[auth.type]) ? auth[auth.type] : [];
  auth[auth.type] = Object.entries(obj).map(([key, value]) => {
    const o = old.find(x => x.key === key);
    return { key, value, type: (o && o.type) || 'string' };
  });
}

/* a request without auth (or "inherit") takes its folder's, then the collection's */
function effectiveAuth(reqAuth, chain) {
  if (reqAuth && reqAuth.type && reqAuth.type !== 'inherit') return { auth: reqAuth, from: null };
  for (let i = chain.length - 1; i >= 0; i--) {
    const a = chain[i] && chain[i].auth;
    if (a && a.type && a.type !== 'inherit') return { auth: a, from: chain[i] };
  }
  return { auth: null, from: null };
}

function scriptOf(node, listen) {
  const ev = node && Array.isArray(node.event) ? node.event.find(e => e && e.listen === listen) : null;
  if (!ev || !ev.script) return '';
  if (ev.disabled) return '';
  const x = ev.script.exec;
  return Array.isArray(x) ? x.join('\n') : typeof x === 'string' ? x : '';
}

function setScript(node, listen, text) {
  if (!Array.isArray(node.event)) node.event = [];
  const i = node.event.findIndex(e => e && e.listen === listen);
  if (!text.trim()) {
    if (i >= 0) node.event.splice(i, 1);
    if (!node.event.length) delete node.event;
    return;
  }
  const exec = text.split('\n');
  if (i >= 0) {
    const ev = node.event[i];
    ev.script = Object.assign({}, ev.script || {}, { type: 'text/javascript', exec });
  } else {
    node.event.push({ listen, script: { type: 'text/javascript', exec } });
  }
}

/* an item ready for the editor: request is an object, url an object */
function normItem(it) {
  const n = clone(it);
  if (typeof n.request === 'string') n.request = { method: 'GET', url: n.request };
  if (!n.request) n.request = { method: 'GET', url: '' };
  const r = n.request;
  r.method = String(r.method || 'GET').toUpperCase();
  r.url = urlObject(r.url);
  if (typeof r.header === 'string') {
    r.header = r.header.split('\n').map(l => { const k = l.indexOf(':'); return k > 0 ? { key: l.slice(0, k).trim(), value: l.slice(k + 1).trim() } : null; }).filter(Boolean);
  }
  if (!Array.isArray(r.header)) r.header = [];
  return n;
}

/* what is written back: a body of "none" with nothing in it is left out */
function cleanItem(it) {
  const n = clone(it);
  const b = n.request && n.request.body;
  if (b && (!b.mode || b.mode === 'none') && !b.raw && !(b.urlencoded || []).length && !(b.formdata || []).length && !b.graphql) delete n.request.body;
  const u = n.request && n.request.url;
  if (isObj(u)) {
    if (!(u.variable || []).length) delete u.variable;
    if (!(u.query || []).length) delete u.query;
  }
  return n;
}

function newRequest(name) {
  return { name, request: { method: 'GET', header: [], url: { raw: '' } }, response: [] };
}

/* a v2.0 collection: the same, but the auth's fields were objects */
function upgradeV20(col) {
  const fix = n => {
    if (n.auth && n.auth.type && isObj(n.auth[n.auth.type])) {
      const o = n.auth[n.auth.type];
      n.auth[n.auth.type] = Object.entries(o).map(([key, value]) => ({ key, value, type: 'string' }));
    }
    if (n.request && n.request.auth) fix(n.request);
    (n.item || []).forEach(fix);
  };
  fix(col);
  col.info.schema = SCHEMA;
  return col;
}

/* ---- the card -------------------------------------------------------------------- */

async function readJSON(path) {
  const t = await P.fsText(path);
  if (t == null) return null;
  return JSON.parse(t);
}
async function writeJSON(path, v) {
  await P.fsPut(path, JSON.stringify(v, null, '\t') + '\n');
}
let dirsMade = null;
function ensureDirs() {
  if (!dirsMade) dirsMade = (async () => {
    const have = new Set((await P.fsList(DIR)).filter(e => e.dir).map(e => e.name));
    if (!have.size) await P.fsMkdir(DIR);
    for (const d of [D_COLS, D_ENVS, D_RUNS, D_RESP, D_WEB]) if (!have.has(d.slice(DIR.length + 1))) await P.fsMkdir(d);
  })().catch(e => { dirsMade = null; throw e; });
  return dirsMade;
}

/* a free file name in a folder, from a name */
function freeName(list, name, def) {
  const base = P.fsSlug(name || '', 40, def);
  const taken = new Set(list.map(x => x.file));
  let f = base + '.json';
  for (let i = 2; taken.has(f); i++) f = base + '-' + i + '.json';
  return f;
}

/* the files of a folder, read again only when their date or size changed */
async function loadFolder(dir, old) {
  const ls = (await P.fsList(dir)).filter(e => !e.dir && /\.json$/i.test(e.name));
  const byFile = new Map((old || []).map(x => [x.file, x]));
  return Promise.all(ls.map(async e => {
    const o = byFile.get(e.name);
    if (o && o.mtime === e.mtime && o.size === e.size && !o.err) return o;
    try {
      const data = await readJSON(dir + '/' + e.name);
      return { file: e.name, mtime: e.mtime, size: e.size, data, err: '' };
    } catch (err) {
      return { file: e.name, mtime: e.mtime, size: e.size, data: null, err: err.message };
    }
  }));
}

/* ---- variables ----------------------------------------------------------------------
 * The first that has it: the script's own (pm.variables.set), the active
 * environment, the collection's, the globals. Unknown ones stay as they are.
 * A value can name others ({{a}} = "{{b}}/x"): a few passes. */

const VAR_RE = /\{\{([^{}]+?)\}\}/g;

function dynVar(k) {
  switch (k) {
    case '$guid': case '$randomUUID': return uuid();
    case '$timestamp': return String(Math.floor(Date.now() / 1000));
    case '$isoTimestamp': return new Date().toISOString();
    case '$randomInt': return String(Math.floor(Math.random() * 1001));
  }
  return undefined;
}
const DYN = ['$guid', '$randomUUID', '$timestamp', '$isoTimestamp', '$randomInt'];

const valStr = v => v == null ? '' : typeof v === 'object' ? JSON.stringify(v) : String(v);

function resolveText(text, lookup) {
  if (typeof text !== 'string' || !text.includes('{{')) return text;
  let s = text;
  for (let pass = 0; pass < 6; pass++) {
    let changed = false;
    s = s.replace(VAR_RE, (m, k) => {
      const v = k[0] === '$' ? dynVar(k) ?? lookup(k) : lookup(k);
      if (v === undefined) return m;
      changed = true;
      return valStr(v);
    });
    if (!changed || !s.includes('{{')) break;
  }
  return s;
}

const enabledEntry = e => e && e.key != null && e.enabled !== false && !e.disabled;

/* one scope of variables for a script run: what it changes is kept as
 * operations, merged afterwards into the file just read again */
class VarScope {
  constructor(name, entries) {
    this.name = name;
    this.map = new Map();
    this.ops = new Map();
    for (const e of entries || []) if (enabledEntry(e)) this.map.set(String(e.key), e.value);
  }
  get(k) { return this.map.get(String(k)); }
  has(k) { return this.map.has(String(k)); }
  set(k, v) { k = String(k); this.map.set(k, v); this.ops.set(k, { v }); }
  unset(k) { k = String(k); this.map.delete(k); this.ops.set(k, null); }
  clear() { for (const k of [...this.map.keys()]) this.unset(k); }
  toObject() { return Object.fromEntries(this.map); }
}

/* the files keep text: a number or an object set by a script is written as text */
const fileVal = v => v == null ? '' : typeof v === 'object' ? JSON.stringify(v) : typeof v === 'string' ? v : String(v);

function applyOps(values, ops, env) {
  for (const [k, op] of ops) {
    const i = values.findIndex(e => e && e.key === k);
    if (op === null) { if (i >= 0) values.splice(i, 1); continue; }
    const v = fileVal(op.v);
    if (i >= 0) {
      values[i].value = v;
      if (env) values[i].enabled = true; else delete values[i].disabled;
    } else {
      values.push(env ? { key: k, value: v, type: 'default', enabled: true } : { key: k, value: v, type: 'string' });
    }
  }
}

/* an editor's table saved over the file read again: the rows it did not
 * touch take the file's version, rows added elsewhere stay */
function mergeTable(orig, edited, fresh) {
  const by = a => new Map((a || []).filter(e => e && e.key != null).map(e => [e.key, e]));
  const o = by(orig), e = by(edited), f = by(fresh);
  const out = [];
  for (const row of edited) {
    if (!row.key) continue;
    const was = o.get(row.key), now = f.get(row.key);
    out.push(was && now && JSON.stringify(was) === JSON.stringify(row) ? now : row);
  }
  for (const row of fresh || []) if (row && !e.has(row.key) && !o.has(row.key)) out.push(row);
  return out;
}

/* ---- the script runtime ---------------------------------------------------------------
 * The browser's own JavaScript runs the scripts, given the same pm the
 * board's interpreter (main/ct_js.c) offers: pm.test and pm.expect (the
 * part of chai collections use), pm.response, the variable scopes,
 * pm.request, pm.info, pm.execution, console, and the old globals (tests[],
 * responseBody, postman.*). The assertion messages are chai's own, in
 * English, as Postman shows them. */

class AssertionError extends Error {
  constructor(m) { super(m); this.name = 'AssertionError'; }
}

function fmtV(v) {
  try {
    if (typeof v === 'string') return "'" + (v.length > 80 ? v.slice(0, 77) + '...' : v) + "'";
    if (v === undefined) return 'undefined';
    if (typeof v === 'function') return '[Function' + (v.name ? ': ' + v.name : '') + ']';
    if (v instanceof RegExp) return String(v);
    if (typeof v !== 'object' || v === null) return String(v);
    const s = JSON.stringify(v);
    return s.length > 80 ? s.slice(0, 77) + '...' : s;
  } catch { return String(v); }
}

function deepEq(a, b) {
  if (a === b) return true;
  if (typeof a === 'number' && typeof b === 'number' && Number.isNaN(a) && Number.isNaN(b)) return true;
  if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object') return false;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (a instanceof Date || b instanceof Date) return a instanceof Date && b instanceof Date && a.getTime() === b.getTime();
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  return ka.every(k => Object.prototype.hasOwnProperty.call(b, k) && deepEq(a[k], b[k]));
}

function typeOf(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'array';
  if (v instanceof Date) return 'date';
  if (v instanceof RegExp) return 'regexp';
  if (v instanceof Error) return 'error';
  return typeof v;
}

/* "data[0].id", "a.b.c": lodash's paths, for nested properties and jsonBody */
function pathGet(obj, path) {
  const keys = String(path).match(/[^.[\]]+/g) || [];
  let v = obj;
  for (const k of keys) {
    if (v == null || typeof v !== 'object' || !(k in v)) return { has: false };
    v = v[k];
  }
  return { has: true, v };
}

const CHAINS = ['to', 'be', 'been', 'is', 'that', 'which', 'and', 'has', 'have', 'with', 'at', 'of', 'same', 'but', 'does', 'still', 'also'];

class Assertion {
  constructor(v, msg) { this._v = v; this._msg = msg; this._neg = false; this._deep = false; this._nested = false; this._any = false; this._contains = false; this._len = false; this._own = false; }
  _ok(cond, pos, neg) {
    if (!!cond === this._neg) throw new AssertionError((this._msg ? this._msg + ': ' : '') + (this._neg ? neg : pos));
    return this;
  }
  /* a method that is also a chain (chai's include, length, a): called it
   * asserts, followed by a property it sets its flag and goes on */
  _chainable(method, flag) {
    const self = this;
    const fn = function (...a) { return method.apply(self, a); };
    return new Proxy(fn, {
      get(t, p) {
        if (p === '__ctChain') return true;
        if (typeof p === 'symbol' || p === 'call' || p === 'apply' || p === 'bind' || p === 'prototype') return t[p];
        if (flag) self[flag] = true;
        const v = self[p];
        return typeof v === 'function' && !v.__ctChain ? v.bind(self) : v;
      },
    });
  }
  get not() { this._neg = !this._neg; return this; }
  get deep() { this._deep = true; return this; }
  get nested() { this._nested = true; return this; }
  get own() { this._own = true; return this; }
  get any() { this._any = true; return this; }
  get all() { this._any = false; return this; }
  get ok() { return this._ok(this._v, `expected ${fmtV(this._v)} to be truthy`, `expected ${fmtV(this._v)} to be falsy`); }
  get true() { return this._ok(this._v === true, `expected ${fmtV(this._v)} to be true`, `expected ${fmtV(this._v)} to be false`); }
  get false() { return this._ok(this._v === false, `expected ${fmtV(this._v)} to be false`, `expected ${fmtV(this._v)} to be true`); }
  get null() { return this._ok(this._v === null, `expected ${fmtV(this._v)} to be null`, `expected ${fmtV(this._v)} not to be null`); }
  get undefined() { return this._ok(this._v === undefined, `expected ${fmtV(this._v)} to be undefined`, `expected ${fmtV(this._v)} not to be undefined`); }
  get NaN() { return this._ok(Number.isNaN(this._v), `expected ${fmtV(this._v)} to be NaN`, `expected ${fmtV(this._v)} not to be NaN`); }
  get exist() { return this._ok(this._v != null, `expected ${fmtV(this._v)} to exist`, `expected ${fmtV(this._v)} to not exist`); }
  get exists() { return this.exist; }
  get finite() { return this._ok(Number.isFinite(this._v), `expected ${fmtV(this._v)} to be a finite number`, `expected ${fmtV(this._v)} to not be a finite number`); }
  get empty() {
    const v = this._v;
    let n;
    if (typeof v === 'string' || Array.isArray(v)) n = v.length;
    else if (v instanceof Map || v instanceof Set) n = v.size;
    else if (v && typeof v === 'object') n = Object.keys(v).length;
    else throw new AssertionError(`.empty was passed non-string primitive ${fmtV(v)}`);
    return this._ok(n === 0, `expected ${fmtV(v)} to be empty`, `expected ${fmtV(v)} not to be empty`);
  }
  get include() { return this._chainable(this._include, '_contains'); }
  get includes() { return this.include; }
  get contain() { return this.include; }
  get contains() { return this.include; }
  get length() { return this._chainable(this._lengthOf, '_len'); }
  get lengthOf() { return this.length; }
  get a() { return this._chainable(this._a); }
  get an() { return this.a; }
  _a(type) {
    const t = typeOf(this._v), want = String(type).toLowerCase();
    const art = /^[aeiou]/.test(want) ? 'an ' : 'a ';
    return this._ok(t === want, `expected ${fmtV(this._v)} to be ${art}${want}`, `expected ${fmtV(this._v)} not to be ${art}${want}`);
  }
  _include(x) {
    const v = this._v;
    let ok;
    if (typeof v === 'string') ok = v.includes(String(x));
    else if (Array.isArray(v)) ok = this._deep ? v.some(e => deepEq(e, x)) : v.includes(x);
    else if (v instanceof Set || v instanceof Map) ok = v.has(x);
    else if (v && typeof v === 'object' && x && typeof x === 'object') ok = Object.keys(x).every(k => k in v && (this._deep ? deepEq(v[k], x[k]) : v[k] === x[k]));
    else throw new AssertionError(`object tested must be an array, a map, an object, a set, a string, or a weakset, but ${typeOf(v)} given`);
    return this._ok(ok, `expected ${fmtV(v)} to ${this._deep ? 'deep ' : ''}include ${fmtV(x)}`, `expected ${fmtV(v)} to not ${this._deep ? 'deep ' : ''}include ${fmtV(x)}`);
  }
  _lengthOf(n) {
    const v = this._v, len = v == null ? undefined : v.size ?? v.length;
    return this._ok(len === n, `expected ${fmtV(v)} to have a length of ${n} but got ${len}`, `expected ${fmtV(v)} to not have a length of ${n}`);
  }
  _cmpSubject(what) {
    if (this._len) { const v = this._v; return { x: v == null ? undefined : v.size ?? v.length, d: `${fmtV(v)} to have a length ` }; }
    return { x: this._v, d: `${fmtV(this._v)} to be ` + what };
  }
  equal(x) {
    if (this._deep) return this.eql(x);
    return this._ok(this._v === x, `expected ${fmtV(this._v)} to equal ${fmtV(x)}`, `expected ${fmtV(this._v)} to not equal ${fmtV(x)}`);
  }
  equals(x) { return this.equal(x); }
  eq(x) { return this.equal(x); }
  eql(x) { return this._ok(deepEq(this._v, x), `expected ${fmtV(this._v)} to deeply equal ${fmtV(x)}`, `expected ${fmtV(this._v)} to not deeply equal ${fmtV(x)}`); }
  eqls(x) { return this.eql(x); }
  above(n) { const s = this._cmpSubject(''); return this._ok(s.x > n, `expected ${s.d}above ${n}`, `expected ${s.d}at most ${n}`); }
  gt(n) { return this.above(n); }
  greaterThan(n) { return this.above(n); }
  least(n) { const s = this._cmpSubject(''); return this._ok(s.x >= n, `expected ${s.d}at least ${n}`, `expected ${s.d}below ${n}`); }
  gte(n) { return this.least(n); }
  greaterThanOrEqual(n) { return this.least(n); }
  below(n) { const s = this._cmpSubject(''); return this._ok(s.x < n, `expected ${s.d}below ${n}`, `expected ${s.d}at least ${n}`); }
  lt(n) { return this.below(n); }
  lessThan(n) { return this.below(n); }
  most(n) { const s = this._cmpSubject(''); return this._ok(s.x <= n, `expected ${s.d}at most ${n}`, `expected ${s.d}above ${n}`); }
  lte(n) { return this.most(n); }
  lessThanOrEqual(n) { return this.most(n); }
  within(a, b) { const s = this._cmpSubject(''); return this._ok(s.x >= a && s.x <= b, `expected ${s.d}within ${a}..${b}`, `expected ${s.d}not within ${a}..${b}`); }
  closeTo(n, d) { return this._ok(Math.abs(this._v - n) <= d, `expected ${fmtV(this._v)} to be close to ${n} +/- ${d}`, `expected ${fmtV(this._v)} not to be close to ${n} +/- ${d}`); }
  approximately(n, d) { return this.closeTo(n, d); }
  oneOf(list) {
    const ok = Array.isArray(list) && (this._deep ? list.some(e => deepEq(e, this._v)) : list.includes(this._v));
    return this._ok(ok, `expected ${fmtV(this._v)} to be one of ${fmtV(list)}`, `expected ${fmtV(this._v)} to not be one of ${fmtV(list)}`);
  }
  match(re) {
    const ok = re instanceof RegExp ? re.test(String(this._v)) : String(this._v).includes(String(re));
    return this._ok(ok, `expected ${fmtV(this._v)} to match ${fmtV(re)}`, `expected ${fmtV(this._v)} not to match ${fmtV(re)}`);
  }
  matches(re) { return this.match(re); }
  string(s) { return this._ok(typeof this._v === 'string' && this._v.includes(s), `expected ${fmtV(this._v)} to contain ${fmtV(s)}`, `expected ${fmtV(this._v)} to not contain ${fmtV(s)}`); }
  property(name, ...val) {
    const v = this._v;
    let r;
    if (this._nested) r = pathGet(v, name);
    else if (v != null && (typeof v === 'object' || typeof v === 'function') && (this._own ? Object.prototype.hasOwnProperty.call(v, name) : name in v)) r = { has: true, v: v[name] };
    else if (typeof v === 'string' && name === 'length') r = { has: true, v: v.length };
    else r = { has: false };
    const what = this._nested ? 'nested property' : this._own ? 'own property' : 'property';
    if (val.length) {
      const same = r.has && (this._deep ? deepEq(r.v, val[0]) : r.v === val[0]);
      this._ok(same, `expected ${fmtV(v)} to have ${this._deep ? 'deep ' : ''}${what} '${name}' of ${fmtV(val[0])}` + (r.has ? `, but got ${fmtV(r.v)}` : ''),
        `expected ${fmtV(v)} to not have ${this._deep ? 'deep ' : ''}${what} '${name}' of ${fmtV(val[0])}`);
    } else {
      this._ok(r.has, `expected ${fmtV(v)} to have ${what} '${name}'`, `expected ${fmtV(v)} to not have ${what} '${name}'`);
    }
    if (!this._neg && r.has) this._v = r.v;
    return this;
  }
  ownProperty(name, ...val) { this._own = true; return this.property(name, ...val); }
  haveOwnProperty(name, ...val) { return this.ownProperty(name, ...val); }
  nestedProperty(name, ...val) { this._nested = true; return this.property(name, ...val); }
  keys(...ks) {
    if (ks.length === 1 && Array.isArray(ks[0])) ks = ks[0];
    else if (ks.length === 1 && isObj(ks[0])) ks = Object.keys(ks[0]);
    const v = this._v, have = v instanceof Map ? [...v.keys()] : v && typeof v === 'object' ? Object.keys(v) : [];
    let ok;
    if (this._any) ok = ks.some(k => have.includes(k));
    else if (this._contains) ok = ks.every(k => have.includes(k));
    else ok = ks.length === have.length && ks.every(k => have.includes(k));
    const w = this._any ? 'any of keys' : 'keys';
    return this._ok(ok, `expected ${fmtV(v)} to have ${w} ${ks.map(k => fmtV(k)).join(', ')}`, `expected ${fmtV(v)} to not have ${w} ${ks.map(k => fmtV(k)).join(', ')}`);
  }
  key(...ks) { return this.keys(...ks); }
  members(list) {
    const v = this._v;
    if (!Array.isArray(v)) throw new AssertionError(`expected ${fmtV(v)} to be an array`);
    const has = x => v.some(e => deepEq(e, x));
    const ok = this._contains ? list.every(has) : list.length === v.length && list.every(has) && v.every(e => list.some(x => deepEq(e, x)));
    return this._ok(ok, `expected ${fmtV(v)} to have the same members as ${fmtV(list)}`, `expected ${fmtV(v)} to not have the same members as ${fmtV(list)}`);
  }
  instanceOf(C) { return this._ok(this._v instanceof C, `expected ${fmtV(this._v)} to be an instance of ${C && C.name}`, `expected ${fmtV(this._v)} to not be an instance of ${C && C.name}`); }
  instanceof(C) { return this.instanceOf(C); }
  satisfy(fn) { return this._ok(fn(this._v), `expected ${fmtV(this._v)} to satisfy ${fmtV(fn)}`, `expected ${fmtV(this._v)} to not satisfy ${fmtV(fn)}`); }
  satisfies(fn) { return this.satisfy(fn); }
  throw(want) {
    let threw = false, err;
    try { this._v(); } catch (e) { threw = true; err = e; }
    let ok = threw;
    if (threw && want != null) {
      if (want instanceof RegExp) ok = want.test(err && err.message);
      else if (typeof want === 'string') ok = String(err && err.message).includes(want);
      else if (typeof want === 'function') ok = err instanceof want;
    }
    return this._ok(ok, `expected ${fmtV(this._v)} to throw` + (want != null ? ' ' + fmtV(want) : ''), `expected ${fmtV(this._v)} to not throw` + (threw ? ` but ${fmtV(err && err.message)} was thrown` : ''));
  }
  throws(w) { return this.throw(w); }
  Throw(w) { return this.throw(w); }
}
for (const c of CHAINS) Object.defineProperty(Assertion.prototype, c, { get() { return this; } });

function makeExpect() {
  const expect = (v, msg) => new Assertion(v, msg);
  expect.fail = (a, b, msg) => { throw new AssertionError(typeof a === 'string' && b === undefined ? a : msg || `expected ${fmtV(a)} to equal ${fmtV(b)}`); };
  return expect;
}

/* pm.response.to...: Postman's own assertions about a response */
function respAssert(res, neg) {
  const chk = (cond, pos, ng) => { if (!!cond === neg) throw new AssertionError(neg ? ng : pos); };
  const o = {};
  const self = () => o;
  for (const c of CHAINS) Object.defineProperty(o, c, { get: self });
  Object.defineProperty(o, 'not', { get: () => respAssert(res, !neg) });
  const cls = (name, test, label) => Object.defineProperty(o, name, { get() {
    chk(test(res.code), `expected response to be ${label} but got ${res.code}`, `expected response to not be ${label} but got ${res.code}`);
    return o;
  } });
  cls('ok', c => c >= 200 && c < 300, '2XX');
  cls('success', c => c >= 200 && c < 300, '2XX');
  cls('info', c => c >= 100 && c < 200, '1XX');
  cls('redirection', c => c >= 300 && c < 400, '3XX');
  cls('clientError', c => c >= 400 && c < 500, '4XX');
  cls('serverError', c => c >= 500 && c < 600, '5XX');
  cls('error', c => c >= 400 && c < 600, '4XX or 5XX');
  cls('notFound', c => c === 404, '404');
  cls('unauthorized', c => c === 401, '401');
  cls('unauthorised', c => c === 401, '401');
  cls('forbidden', c => c === 403, '403');
  cls('badRequest', c => c === 400, '400');
  cls('accepted', c => c === 202, '202');
  cls('rateLimited', c => c === 429, '429');
  Object.defineProperty(o, 'json', { get() {
    let ok = true;
    try { res.json(); } catch { ok = false; }
    chk(ok, 'expected response body to be a valid json', 'expected response body not to be a valid json');
    return o;
  } });
  Object.defineProperty(o, 'withBody', { get() { chk(res.text() !== '', 'expected response to have content in body', 'expected response to not have content in body'); return o; } });
  o.status = x => {
    if (typeof x === 'number') chk(res.code === x, `expected response to have status code ${x} but got ${res.code}`, `expected response to not have status code ${x}`);
    else chk(res.status === x, `expected response to have status reason '${x}' but got '${res.status}'`, `expected response to not have status reason '${x}'`);
    return o;
  };
  o.code = o.status;
  o.header = (k, ...v) => {
    const has = res.headers.has(k), got = res.headers.get(k);
    if (v.length) chk(has && got === String(v[0]), `expected '${k}' response header to be '${v[0]}' but got '${got}'`, `expected '${k}' response header to not be '${v[0]}'`);
    else chk(has, `expected response to have header with key '${k}'`, `expected response to not have header with key '${k}'`);
    return o;
  };
  o.body = (...x) => {
    const t = res.text();
    if (!x.length) chk(t !== '', 'expected response to have content in body', 'expected response to not have content in body');
    else if (x[0] instanceof RegExp) chk(x[0].test(t), `expected response body to match ${x[0]}`, `expected response body to not match ${x[0]}`);
    else if (typeof x[0] === 'string') chk(t === x[0], `expected response body to equal ${fmtV(x[0])}`, `expected response body to not equal ${fmtV(x[0])}`);
    else { let j; try { j = res.json(); } catch {} chk(deepEq(j, x[0]), `expected response body json to equal ${fmtV(x[0])}`, `expected response body json to not equal ${fmtV(x[0])}`); }
    return o;
  };
  o.jsonBody = (...a) => {
    let j, ok = true;
    try { j = res.json(); } catch { ok = false; }
    if (!a.length) { chk(ok, 'expected response body to be a valid json', 'expected response body not to be a valid json'); return o; }
    if (isObj(a[0])) { chk(ok && deepEq(j, a[0]), `expected response body json to equal ${fmtV(a[0])}`, `expected response body json to not equal ${fmtV(a[0])}`); return o; }
    const r = ok ? pathGet(j, a[0]) : { has: false };
    if (a.length > 1) chk(r.has && deepEq(r.v, a[1]), `expected response body json at "${a[0]}" to contain ${fmtV(a[1])}` + (r.has ? ` but got ${fmtV(r.v)}` : ''), `expected response body json at "${a[0]}" to not contain ${fmtV(a[1])}`);
    else chk(r.has, `expected response body json to contain "${a[0]}"`, `expected response body json to not contain "${a[0]}"`);
    return o;
  };
  o.responseTime = n => { chk(res.responseTime <= n, `expected response time to be at most ${n} but got ${res.responseTime}`, `expected response time to be above ${n}`); return o; };
  return o;
}

/* a header list with Postman's methods ([[name, value]] or [{key, value}]) */
function headerList(get, writable) {
  const all = () => get().filter(e => !e.disabled);
  const same = (e, k) => String(e.key).toLowerCase() === String(k).toLowerCase();
  const o = {
    get: k => { const f = all().find(e => same(e, k)); return f ? f.value : undefined; },
    has: (k, v) => all().some(e => same(e, k) && (v === undefined || e.value === v)),
    toObject: () => Object.fromEntries(all().map(e => [e.key, e.value])),
    all: () => all().map(e => ({ key: e.key, value: e.value })),
    each: fn => all().forEach(e => fn({ key: e.key, value: e.value })),
    count: () => all().length,
    one: k => { const f = all().find(e => same(e, k)); return f ? { key: f.key, value: f.value } : undefined; },
    toJSON: () => o.all(),
  };
  if (writable) {
    o.add = x => { if (typeof x === 'string') { const i = x.indexOf(':'); x = { key: x.slice(0, i).trim(), value: x.slice(i + 1).trim() }; } writable().push({ key: String(x.key), value: valStr(x.value) }); };
    o.upsert = x => {
      if (typeof x === 'string') { const i = x.indexOf(':'); x = { key: x.slice(0, i).trim(), value: x.slice(i + 1).trim() }; }
      const list = writable(), f = list.find(e => same(e, x.key));
      if (f) { f.value = valStr(x.value); delete f.disabled; } else list.push({ key: String(x.key), value: valStr(x.value) });
    };
    o.remove = k => { const list = writable(); for (let i = list.length - 1; i >= 0; i--) if (same(list[i], typeof k === 'object' ? k.key : k)) list.splice(i, 1); };
  } else {
    o.add = o.upsert = o.remove = () => {};
  }
  return o;
}

/* pm.response, from a normalized answer */
function makeResponse(r) {
  const pairs = () => (r.headers || []).map(([key, value]) => ({ key, value }));
  let parsed, done = false;
  const res = {
    code: r.status,
    status: r.reason || REASON[r.status] || '',
    responseTime: Math.round(r.ms || 0),
    responseSize: r.size || 0,
    headers: headerList(pairs, null),
    text: () => r.text || '',
    json() {
      if (!done) { parsed = JSON.parse((r.text || '').replace(/^﻿/, '')); done = true; }
      return parsed;
    },
    reason: () => r.reason || REASON[r.status] || '',
    size: () => ({ body: r.size || 0, header: 0, total: r.size || 0 }),
    cookies: { get: () => undefined, has: () => false, toObject: () => ({}) },
    toJSON: () => ({ code: r.status, status: r.reason, responseTime: r.ms, headers: pairs(), body: r.text }),
  };
  Object.defineProperty(res, 'to', { get: () => respAssert(res, false) });
  Object.defineProperty(res, 'body', { get: () => r.text || '' });
  return res;
}

/* pm.request: before sending, the request to change; in the tests, the one
 * that was sent */
function makeRequest(ctx, phase) {
  const rq = ctx.req;
  if (phase === 'test' && ctx.final) {
    const f = ctx.final;
    return {
      url: f.url, method: f.method, name: ctx.name,
      headers: headerList(() => f.headers.map(([key, value]) => ({ key, value })), null),
      body: { mode: (rq.body && rq.body.mode) || 'none', raw: f.body, toString: () => f.body },
      auth: rq.auth,
      toJSON: () => ({ url: f.url, method: f.method, header: f.headers, body: f.body }),
    };
  }
  if (!rq.body) rq.body = { mode: 'none' };
  const body = {
    get mode() { return rq.body.mode; }, set mode(v) { rq.body.mode = v; },
    get raw() { return rq.body.raw || ''; }, set raw(v) { rq.body.raw = String(v); if (!rq.body.mode || rq.body.mode === 'none') rq.body.mode = 'raw'; },
    get urlencoded() { return headerList(() => rq.body.urlencoded || [], () => rq.body.urlencoded || (rq.body.urlencoded = [])); },
    get formdata() { return headerList(() => rq.body.formdata || [], () => rq.body.formdata || (rq.body.formdata = [])); },
    get graphql() { return rq.body.graphql; },
    update(v) { if (typeof v === 'string') { rq.body.mode = 'raw'; rq.body.raw = v; } else if (isObj(v)) Object.assign(rq.body, v); },
    toString() { return rq.body.raw || ''; },
  };
  const o = {
    get url() { return urlRaw(rq.url); },
    set url(v) { setUrlRaw(rq.url, String(v)); },
    get method() { return rq.method; },
    set method(v) { rq.method = String(v).toUpperCase(); },
    name: ctx.name,
    headers: headerList(() => rq.header || [], () => rq.header || (rq.header = [])),
    body,
    get auth() { return rq.auth; },
    set auth(v) { rq.auth = v; },
    addHeader: x => o.headers.add(x),
    removeHeader: k => o.headers.remove(k),
    toJSON: () => clone(rq),
  };
  return o;
}

function scopeApi(sc, ctx, label) {
  return {
    get name() { return label; },
    get: k => sc.get(k),
    set: (k, v) => { sc.set(k, v); if (sc.dead) ctx.console.push({ level: 'warn', text: tr('No hay entorno activo: «%s» no se guarda', k), where: '' }); },
    unset: k => sc.unset(k),
    has: k => sc.has(k),
    clear: () => sc.clear(),
    toObject: () => sc.toObject(),
    replaceIn: t => resolveText(t, k => sc.get(k)),
    toJSON: () => sc.toObject(),
  };
}

/* everything a script gets, as the parameters of its function */
const SCRIPT_ARGS = ['pm', 'postman', 'console', 'tests', 'responseBody', 'responseCode', 'responseTime', 'responseHeaders',
  'request', 'environment', 'globals', 'iteration', 'data', 'require'];

function lookupIn(ctx) {
  const s = ctx.scopes;
  return k => s.local.has(k) ? s.local.get(k) : s.env.has(k) ? s.env.get(k) : s.coll.has(k) ? s.coll.get(k) : s.glob.has(k) ? s.glob.get(k) : undefined;
}

function makePm(ctx, phase, where, legacyTests) {
  const s = ctx.scopes;
  const lookup = lookupIn(ctx);
  const test = (name, fn) => {
    const t = { name: String(name), ok: true, error: '' };
    ctx.tests.push(t);
    if (typeof fn !== 'function') { t.skipped = true; return; }
    try {
      const r = fn();
      if (r && typeof r.then === 'function') ctx.pending.push(Promise.resolve(r).then(() => {}, e => { t.ok = false; t.error = errText(e); }));
    } catch (e) { t.ok = false; t.error = errText(e); }
  };
  test.skip = name => { ctx.tests.push({ name: String(name), ok: true, error: '', skipped: true }); };
  const setNext = n => { ctx.next = n == null ? null : String(n); };
  const res = phase === 'test' && ctx.resp ? makeResponse(ctx.resp) : undefined;
  const pm = {
    test,
    expect: makeExpect(),
    response: res,
    request: makeRequest(ctx, phase),
    environment: scopeApi(s.env, ctx, s.env.name),
    globals: scopeApi(s.glob, ctx, 'globals'),
    collectionVariables: scopeApi(s.coll, ctx, s.coll.name),
    variables: {
      get: k => lookup(String(k)),
      set: (k, v) => s.local.set(String(k), v),
      has: k => lookup(String(k)) !== undefined,
      unset: k => s.local.delete(String(k)),
      toObject: () => Object.assign({}, s.glob.toObject(), s.coll.toObject(), s.env.toObject(), Object.fromEntries(s.local)),
      replaceIn: t => resolveText(t, lookup),
    },
    iterationData: { get: () => undefined, has: () => false, toObject: () => ({}), set: () => {}, unset: () => {} },
    info: { requestName: ctx.name, eventName: phase, iteration: ctx.iteration || 0, iterationCount: ctx.iterations || 1, requestId: ctx.id },
    execution: {
      setNextRequest: setNext,
      skipRequest: () => { if (phase === 'prerequest') ctx.skip = true; },
      location: ctx.location,
    },
    sendRequest: (req, cb) => sendFromScript(ctx, req, cb),
    cookies: { get: () => undefined, has: () => false, toObject: () => ({}) },
    require: name => sandboxRequire(name),
  };
  pm.execution.location.current = where;
  const postman = {
    setEnvironmentVariable: (k, v) => s.env.set(k, v),
    getEnvironmentVariable: k => s.env.get(k),
    clearEnvironmentVariable: k => s.env.unset(k),
    clearEnvironmentVariables: () => s.env.clear(),
    setGlobalVariable: (k, v) => s.glob.set(k, v),
    getGlobalVariable: k => s.glob.get(k),
    clearGlobalVariable: k => s.glob.unset(k),
    clearGlobalVariables: () => s.glob.clear(),
    getVariable: k => lookup(String(k)),
    setNextRequest: setNext,
    getResponseHeader: k => res ? res.headers.get(k) : undefined,
    getResponseCookie: () => undefined,
  };
  const con = {};
  for (const lvl of ['log', 'info', 'warn', 'error', 'debug']) {
    con[lvl] = (...a) => {
      if (ctx.console.length >= 500) return;
      const text = a.map(x => typeof x === 'string' ? x : (() => { try { return JSON.stringify(x, null, 2) ?? String(x); } catch { return String(x); } })()).join(' ');
      ctx.console.push({ level: lvl, text: text.length > 4000 ? text.slice(0, 4000) + '…' : text, where });
    };
  }
  const headersObj = res ? Object.fromEntries((ctx.resp.headers || []).map(([k, v]) => [k, v])) : {};
  return [pm, postman, con, legacyTests,
    res ? res.text() : undefined,
    res ? { code: res.code, name: res.status, detail: res.status } : undefined,
    res ? res.responseTime : undefined,
    headersObj,
    { url: phase === 'test' && ctx.final ? ctx.final.url : urlRaw(ctx.req.url), method: ctx.req.method, headers: Object.fromEntries((ctx.req.header || []).filter(e => !e.disabled).map(e => [e.key, e.value])), data: (ctx.req.body && ctx.req.body.raw) || {} },
    s.env.toObject(), s.glob.toObject(), ctx.iteration || 0, {}, pm.require];
}

function sandboxRequire(name) {
  throw new Error(tr("require('%s'): Cartero no trae librerías", name));
}

/* one script: errors are caught and kept, the tests' and sendRequest's
 * promises are waited for */
async function runScript(ctx, phase, code, where) {
  if (!code || !code.trim()) return;
  const legacy = {};
  let fn;
  try { fn = new Function(...SCRIPT_ARGS, code); } catch (e) { ctx.errors.push({ where, phase, error: errText(e) }); return; }
  try {
    const r = fn(...makePm(ctx, phase, where, legacy));
    if (r && typeof r.then === 'function') ctx.pending.push(Promise.resolve(r).catch(e => ctx.errors.push({ where, phase, error: errText(e) })));
  } catch (e) {
    ctx.errors.push({ where, phase, error: errText(e) });
  }
  for (let i = 0; i < 20 && ctx.pending.length; i++) await Promise.allSettled(ctx.pending.splice(0));
  for (const [k, v] of Object.entries(legacy)) ctx.tests.push({ name: k, ok: !!v, error: v ? '' : tr('tests["%s"] dio %s', k, fmtV(v)) });
}

/* pm.sendRequest: through the same way the page sends (the board or the
 * browser); its callback runs before the script is over */
function sendFromScript(ctx, req, cb) {
  const p = (async () => {
    let item;
    if (typeof req === 'string') item = { method: 'GET', url: urlObject(req), header: [] };
    else {
      const hd = Array.isArray(req.header) ? req.header.map(x => typeof x === 'string' ? { key: x.split(':')[0].trim(), value: x.slice(x.indexOf(':') + 1).trim() } : x)
        : isObj(req.header) ? Object.entries(req.header).map(([key, value]) => ({ key, value: valStr(value) })) : [];
      item = { method: String(req.method || 'GET').toUpperCase(), url: urlObject(req.url && req.url.toString ? req.url.toString() : req.url), header: hd, body: req.body, auth: req.auth };
    }
    const sub = { req: item, scopes: ctx.scopes, console: ctx.console };
    const final = buildFinal(sub, [], item.auth || { type: 'noauth' });
    const resp = await sendFinal(final, { timeout: ctx.timeout || 30000, follow: true, token: {}, mode: ctx.mode });
    if (!resp.ok) throw new Error(resp.error || tr('sin respuesta'));
    return makeResponse(resp);
  })();
  const done = p.then(r => { if (cb) cb(null, r); return r; }, e => { if (cb) cb(e, undefined); else throw e; });
  ctx.pending.push(done.catch(e => ctx.errors.push({ where: 'pm.sendRequest', phase: '', error: errText(e) })));
  return done;
}

/* ---- building and sending a request ----------------------------------------------------
 * execute() is the whole pipeline, the same for a request sent by hand and
 * for each step of the runner: pre-request scripts (collection, folders,
 * request), variables, auth, headers and body, the sending, the tests in the
 * same order. Its caller merges the variables the scripts changed and keeps
 * the history. */

const LANG_CT = { json: 'application/json', xml: 'application/xml', html: 'text/html', javascript: 'application/javascript', text: 'text/plain' };
const formEnc = s => encodeURIComponent(s).replace(/%20/g, '+');

class NotOpenError extends Error {}

function failResp(error, ms, via) {
  return { ok: false, status: 0, reason: '', ms: Math.round(ms || 0), times: null, headers: [], size: 0, cut: false, binary: false,
    redirects: 0, finalUrl: '', text: '', bytes: null, error, via };
}

/* the request as it goes out: [[name, value]] headers, a text body, and
 * what was added by itself (auto) for the editor to show */
function buildFinal(ctx, chain, authOverride) {
  const rq = ctx.req;
  const look = lookupIn(ctx);
  const R = s => resolveText(s == null ? '' : String(s), look);
  const notes = [];
  let raw = urlRaw(rq.url);
  const pv = isObj(rq.url) && Array.isArray(rq.url.variable) ? rq.url.variable : [];
  if (pv.length) {
    const { base, query, hash } = splitUrl(raw);
    raw = base.replace(/(^|\/):([A-Za-z_][\w-]*)(?=\/|$)/g, (m, sl, k) => {
      const e = pv.find(x => x.key === k && !x.disabled);
      return e ? sl + (e.value ?? '') : m;
    }) + (query != null ? '?' + query : '') + (hash != null ? '#' + hash : '');
  }
  let url = R(raw).trim();
  if (url && !/^[a-z][\w+.-]*:\/\//i.test(url)) url = 'http://' + url;
  const headers = [];
  for (const e of rq.header || []) if (!e.disabled && e.key) headers.push([R(e.key), R(e.value)]);
  const has = k => headers.some(x => x[0].toLowerCase() === k.toLowerCase());
  const auto = [];
  const add = (k, v, why) => { headers.push([k, v]); auto.push([k, v, why]); };

  const { auth, from } = authOverride !== undefined ? { auth: authOverride, from: null } : effectiveAuth(rq.auth, chain);
  const ap = authParams(auth), t = auth && auth.type;
  const why = from ? tr('auth de «%s»', nameOf(from)) : tr('auth');
  if (t === 'bearer') { if (!has('Authorization')) add('Authorization', 'Bearer ' + R(ap.token), why); }
  else if (t === 'basic') { if (!has('Authorization')) add('Authorization', 'Basic ' + b64(R(ap.username) + ':' + R(ap.password)), why); }
  else if (t === 'apikey') {
    const k = R(ap.key || 'X-Api-Key'), v = R(ap.value);
    if (ap.in === 'query') {
      const hi = url.indexOf('#');
      const tail = hi >= 0 ? url.slice(hi) : '';
      url = (hi >= 0 ? url.slice(0, hi) : url);
      url += (url.includes('?') ? '&' : '?') + encodeURIComponent(k) + '=' + encodeURIComponent(v) + tail;
      auto.push([k, v, tr('auth, en la URL')]);
    } else if (!has(k)) add(k, v, why);
  } else if (t === 'oauth2' && ap.accessToken) {
    if (!has('Authorization')) add('Authorization', (ap.headerPrefix || 'Bearer') + ' ' + R(ap.accessToken), why);
  } else if (t && t !== 'noauth') notes.push(tr('La auth «%s» no está en Cartero: va sin ella', t));

  const b = rq.body || {};
  let body = '', ct = null;
  switch (b.mode) {
    case 'raw': {
      body = R(b.raw || '');
      const lang = (b.options && b.options.raw && b.options.raw.language) || 'text';
      ct = LANG_CT[lang] || 'text/plain';
      break;
    }
    case 'urlencoded':
      body = (b.urlencoded || []).filter(e => !e.disabled && e.key).map(e => formEnc(R(e.key)) + '=' + formEnc(R(e.value))).join('&');
      ct = 'application/x-www-form-urlencoded';
      break;
    case 'formdata': {
      const bd = '----CarteroForm' + uuid().replace(/-/g, '').slice(0, 16);
      let s = '';
      for (const e of b.formdata || []) {
        if (e.disabled || !e.key) continue;
        if (e.type === 'file') { notes.push(tr('«%s»: los archivos de form-data no van, sólo los textos', e.key)); continue; }
        s += `--${bd}\r\nContent-Disposition: form-data; name="${R(e.key).replace(/"/g, '%22')}"\r\n` +
          (e.contentType ? `Content-Type: ${e.contentType}\r\n` : '') + '\r\n' + R(e.value) + '\r\n';
      }
      body = s + `--${bd}--\r\n`;
      ct = 'multipart/form-data; boundary=' + bd;
      break;
    }
    case 'graphql': {
      const g = b.graphql || {};
      let vars = {};
      const vt = R(g.variables || '').trim();
      if (vt) { try { vars = JSON.parse(vt); } catch (e) { notes.push(tr('Las variables de GraphQL no son JSON: %s', e.message)); } }
      body = JSON.stringify({ query: R(g.query || ''), variables: vars });
      ct = 'application/json';
      break;
    }
    case 'file':
      notes.push(tr('Un archivo como cuerpo no está en Cartero: va sin cuerpo'));
      break;
  }
  if (ct && !has('Content-Type')) add('Content-Type', ct, tr('por el cuerpo'));
  if (!has('Accept')) add('Accept', '*/*', tr('de siempre'));
  return { method: String(rq.method || 'GET').toUpperCase(), url, headers, body, auto, notes };
}

function sendFinal(final, opt) {
  return opt.mode === 'board' ? viaBoard(final, opt) : sendDirect(final, opt);
}

/* straight from the browser: CORS decides, and the browser hides what the
 * server does not expose */
function corsText(url) {
  if (location.protocol === 'https:' && /^http:/i.test(url)) return tr('El navegador no deja pedir HTTP desde una página HTTPS. Mandalo vía la placa.');
  return tr('El navegador no pudo hacer el pedido: el servidor no lo permite desde otra página (CORS), no contesta, o su certificado no es válido. Vía la placa no hay CORS.');
}

function resTiming(url, t0) {
  try {
    let name = url;
    try { name = new URL(url).href; } catch {}
    const list = performance.getEntriesByType('resource').filter(e => e.startTime >= t0 - 5 && e.name === name);
    const e = list[list.length - 1];
    if (!e || !e.requestStart || !e.responseStart) return null;
    return { connect: Math.max(0, Math.round((e.connectEnd || e.requestStart) - (e.domainLookupStart || e.fetchStart))), send: 0,
      wait: Math.max(0, Math.round(e.responseStart - e.requestStart)), download: Math.max(0, Math.round(e.responseEnd - e.responseStart)) };
  } catch { return null; }
}

async function sendDirect(final, opt) {
  const ac = new AbortController();
  let why = '';
  const tm = setTimeout(() => { why = 'timeout'; ac.abort(); }, opt.timeout || 30000);
  opt.token.cancel = () => { why = 'cancel'; ac.abort(); };
  /* pairs and not a Headers object: that one sends the names in lower case */
  const hd = final.headers.filter(([k, v]) => { try { new Headers([[k, v]]); return true; } catch { return false; } });
  const noBody = final.method === 'GET' || final.method === 'HEAD';
  const t0 = performance.now();
  let r, buf;
  try {
    r = await fetch(final.url, { method: final.method, headers: hd, body: noBody || !final.body ? undefined : final.body,
      redirect: opt.follow === false ? 'manual' : 'follow', signal: ac.signal, cache: 'no-store', credentials: 'omit' });
    buf = new Uint8Array(await r.arrayBuffer());
  } catch (e) {
    clearTimeout(tm);
    const ms = performance.now() - t0;
    if (why === 'timeout') return failResp(tr('Se pasó el tiempo de espera (%s)', fmtMs(opt.timeout)), ms, 'direct');
    if (why === 'cancel') return failResp(tr('Cancelado'), ms, 'direct');
    if (r) return failResp(tr('Se cortó la respuesta: %s', e.message), ms, 'direct');
    return Object.assign(failResp(corsText(final.url), ms, 'direct'), { cors: true });
  }
  clearTimeout(tm);
  const ms = performance.now() - t0;
  if (r.type === 'opaqueredirect') return failResp(tr('El servidor redirige y el navegador no deja ver adónde: activá «Seguir redirecciones» o mandalo vía la placa'), ms, 'direct');
  const headers = [];
  r.headers.forEach((v, k) => headers.push([k, v]));
  const ct = r.headers.get('content-type') || '';
  const binary = looksBinary(ct, buf);
  return { ok: true, status: r.status, reason: r.statusText || REASON[r.status] || '', ms: Math.round(ms), times: resTiming(final.url, t0),
    headers, size: buf.length, cut: false, binary, redirects: r.redirected ? 1 : 0, finalUrl: r.redirected ? r.url : '',
    text: binary ? '' : decodeText(buf, ct), bytes: buf, via: 'direct', error: '' };
}

/* through the board: pedido.json, "send <n>" on the live channel, and the
 * live key "respuesta" with that n. One at a time. */
let boardN = Math.floor(Math.random() * 900000) + 1000;
let boardQueue = Promise.resolve();
function viaBoard(final, opt) {
  const p = boardQueue.then(() => sendBoard(final, opt));
  boardQueue = p.catch(() => {});
  return p;
}

async function boardOpen() {
  const r = await liveGet('estado');
  if (r.status === 404) return false;
  if (!r.ok) throw new Error(tr('La placa no contesta (%d)', r.status));
  return true;
}

async function sendBoard(final, opt) {
  if (!await boardOpen()) throw new NotOpenError(tr('Cartero no está abierta en la placa'));
  await ensureDirs();
  const n = ++boardN;
  const ask = { n, method: final.method, url: final.url, headers: final.headers, body: final.body || '',
    timeout: opt.timeout || 30000, follow: opt.follow !== false };
  const t0 = performance.now();
  await P.fsPut(F_ASK, JSON.stringify(ask));
  /* the answer to an earlier request is not ours: its version is noted first */
  let seen = null;
  try {
    const r0 = await liveGet('');
    if (r0.ok) { const e0 = (await r0.json()).find(x => x.key === 'respuesta'); if (e0) seen = e0.seq; }
  } catch {}
  await livePost('send ' + n);
  let cancelAt = 0;
  opt.token.cancel = () => { if (!cancelAt) { cancelAt = Date.now(); livePost('cancel').catch(() => {}); } };
  const limit = Date.now() + (opt.timeout || 30000) + 15000;
  let gone = 0;
  for (;;) {
    await sleep(120);
    let list = null;
    try { const r = await liveGet(''); if (r.ok) list = await r.json(); } catch {}
    if (Array.isArray(list)) {
      if (!list.some(x => x.key === 'estado')) { if (++gone > 10) return failResp(tr('La app Cartero se cerró en la placa'), performance.now() - t0, 'board'); } else gone = 0;
      const e = list.find(x => x.key === 'respuesta');
      if (e && e.seq !== seen) {
        seen = e.seq;
        const r = await liveGet('respuesta').catch(() => null);
        const j = r && r.ok ? await r.json().catch(() => null) : null;
        if (j && j.n === n) return fromBoard(j, performance.now() - t0);
      }
    }
    if (cancelAt && Date.now() - cancelAt > 4000) return failResp(tr('Cancelado'), performance.now() - t0, 'board');
    if (Date.now() > limit) return failResp(tr('La placa no contestó a tiempo'), performance.now() - t0, 'board');
  }
}

async function fromBoard(j, wall) {
  if (!j.ok) return failResp(j.error || tr('sin respuesta'), j.ms ?? wall, 'board');
  const headers = Array.isArray(j.headers) ? j.headers.filter(x => Array.isArray(x)) : [];
  const ct = hget(headers, 'content-type') || '';
  let text = typeof j.body === 'string' ? j.body : '', bytes = null, note = '';
  if (!text && j.size > 0) {
    try { bytes = new Uint8Array(await P.fsBytes(j.file || F_BIN)); } catch (e) { note = e.message; }
  } else if (text) bytes = new TextEncoder().encode(text);
  const binary = !!j.binary || (!!bytes && !text && looksBinary(ct, bytes));
  if (bytes && !text && !binary) text = decodeText(bytes, ct);
  return { ok: true, status: j.status, reason: j.reason || REASON[j.status] || '', ms: Math.round(j.ms ?? wall), times: j.times || null,
    headers, size: j.size ?? (bytes ? bytes.length : 0), cut: !!j.cut, binary, redirects: j.redirects || 0,
    finalUrl: j.final_url || '', text: binary ? '' : text, bytes, via: 'board', error: '', note };
}

/* the variable scopes of a run, from what the page has in memory */
function scopesFor(col, local) {
  const e = activeEnvEntry();
  const env = new VarScope(e ? nameOf(e.data) : '', e && e.data ? e.data.values : []);
  if (!e) env.dead = true;
  return {
    local: local || new Map(),
    env,
    coll: new VarScope(col ? nameOf(col) : '', col ? col.variable : []),
    glob: new VarScope('globals', S.glob && S.glob.values),
  };
}

const where = n => n.info ? tr('colección «%s»', nameOf(n)) : tr('carpeta «%s»', nameOf(n));

/* job: { col, path, item (normalized), local, iteration, iterations, mode,
 * timeout, follow, token, onSend } */
async function execute(job) {
  const chain = job.col && job.path ? chainOf(job.col, job.path).slice(0, -1) : [];
  const ctx = {
    id: uuid(), name: job.item.name || '', req: job.item.request, iteration: job.iteration || 0, iterations: job.iterations || 1,
    scopes: scopesFor(job.col, job.local), tests: [], errors: [], console: [], pending: [], next: undefined, skip: false,
    location: chain.map(nameOf).concat(job.item.name || ''), timeout: job.timeout, mode: job.mode, final: null, resp: null,
  };
  for (const n of chain) await runScript(ctx, 'prerequest', scriptOf(n, 'prerequest'), where(n));
  await runScript(ctx, 'prerequest', scriptOf(job.item, 'prerequest'), tr('el pedido'));
  if (ctx.skip) return ctx;
  ctx.final = buildFinal(ctx, chain);
  for (const n of ctx.final.notes) ctx.console.push({ level: 'warn', text: n, where: '' });
  if (job.mode === 'direct' && ctx.final.body && (ctx.final.method === 'GET' || ctx.final.method === 'HEAD'))
    ctx.console.push({ level: 'warn', text: tr('El navegador no manda cuerpo en un %s: va sin él', ctx.final.method), where: '' });
  if (!ctx.final.url) { ctx.resp = failResp(tr('Falta la URL'), 0, job.mode); return ctx; }
  if (job.onSend) job.onSend(ctx.final);
  ctx.resp = await sendFinal(ctx.final, { timeout: job.timeout, follow: job.follow, token: job.token || {}, mode: job.mode });
  if (ctx.resp.ok) {
    for (const n of chain) await runScript(ctx, 'test', scriptOf(n, 'test'), where(n));
    await runScript(ctx, 'test', scriptOf(job.item, 'test'), tr('el pedido'));
  }
  return ctx;
}

/* what the scripts changed, into the memory and the files read again */
async function mergeVarsFile(path, ops, env, key, fallback) {
  let d = await readJSON(path);
  if (!d) d = clone(fallback());
  if (!Array.isArray(d[key])) d[key] = [];
  applyOps(d[key], ops, env);
  await writeJSON(path, d);
  return d;
}

async function flushVars(ctx, colFile) {
  const { env, glob, coll } = ctx.scopes;
  const jobs = [];
  const e = activeEnvEntry();
  if (env.ops.size && e) {
    applyOps(e.data.values || (e.data.values = []), env.ops, true);
    jobs.push(mergeVarsFile(D_ENVS + '/' + e.file, env.ops, true, 'values', () => e.data).then(d => { e.data = d; }));
  }
  if (glob.ops.size) {
    if (!S.glob) S.glob = { name: 'Globales', values: [], _postman_variable_scope: 'globals' };
    applyOps(S.glob.values || (S.glob.values = []), glob.ops, true);
    jobs.push(mergeVarsFile(F_GLOB, glob.ops, true, 'values', () => S.glob).then(d => { S.glob = d; }));
  }
  const c = colFile && colEntry(colFile);
  if (coll.ops.size && c && c.data) {
    applyOps(c.data.variable || (c.data.variable = []), coll.ops, false);
    jobs.push(mergeVarsFile(D_COLS + '/' + c.file, coll.ops, false, 'variable', () => c.data).then(d => { c.data = d; }));
  }
  if (!jobs.length) return false;
  await ensureDirs();
  const res = await Promise.allSettled(jobs);
  const bad = res.find(r => r.status === 'rejected');
  if (bad) toast(tr('No se pudieron guardar las variables: %s', bad.reason && bad.reason.message), true);
  tellBoard();
  varsChanged();
  return true;
}

function histRequest(rq) {
  const c = cleanItem({ request: rq }).request;
  c.url = urlRaw(c.url);
  if (!c.header) c.header = [];
  if (!c.body) c.body = {};
  return c;
}

async function addHistory(item, ctx) {
  if (!ctx.final) return;
  const r = ctx.resp || {};
  const e = { t: Math.floor(Date.now() / 1000), name: item.name || '', method: ctx.final.method, url: ctx.final.url,
    status: r.ok ? r.status : 0, ms: Math.round(r.ms || 0), size: r.size || 0, request: histRequest(item.request) };
  try {
    await ensureDirs();
    let list = null;
    try { list = await readJSON(F_HIST); } catch {}
    if (!Array.isArray(list)) list = [];
    list.unshift(e);
    if (list.length > HIST_MAX) list.length = HIST_MAX;
    await writeJSON(F_HIST, list);
    S.hist = list;
  } catch (err) {
    S.hist = [e].concat(S.hist || []).slice(0, HIST_MAX);
  }
  if (S.side === 'hist') drawSide();
}

/* ---- cURL ------------------------------------------------------------------------------
 * A command as a shell would split it (quotes, $'...', backslashes and line
 * continuations), then curl's options that make a request. */

function shellWords(s) {
  s = s.replace(/\\\r?\n/g, ' ').replace(/\^\r?\n/g, ' ');
  const out = [];
  let cur = null, i = 0;
  const ESC = { n: '\n', t: '\t', r: '\r', '\\': '\\', "'": "'", '"': '"', a: '\x07', b: '\b', e: '\x1b', f: '\f', v: '\v', 0: '\0' };
  while (i < s.length) {
    const c = s[i];
    if (/\s/.test(c)) { if (cur !== null) { out.push(cur); cur = null; } i++; continue; }
    if (cur === null) cur = '';
    if (c === "'") {
      const j = s.indexOf("'", i + 1);
      cur += s.slice(i + 1, j < 0 ? s.length : j);
      i = j < 0 ? s.length : j + 1;
    } else if (c === '$' && s[i + 1] === "'") {
      i += 2;
      while (i < s.length && s[i] !== "'") {
        if (s[i] === '\\' && i + 1 < s.length) {
          const n = s[i + 1];
          if (n === 'x' && /^[0-9a-fA-F]{1,2}/.test(s.slice(i + 2))) { const m = /^[0-9a-fA-F]{1,2}/.exec(s.slice(i + 2))[0]; cur += String.fromCharCode(parseInt(m, 16)); i += 2 + m.length; continue; }
          if (n === 'u' && /^[0-9a-fA-F]{4}/.test(s.slice(i + 2))) { cur += String.fromCharCode(parseInt(s.slice(i + 2, i + 6), 16)); i += 6; continue; }
          cur += ESC[n] ?? '\\' + n;
          i += 2;
        } else cur += s[i++];
      }
      i++;
    } else if (c === '"') {
      i++;
      while (i < s.length && s[i] !== '"') {
        if (s[i] === '\\' && i + 1 < s.length && '"\\$`\n'.includes(s[i + 1])) { cur += s[i + 1]; i += 2; } else cur += s[i++];
      }
      i++;
    } else if (c === '\\' && i + 1 < s.length) {
      cur += s[i + 1];
      i += 2;
    } else {
      cur += c;
      i++;
    }
  }
  if (cur !== null) out.push(cur);
  return out;
}

const CURL_ARG = new Set(['-o', '--output', '-m', '--max-time', '--connect-timeout', '-w', '--write-out', '-x', '--proxy', '--retry',
  '--retry-delay', '--resolve', '--cacert', '--cert', '--key', '-E', '--limit-rate', '-c', '--cookie-jar', '-r', '--range',
  '--max-redirs', '--interface', '--dns-servers', '-T', '--upload-file', '-K', '--config', '-U', '--proxy-user', '--ciphers', '--tls-max']);

function parseCurl(text) {
  const w = shellWords(text.trim());
  if (!w.length || !/^curl(\.exe)?$/i.test(w[0])) throw new Error(tr('No empieza con curl'));
  let method = null, url = null, user = null, get = false, json = false, bearer = null;
  const headers = [], data = [], form = [];
  for (let i = 1; i < w.length; i++) {
    let a = w[i], v;
    const take = () => { i++; return w[i] ?? ''; };
    if (/^--[a-z-]+=/.test(a)) { const e = a.indexOf('='); v = a.slice(e + 1); a = a.slice(0, e); }
    if (/^-[A-Za-z]/.test(a) && a.length > 2) {
      if ('XHdubFAeo'.includes(a[1])) { v = a.slice(2); a = a.slice(0, 2); }
      else { if (a.includes('I')) method = 'HEAD'; if (a.includes('G')) get = true; continue; }
    }
    const val = () => v ?? take();
    switch (a) {
      case '-X': case '--request': method = val().toUpperCase(); break;
      case '-H': case '--header': {
        const hv = val(), k = hv.indexOf(':');
        if (k > 0) headers.push({ key: hv.slice(0, k).trim(), value: hv.slice(k + 1).trim() });
        break;
      }
      case '-d': case '--data': case '--data-ascii': data.push(val().replace(/[\r\n]/g, '')); break;
      case '--data-binary': case '--data-raw': data.push(val()); break;
      case '--data-urlencode': {
        const x = val(), e = x.indexOf('=');
        data.push(e > 0 ? x.slice(0, e + 1) + encodeURIComponent(x.slice(e + 1)) : encodeURIComponent(e === 0 ? x.slice(1) : x));
        break;
      }
      case '--json': json = true; data.push(val()); break;
      case '-u': case '--user': user = val(); break;
      case '-F': case '--form': case '--form-string': form.push(val()); break;
      case '-A': case '--user-agent': headers.push({ key: 'User-Agent', value: val() }); break;
      case '-b': case '--cookie': headers.push({ key: 'Cookie', value: val() }); break;
      case '-e': case '--referer': headers.push({ key: 'Referer', value: val() }); break;
      case '--oauth2-bearer': bearer = val(); break;
      case '-G': case '--get': get = true; break;
      case '-I': case '--head': method = 'HEAD'; break;
      case '--url': url = val(); break;
      default:
        if (CURL_ARG.has(a)) { val(); break; }
        if (!a.startsWith('-') && url == null) url = a;
    }
  }
  if (!url) throw new Error(tr('El comando no tiene URL'));
  const hasH = k => headers.some(x => x.key.toLowerCase() === k);
  const ctH = headers.find(x => x.key.toLowerCase() === 'content-type');
  let body;
  if (form.length) {
    body = { mode: 'formdata', formdata: form.map(f => {
      const e = f.indexOf('=');
      const key = e >= 0 ? f.slice(0, e) : f, value = e >= 0 ? f.slice(e + 1) : '';
      return value[0] === '@' ? { key, type: 'file', src: value.slice(1) } : { key, value: value.replace(/^"(.*)"$/, '$1'), type: 'text' };
    }) };
    if (ctH && /multipart/i.test(ctH.value)) headers.splice(headers.indexOf(ctH), 1);
  } else if (data.length && get) {
    url += (url.includes('?') ? '&' : '?') + data.join('&');
  } else if (data.length) {
    const d = json ? data.join('') : data.join('&');
    const ct = ctH ? ctH.value.toLowerCase() : '';
    const jsonish = /^\s*[[{]/.test(d) && (() => { try { JSON.parse(d); return true; } catch { return false; } })();
    if (json) {
      if (!hasH('content-type')) headers.push({ key: 'Content-Type', value: 'application/json' });
      if (!hasH('accept')) headers.push({ key: 'Accept', value: 'application/json' });
      body = { mode: 'raw', raw: d, options: { raw: { language: 'json' } } };
    } else if ((!ct || ct.includes('x-www-form-urlencoded')) && !jsonish && /^[^=&\s]+=/.test(d)) {
      const dec = x => { try { return decodeURIComponent(x.replace(/\+/g, ' ')); } catch { return x; } };
      body = { mode: 'urlencoded', urlencoded: d.split('&').filter(Boolean).map(p => {
        const e = p.indexOf('=');
        return { key: dec(e >= 0 ? p.slice(0, e) : p), value: dec(e >= 0 ? p.slice(e + 1) : ''), type: 'text' };
      }) };
      if (ctH) headers.splice(headers.indexOf(ctH), 1);
    } else {
      const lang = /json/.test(ct) || (!ct && jsonish) ? 'json' : /xml/.test(ct) ? 'xml' : /html/.test(ct) ? 'html' : /javascript/.test(ct) ? 'javascript' : 'text';
      body = { mode: 'raw', raw: d, options: { raw: { language: lang } } };
      if (!ct && jsonish) headers.push({ key: 'Content-Type', value: 'application/json' });
    }
  }
  if (!method) method = body ? 'POST' : 'GET';
  const request = { method, header: headers, url: urlObject(url) };
  if (body) request.body = body;
  if (user != null) {
    const e = user.indexOf(':');
    request.auth = { type: 'basic', basic: [{ key: 'username', value: e >= 0 ? user.slice(0, e) : user, type: 'string' }, { key: 'password', value: e >= 0 ? user.slice(e + 1) : '', type: 'string' }] };
  } else if (bearer != null) {
    request.auth = { type: 'bearer', bearer: [{ key: 'token', value: bearer, type: 'string' }] };
  }
  const seg = splitUrl(url).base.replace(/^[a-z]+:\/\//i, '').split('/').filter(Boolean);
  return { name: method + ' ' + (seg.length > 1 ? '/' + seg.slice(1).join('/') : seg[0] || url), request, response: [] };
}

/* the request that was sent, as a command to paste */
function toCurl(final) {
  const q = s => "'" + String(s).replace(/'/g, "'\\''") + "'";
  let s = 'curl';
  if (final.method === 'HEAD') s += ' -I';
  else if (final.method !== 'GET' && !(final.method === 'POST' && final.body)) s += ' -X ' + final.method;
  s += ' ' + q(final.url);
  for (const [k, v] of final.headers) s += ' \\\n  -H ' + q(k + ': ' + v);
  if (final.body && final.method !== 'GET' && final.method !== 'HEAD') s += ' \\\n  --data-raw ' + q(final.body);
  return s;
}

/* ---- JSON and XML -----------------------------------------------------------------------
 * A raw JSON body may carry {{vars}} where a value goes ({"n": {{n}}}): it is
 * checked with them as 0, and formatted by a tokenizer that leaves them as
 * they are. */

function jsonVarsSafe(text) {
  let out = '', inStr = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (inStr) {
      out += c;
      if (c === '\\') { out += text[i + 1] ?? ''; i++; } else if (c === '"') inStr = false;
      continue;
    }
    if (c === '"') { inStr = true; out += c; continue; }
    if (c === '{' && text[i + 1] === '{') {
      const j = text.indexOf('}}', i + 2);
      if (j > 0) { out += '0'; i = j + 1; continue; }
    }
    out += c;
  }
  return out;
}

function jsonCheck(text) {
  if (!text.trim()) return null;
  try { JSON.parse(jsonVarsSafe(text)); return null; } catch (e) { return e.message; }
}

function jsonFormat(text, ind = '  ') {
  if (jsonCheck(text)) throw new Error(jsonCheck(text));
  let out = '', d = 0, i = 0;
  const nl = () => '\n' + ind.repeat(d);
  while (i < text.length) {
    const c = text[i];
    if (c === '"') {
      let j = i + 1;
      while (j < text.length && text[j] !== '"') j += text[j] === '\\' ? 2 : 1;
      out += text.slice(i, j + 1);
      i = j + 1;
      continue;
    }
    if (c === '{' && text[i + 1] === '{') {
      const j = text.indexOf('}}', i + 2);
      out += text.slice(i, j + 2);
      i = j + 2;
      continue;
    }
    if (/\s/.test(c)) { i++; continue; }
    if (c === '{' || c === '[') {
      const close = c === '{' ? '}' : ']';
      let j = i + 1;
      while (j < text.length && /\s/.test(text[j])) j++;
      if (text[j] === close) { out += c + close; i = j + 1; continue; }
      d++;
      out += c + nl();
    } else if (c === '}' || c === ']') {
      d--;
      out += nl() + c;
    } else if (c === ',') {
      out += ',' + nl();
    } else if (c === ':') {
      out += ': ';
    } else out += c;
    i++;
  }
  return out;
}

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr', '!doctype']);

/* XML and HTML indented, near enough to read */
function xmlPretty(s) {
  if (s.length > 1500000) return s;
  const toks = s.split(/(<!--[\s\S]*?-->|<!\[CDATA\[[\s\S]*?\]\]>|<[^>]+>)/).filter(t => t.trim() !== '');
  const out = [];
  let d = 0;
  const pad = () => '  '.repeat(Math.max(0, d));
  const tagName = t => (/^<\/?\s*([^\s/>]+)/.exec(t) || [])[1] || '';
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i].trim();
    if (t[0] !== '<' || t.startsWith('<!--') || t.startsWith('<![CDATA[')) { out.push(pad() + t); continue; }
    if (t.startsWith('</')) { d--; out.push(pad() + t); continue; }
    const name = tagName(t).toLowerCase();
    const selfish = t.endsWith('/>') || t.startsWith('<?') || t.startsWith('<!') || VOID.has(name);
    const next = toks[i + 1], after = toks[i + 2];
    if (!selfish && next && next.trim()[0] !== '<' && after && after.trim().startsWith('</') && tagName(after.trim()).toLowerCase() === name) {
      out.push(pad() + t + next.trim() + after.trim());
      i += 2;
      continue;
    }
    out.push(pad() + t);
    if (!selfish) d++;
  }
  return out.join('\n');
}

/* the path of a value for a script: pm.response.json().data[0].id */
function jsPath(path) {
  let s = 'pm.response.json()';
  for (const k of path) s += typeof k === 'number' ? `[${k}]` : /^[A-Za-z_$][\w$]*$/.test(k) ? '.' + k : `[${JSON.stringify(k)}]`;
  return s;
}

/* ---- the page's state -----------------------------------------------------------------
 * One object, made again each time the page is drawn (render). The
 * collections and environments as they were read: { file, mtime, size,
 * data, err }. cur is what the editor has open (a request, a folder or a
 * collection: a copy, written back on saving); envEd the environment being
 * edited; res the last answer; run the runner's current or last run. */

let S = null;

const colEntry = file => S.cols.find(c => c.file === file);
const envEntry = file => S.envs.find(e => e.file === file);
const slugOf = file => file.replace(/\.json$/i, '');
function activeEnvEntry() {
  if (!S || !S.active) return null;
  return S.envs.find(e => slugOf(e.file) === S.active && e.data) || null;
}
const samePath = (a, b) => !!a && !!b && a.length === b.length && a.every((x, i) => x === b[i]);
const sortCols = () => S.cols.sort((a, b) => nameOf(a.data || {}).localeCompare(nameOf(b.data || {})) || a.file.localeCompare(b.file));
const sortEnvs = () => S.envs.sort((a, b) => nameOf(a.data || {}).localeCompare(nameOf(b.data || {})) || a.file.localeCompare(b.file));
const narrow = () => S.dom.root && S.dom.root.clientWidth <= 820;
const reqMethod = n => n && n.request ? (typeof n.request === 'string' ? 'GET' : String(n.request.method || 'GET').toUpperCase()) : 'GET';
const descText = d => typeof d === 'string' ? d : d && typeof d.content === 'string' ? d.content : '';

/* an action that may fail: its message as a toast */
function guard(fn) {
  return async (...a) => {
    try { return await fn(...a); } catch (e) { toast(errText(e), true); console.error(e); }
  };
}

/* a {{variable}} as the editor sees it: the active environment, the open
 * collection's, the globals (the script's own are not known until it runs) */
function varInfo(k) {
  if (k[0] === '$') return DYN.includes(k) ? { dyn: true, show: '{{' + k + '}}', scope: tr('dinámica') } : null;
  const e = activeEnvEntry();
  const find = list => (list || []).find(x => x && x.key === k && enabledEntry(x));
  let f = e && e.data && find(e.data.values);
  if (f) return { show: f.type === 'secret' ? '••••••' : valStr(f.value), scope: tr('entorno «%s»', nameOf(e.data)) };
  const c = S.cur && S.cur.file ? colEntry(S.cur.file) : null;
  const vars = S.cur && S.cur.kind === 'collection' ? S.cur.item.variable : c && c.data && c.data.variable;
  f = find(vars);
  if (f) return { show: valStr(f.value), scope: tr('colección') };
  f = S.glob && find(S.glob.values);
  if (f) return { show: f.type === 'secret' ? '••••••' : valStr(f.value), scope: tr('globales') };
  return null;
}

/* ---- widgets ------------------------------------------------------------------------- */

const methodBadge = m => h('span', { class: 'ctm ctm-' + m }, SHORT[m] || m);

function statusPill(code, reason) {
  return h('span', { class: 'ctst s' + (code ? Math.floor(code / 100) : 0) }, code ? code + (reason ? ' ' + reason : '') : tr('Error'));
}

function tabs(list, cur, onPick) {
  return h('div', { class: 'cttabs', role: 'tablist' }, list.filter(Boolean).map(t =>
    h('button', { class: t.id === cur ? 'on' : null, role: 'tab', onclick: () => onPick(t.id) }, t.label,
      t.count ? h('span', { class: 'ctcnt' }, String(t.count)) : null,
      t.dot ? h('span', { class: 'ctdot' }) : null)));
}

function debounce(fn, ms) {
  let t = 0;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
}

/* a key/value table, Postman's: enabled, key, value (and a description),
 * with an empty row at the end that becomes a real one when typed into.
 * Rows are changed in place; opts.env uses "enabled" instead of
 * "disabled", opts.secret shows secrets as dots with an eye. */
function kvTable(rows, opts = {}) {
  const box = h('div', {});
  const envStyle = !!opts.env;
  const isOn = r => envStyle ? r.enabled !== false : !r.disabled;
  const setOn = (r, on) => { if (envStyle) r.enabled = on; else if (on) delete r.disabled; else r.disabled = true; };
  const shown = new Set();
  let focusAfter = null;
  const changed = () => { if (opts.onChange) opts.onChange(); };
  const draw = () => {
    const g = h('div', { class: 'ctkv' + (opts.desc ? ' desc' : '') });
    g.append(h('div'), h('div', { class: 'ctkvh' }, tr(opts.keyLabel || N_('Clave'))), h('div', { class: 'ctkvh' }, tr(opts.valLabel || N_('Valor'))),
      opts.desc ? h('div', { class: 'ctkvh ctkvd' }, tr('Descripción')) : '', h('div'));
    rows.forEach((r, i) => {
      const off = !isOn(r);
      const secret = opts.secret && r.type === 'secret' && !shown.has(i);
      const file = r.type === 'file';
      g.append(
        h('input', { type: 'checkbox', checked: !off, title: tr('Activado'), onchange: e => { setOn(r, e.target.checked); changed(); draw(); } }),
        h('input', { type: 'text', value: r.key ?? '', class: off ? 'off' : null, spellcheck: 'false', autocomplete: 'off', 'data-r': i, 'data-f': 'key',
          oninput: e => { r.key = e.target.value; changed(); } }),
        file ? h('input', { type: 'text', value: tr('archivo: %s', valStr(r.src)), disabled: true, title: tr('Los archivos de form-data no se mandan desde Cartero') })
          : h('input', { type: secret ? 'password' : 'text', value: valStr(r.value), class: off ? 'off' : null, spellcheck: 'false', autocomplete: secret ? 'new-password' : 'off',
            'data-r': i, 'data-f': 'value', oninput: e => { r.value = e.target.value; changed(); } }),
        opts.desc ? h('input', { type: 'text', class: 'ctkvd' + (off ? ' off' : ''), value: descText(r.description), 'data-r': i, 'data-f': 'description',
          oninput: e => { if (e.target.value) r.description = e.target.value; else delete r.description; changed(); } }) : '',
        h('div', { class: 'ctkva' },
          opts.secret ? h('button', { class: r.type === 'secret' ? 'on' : null, title: tr('Secreto: se ve como puntos'),
            onclick: () => { r.type = r.type === 'secret' ? 'default' : 'secret'; shown.delete(i); changed(); draw(); } }, '🔒') : null,
          opts.secret && r.type === 'secret' ? h('button', { title: tr('Mostrar u ocultar'), onclick: () => { shown.has(i) ? shown.delete(i) : shown.add(i); draw(); } }, shown.has(i) ? '◡' : '👁') : null,
          h('button', { title: tr('Sacar'), onclick: () => { rows.splice(i, 1); shown.clear(); changed(); draw(); } }, '✕')));
    });
    if (!opts.readonly) {
      const add = f => e => {
        const r = envStyle ? { key: '', value: '', type: 'default', enabled: true } : Object.assign({ key: '', value: '' }, opts.newRow ? opts.newRow() : {});
        r[f] = e.target.value;
        rows.push(r);
        focusAfter = { i: rows.length - 1, f, pos: e.target.selectionStart };
        changed();
        draw();
      };
      g.append(h('div', { class: 'ctghost' }),
        h('input', { type: 'text', placeholder: tr(opts.keyPh || N_('Clave nueva')), spellcheck: 'false', autocomplete: 'off', oninput: add('key') }),
        h('input', { type: 'text', placeholder: tr(opts.valPh || N_('Valor')), spellcheck: 'false', autocomplete: 'off', oninput: add('value') }),
        opts.desc ? h('input', { type: 'text', class: 'ctkvd', placeholder: tr('Descripción'), oninput: add('description') }) : '',
        h('div'));
    }
    put(box, g);
    if (focusAfter) {
      const el = g.querySelector(`[data-r="${focusAfter.i}"][data-f="${focusAfter.f}"]`);
      if (el) { el.focus(); try { el.setSelectionRange(focusAfter.pos, focusAfter.pos); } catch {} }
      focusAfter = null;
    }
  };
  draw();
  box.redraw = draw;
  return box;
}

/* a textarea for code: Tab puts two spaces (or indents the lines chosen),
 * Shift+Tab takes them out, Enter keeps the indentation */
function codeArea(value, onInput, opts = {}) {
  const ta = h('textarea', { class: 'ctcode', spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off', placeholder: opts.placeholder || '',
    style: opts.minH ? `min-height:${opts.minH}px` : null });
  ta.value = value || '';
  const ins = text => {
    ta.focus();
    let ok = false;
    try { ok = document.execCommand('insertText', false, text); } catch {}
    if (!ok) { ta.setRangeText(text, ta.selectionStart, ta.selectionEnd, 'end'); onInput(ta.value); }
  };
  ta.addEventListener('input', () => onInput(ta.value));
  ta.addEventListener('keydown', e => {
    if (e.key === 'Tab' && !e.ctrlKey && !e.metaKey && !e.altKey) {
      e.preventDefault();
      const v = ta.value, s = ta.selectionStart, en = ta.selectionEnd;
      if ((s !== en && v.slice(s, en).includes('\n')) || e.shiftKey) {
        const ls = v.lastIndexOf('\n', s - 1) + 1;
        const block = v.slice(ls, en);
        const nb = e.shiftKey ? block.replace(/^ {1,2}/gm, '') : block.replace(/^/gm, '  ');
        ta.setSelectionRange(ls, en);
        ins(nb);
        ta.setSelectionRange(ls, ls + nb.length);
      } else ins('  ');
    } else if (e.key === 'Enter' && !e.ctrlKey && !e.metaKey && !e.shiftKey && !e.altKey && !e.isComposing) {
      const v = ta.value, s = ta.selectionStart;
      const ls = v.lastIndexOf('\n', s - 1) + 1;
      const line = v.slice(ls, s);
      let ind = /^[ \t]*/.exec(line)[0];
      const last = line.trimEnd().slice(-1);
      if (last && '{[('.includes(last)) ind += '  ';
      e.preventDefault();
      ins('\n' + ind);
    }
  });
  ta.insert = text => {
    const v = ta.value;
    if (document.activeElement !== ta) { ta.focus(); ta.setSelectionRange(v.length, v.length); }
    const s = ta.selectionStart;
    const before = ta.value.slice(0, s);
    ins((before && !before.endsWith('\n') ? '\n' : '') + text + '\n');
  };
  return ta;
}

/* the URL, with its {{variables}} coloured by a copy of the text behind a
 * transparent input, and the resolved address under it */
function urlField(u, onChange) {
  const back = h('div', { class: 'cturlback', 'aria-hidden': 'true' });
  const inp = h('input', { type: 'text', spellcheck: 'false', autocomplete: 'off', autocapitalize: 'off', value: u.raw || '',
    placeholder: tr('https://api.ejemplo.com/recurso, o {{base}}/recurso'), 'aria-label': tr('URL') });
  const prev = h('div', { class: 'ctprev mono', style: 'display:none' });
  const sync = () => { back.scrollLeft = inp.scrollLeft; };
  const paint = () => {
    const text = inp.value;
    const kids = [];
    let last = 0;
    const found = [...text.matchAll(VAR_RE)];
    for (const m of found) {
      kids.push(text.slice(last, m.index));
      const info = varInfo(m[1]);
      kids.push(h('span', { class: info ? (info.dyn ? 'ctv-dyn' : 'ctv-ok') : 'ctv-bad' }, m[0]));
      last = m.index + m[0].length;
    }
    kids.push(text.slice(last) + ' ');
    put(back, ...kids);
    requestAnimationFrame(sync);
    if (!found.length) { prev.style.display = 'none'; inp.title = ''; return; }
    prev.style.display = '';
    const pk = [];
    last = 0;
    for (const m of found) {
      pk.push(text.slice(last, m.index));
      const info = varInfo(m[1]);
      pk.push(info ? h('span', { class: info.dyn ? 'ctv-dyn' : 'ctv-ok', title: '{{' + m[1] + '}} · ' + info.scope }, info.dyn ? m[0] : info.show)
        : h('span', { class: 'ctv-bad', title: tr('Variable desconocida: no está en el entorno, la colección ni las globales') }, m[0]));
      last = m.index + m[0].length;
    }
    pk.push(text.slice(last));
    put(prev, '→ ', ...pk);
    inp.title = found.map(m => { const i = varInfo(m[1]); return m[1] + ' = ' + (i ? i.show : '?'); }).join('\n');
  };
  inp.addEventListener('input', () => { onChange(inp.value); paint(); });
  for (const ev of ['scroll', 'keyup', 'keydown', 'click', 'select', 'focus', 'blur', 'mouseup']) inp.addEventListener(ev, () => requestAnimationFrame(sync));
  paint();
  return { el: h('div', { class: 'cturl' }, back, inp), prev, paint, input: inp, set(v) { if (inp.value !== v) { inp.value = v; paint(); } } };
}

/* a small menu by a button */
let menuEl = null;
function closeMenu() {
  if (!menuEl) return;
  menuEl.remove();
  menuEl = null;
  document.removeEventListener('mousedown', menuOutside, true);
  document.removeEventListener('scroll', closeMenu, true);
  window.removeEventListener('resize', closeMenu);
}
function menuOutside(e) { if (menuEl && !menuEl.contains(e.target)) closeMenu(); }
function openMenu(anchor, items) {
  closeMenu();
  const m = h('div', { class: 'ctmenu', role: 'menu' }, items.filter(Boolean).map(it => it === '-' ? h('hr')
    : h('button', { class: it.bad ? 'bad' : null, role: 'menuitem', onclick: () => { closeMenu(); it.do(); } }, it.label)));
  document.body.append(m);
  const r = anchor.getBoundingClientRect();
  const mw = m.offsetWidth, mh = m.offsetHeight;
  let y = r.bottom + 4;
  if (y + mh > innerHeight - 8) y = Math.max(8, r.top - mh - 4);
  m.style.left = Math.max(8, Math.min(r.right - mw, innerWidth - mw - 8)) + 'px';
  m.style.top = y + 'px';
  menuEl = m;
  setTimeout(() => {
    document.addEventListener('mousedown', menuOutside, true);
    document.addEventListener('scroll', closeMenu, true);
    window.addEventListener('resize', closeMenu);
  });
}

/* a dialog: onOk returns false to stay open */
function modal(title, body, okLabel, onOk) {
  return new Promise(res => {
    let el;
    const key = e => { if (e.key === 'Escape') { e.stopPropagation(); close(null); } };
    const close = v => { el.remove(); document.removeEventListener('keydown', key, true); res(v); };
    const ok = h('button', { class: 'btn pri', onclick: async () => {
      ok.disabled = true;
      let v;
      try { v = await onOk(); } catch (e) { toast(errText(e), true); v = false; }
      ok.disabled = false;
      if (v !== false) close(v === undefined ? true : v);
    } }, okLabel);
    el = h('div', { class: 'ctmodal', onmousedown: e => { if (e.target === el) close(null); } },
      h('div', { role: 'dialog' }, h('h3', {}, title), body,
        h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => close(null) }, tr('Cancelar')), ok)));
    document.body.append(el);
    document.addEventListener('keydown', key, true);
    const f = el.querySelector('input,select,textarea');
    if (f) f.focus();
  });
}

/* ---- the sidebar ---------------------------------------------------------------------
 * Four lists: the collections as a tree, the history, the environments and
 * the runner's results. */

const SIDE_TABS = [['col', N_('Colecciones')], ['hist', N_('Historial')], ['env', N_('Entornos')], ['runs', N_('Corridas')]];

function drawSide() {
  const d = S.dom;
  put(d.sideTabs, tabs(SIDE_TABS.map(([id, label]) => ({ id, label: tr(label) })), S.side, id => {
    S.side = id;
    S.sideFold = false;
    lsSet('side', id);
    S.dom.sideFor = null;
    drawSide();
  }));
  d.side.classList.toggle('fold', !!S.sideFold);
  d.sideToggle.textContent = S.sideFold ? '☰ ' + tr('Mostrar') : '✕ ' + tr('Ocultar');
  if (d.sideFor !== S.side) { d.sideFor = S.side; drawSideBar(); }
  drawSideBody();
}

function drawSideBar() {
  const bar = S.dom.sideBar;
  if (S.side === 'col') {
    const f = h('input', { type: 'search', placeholder: tr('Buscar'), value: S.filter, oninput: e => { S.filter = e.target.value; drawSideBody(); } });
    put(bar, f,
      h('button', { class: 'btn ctsmall', title: tr('Colección nueva'), onclick: guard(newCollection) }, '+ ' + tr('Colección')),
      h('button', { class: 'btn ctsmall', title: tr('Un pedido suelto, sin colección'), onclick: () => openScratch(newRequest(tr('Pedido nuevo'))) }, '+ ' + tr('Pedido')));
  } else if (S.side === 'hist') {
    put(bar, h('span', { class: 'muted small', style: 'flex:1' }, tr('Los últimos %d', HIST_MAX)),
      h('button', { class: 'btn ctsmall', onclick: guard(clearHistory) }, tr('Borrar el historial')));
  } else if (S.side === 'env') {
    put(bar, h('span', { style: 'flex:1' }), h('button', { class: 'btn ctsmall', onclick: guard(newEnv) }, '+ ' + tr('Entorno')));
  } else {
    put(bar, h('span', { style: 'flex:1' }), h('button', { class: 'btn ctsmall', onclick: () => openRunner() }, tr('Correr una colección')));
  }
}

function drawSideBody() {
  const body = S.dom.sideBody;
  if (!S.loaded) { put(body, h('div', { class: 'ctempty' }, tr('Leyendo la tarjeta...'))); return; }
  if (S.side === 'col') put(body, colTree());
  else if (S.side === 'hist') put(body, histList());
  else if (S.side === 'env') put(body, envList());
  else put(body, runList());
}

function colTree() {
  if (!S.cols.length) return h('div', { class: 'ctempty' }, tr('Ninguna colección todavía. Creá una o importá una de Postman.'));
  const q = S.filter.trim().toLowerCase();
  const match = n => nameOf(n).toLowerCase().includes(q) || (!!n.request && urlRaw(typeof n.request === 'string' ? n.request : n.request.url).toLowerCase().includes(q));
  const anyMatch = n => !q || match(n) || (n.item || []).some(anyMatch);
  const out = [];
  for (const c of S.cols) {
    if (c.err || !c.data) {
      out.push(h('div', { class: 'ctnode', title: c.err }, h('span', { class: 'ctcar' }, '⚠'), h('span', { class: 'ctnm' }, c.file), h('span', { class: 'ctsub' }, tr('no se pudo leer'))));
      continue;
    }
    if (!anyMatch(c.data)) continue;
    const row = (node, path, depth) => {
      const key = c.file + ':' + path.join('.');
      const folder = path.length === 0 || isFolder(node);
      const open = q ? true : S.open.has(key);
      const sel = S.cur && S.cur.file === c.file && samePath(S.cur.path, path) && S.view === 'req';
      const more = h('button', { class: 'ctmore', title: tr('Más'), 'aria-label': tr('Más'), onclick: e => { e.stopPropagation(); nodeMenu(e.currentTarget, c.file, path); } }, '⋯');
      const m = folder ? '' : reqMethod(node);
      out.push(h('div', { class: 'ctnode' + (sel ? ' sel' : ''), style: `padding-left:${4 + depth * 14}px;${path.length ? '' : 'font-weight:600'}`,
        title: folder ? '' : urlRaw(typeof node.request === 'string' ? node.request : node.request.url),
        onclick: () => folder ? toggleOpen(key) : openItem(c.file, path),
        ondblclick: folder ? () => openContainer(c.file, path) : null },
        folder ? h('span', { class: 'ctcar' }, open ? '▾' : '▸') : methodBadge(m),
        h('span', { class: 'ctnm' }, nameOf(node) || tr('(sin nombre)')),
        more));
      if (folder && open) {
        const kids = node.item || [];
        kids.forEach((it, i) => { if (anyMatch(it)) row(it, path.concat(i), depth + 1); });
        if (!kids.length && !q) out.push(h('div', { class: 'ctempty', style: `padding:2px 0 4px ${30 + depth * 14}px` }, tr('vacía')));
      }
    };
    row(c.data, [], 0);
  }
  return out.length ? out : h('div', { class: 'ctempty' }, tr('Nada con «%s».', S.filter));
}

function toggleOpen(key) {
  if (S.open.has(key)) S.open.delete(key); else S.open.add(key);
  lsSet('open', [...S.open].slice(-300));
  drawSideBody();
}

function nodeMenu(anchor, file, path) {
  const c = colEntry(file), node = c && c.data && nodeAt(c.data, path);
  if (!node) return;
  const root = !path.length, folder = root || isFolder(node);
  openMenu(anchor, folder ? [
    { label: tr('Pedido nuevo'), do: guard(() => addNode(file, path, false)) },
    { label: tr('Carpeta nueva'), do: guard(() => addNode(file, path, true)) },
    { label: root ? tr('Auth, scripts y variables') : tr('Auth y scripts'), do: () => openContainer(file, path) },
    { label: tr('Correr…'), do: () => openRunner(file, path) },
    '-',
    { label: tr('Renombrar'), do: guard(() => renameNode(file, path)) },
    { label: tr('Duplicar'), do: guard(() => duplicateNode(file, path)) },
    root ? { label: tr('Exportar'), do: () => exportCollection(c) } : null,
    { label: tr('Borrar'), bad: true, do: guard(() => deleteNode(file, path)) },
  ] : [
    { label: tr('Abrir'), do: () => openItem(file, path) },
    { label: tr('Renombrar'), do: guard(() => renameNode(file, path)) },
    { label: tr('Duplicar'), do: guard(() => duplicateNode(file, path)) },
    '-',
    { label: tr('Borrar'), bad: true, do: guard(() => deleteNode(file, path)) },
  ]);
}

/* one collection changed: read again, changed by fn, written, kept */
async function editCollection(file, fn) {
  await ensureDirs();
  const path = D_COLS + '/' + file;
  const data = await readJSON(path);
  if (!data) throw new Error(tr('La colección ya no está en la tarjeta'));
  const r = fn(data);
  await writeJSON(path, data);
  const c = colEntry(file);
  if (c) { c.data = data; c.err = ''; c.mtime = -1; }
  tellBoard();
  return r;
}

const notFound = name => new Error(tr('No encuentro «%s» en la tarjeta: se cambió en otro lado. Volvé a la página para leerla de nuevo.', name));

async function newCollection() {
  const name = prompt(tr('Nombre de la colección'), tr('Colección nueva'));
  if (!name || !name.trim()) return;
  const file = await createCollection(name.trim());
  S.open.add(file + ':');
  drawSide();
}

async function createCollection(name, data) {
  await ensureDirs();
  const file = freeName(S.cols, name, 'coleccion');
  data = data || { info: { _postman_id: uuid(), name, schema: SCHEMA }, item: [] };
  await writeJSON(D_COLS + '/' + file, data);
  S.cols.push({ file, mtime: -1, size: -1, data, err: '' });
  sortCols();
  tellBoard();
  return file;
}

async function addNode(file, parentPath, folder) {
  const name = prompt(folder ? tr('Nombre de la carpeta') : tr('Nombre del pedido'), folder ? tr('Carpeta nueva') : tr('Pedido nuevo'));
  if (!name || !name.trim()) return;
  const c = colEntry(file);
  const pname = nameOf(nodeAt(c.data, parentPath));
  let newPath;
  await editCollection(file, data => {
    const loc = locate(data, parentPath, pname, true);
    if (!loc) throw notFound(pname);
    if (!Array.isArray(loc.node.item)) loc.node.item = [];
    loc.node.item.push(folder ? { name: name.trim(), item: [] } : newRequest(name.trim()));
    newPath = loc.path.concat(loc.node.item.length - 1);
  });
  S.open.add(file + ':' + newPath.slice(0, -1).join('.'));
  lsSet('open', [...S.open]);
  if (folder) drawSide(); else openItem(file, newPath, true);
}

async function renameNode(file, path) {
  const c = colEntry(file), node = nodeAt(c.data, path);
  const old = nameOf(node);
  const name = prompt(tr('Nombre nuevo'), old);
  if (!name || !name.trim() || name === old) return;
  let at = path;
  await editCollection(file, data => {
    const loc = locate(data, path, old, !path.length || isFolder(node));
    if (!loc) throw notFound(old);
    if (!loc.path.length) data.info.name = name.trim(); else loc.node.name = name.trim();
    at = loc.path;
  });
  if (S.cur && S.cur.file === file && samePath(S.cur.path, path)) {
    S.cur.path = at;
    S.cur.origName = name.trim();
    if (S.cur.kind === 'collection') S.cur.item.info.name = name.trim(); else S.cur.item.name = name.trim();
    if (S.view === 'req') drawMain();
  }
  if (!path.length) sortCols();
  drawSide();
  drawTop();
}

async function duplicateNode(file, path) {
  const c = colEntry(file), node = nodeAt(c.data, path);
  if (!path.length) {
    const data = stripIds(clone(c.data));
    data.info.name = nameOf(c.data) + ' ' + tr('(copia)');
    data.info._postman_id = uuid();
    const f = await createCollection(data.info.name, data);
    S.open.add(f + ':');
    drawSide();
    return;
  }
  let ins;
  await editCollection(file, data => {
    const loc = locate(data, path, nameOf(node), isFolder(node));
    if (!loc) throw notFound(nameOf(node));
    const parent = loc.path.slice(0, -1), idx = loc.path[loc.path.length - 1];
    const cp = stripIds(clone(loc.node));
    cp.name = nameOf(loc.node) + ' ' + tr('(copia)');
    nodeAt(data, parent).item.splice(idx + 1, 0, cp);
    ins = { parent, index: idx + 1 };
  });
  if (S.cur && S.cur.file === file) S.cur.path = shiftPath(S.cur.path, ins.parent, ins.index, 1);
  drawSide();
}

async function deleteNode(file, path) {
  const c = colEntry(file), node = nodeAt(c.data, path);
  const name = nameOf(node);
  if (!path.length) {
    if (!confirm(tr('¿Borrar la colección «%s» con todo lo que tiene?', name))) return;
    await P.fsDelete(D_COLS + '/' + file);
    S.cols = S.cols.filter(x => x.file !== file);
    if (S.cur && S.cur.file === file) { S.cur = null; S.res = null; if (S.view === 'req') S.view = 'empty'; }
    tellBoard();
    drawSide();
    drawMain();
    return;
  }
  const folder = isFolder(node);
  if (!confirm(folder ? tr('¿Borrar la carpeta «%s» con todo lo que tiene?', name) : tr('¿Borrar el pedido «%s»?', name))) return;
  let rm;
  await editCollection(file, data => {
    const loc = locate(data, path, name, folder);
    if (!loc) throw notFound(name);
    const parent = loc.path.slice(0, -1), idx = loc.path[loc.path.length - 1];
    nodeAt(data, parent).item.splice(idx, 1);
    rm = { parent, index: idx };
  });
  if (S.cur && S.cur.file === file) {
    const p = shiftPath(S.cur.path, rm.parent, rm.index, -1);
    if (p) S.cur.path = p;
    else { S.cur = null; S.res = null; if (S.view === 'req') S.view = 'empty'; drawMain(); }
  }
  drawSide();
}

function histList() {
  if (!S.hist.length) return h('div', { class: 'ctempty' }, tr('Todavía no se mandó nada.'));
  return h('div', { class: 'ctlist' }, S.hist.map(e => h('div', { class: 'ctnode', title: e.url,
    onclick: () => openScratch({ name: e.name || e.url, request: clone(e.request) }, true) },
    methodBadge(String(e.method || 'GET').toUpperCase()),
    h('span', { class: 'ctnm' }, e.name || e.url, h('small', {}, e.url)),
    h('span', { class: 'ctsub', style: `color:${!e.status ? 'var(--red)' : e.status >= 400 ? 'var(--orange)' : 'var(--dim)'}` },
      (e.status || tr('error')) + ' · ' + ago(e.t)))));
}

async function clearHistory() {
  if (!S.hist.length || !confirm(tr('¿Borrar el historial?'))) return;
  await ensureDirs();
  await writeJSON(F_HIST, []);
  S.hist = [];
  drawSideBody();
}

/* ---- environments ---------------------------------------------------------------- */

function envList() {
  const act = S.active;
  const rowFor = (file, label, sub) => {
    const isG = file == null;
    const on = !isG && slugOf(file) === act;
    const sel = S.view === 'env' && S.envEd && S.envEd.file === file;
    return h('div', { class: 'ctnode' + (sel ? ' sel' : ''), onclick: () => openEnv(file) },
      h('span', { class: 'ctcar', title: on ? tr('El activo') : '', style: on ? 'color:var(--green)' : '' }, isG ? '◎' : on ? '●' : '○'),
      h('span', { class: 'ctnm' }, label), h('span', { class: 'ctsub' }, sub),
      isG ? null : h('button', { class: 'ctmore', title: tr('Más'), onclick: ev => { ev.stopPropagation(); envMenu(ev.currentTarget, file); } }, '⋯'));
  };
  const cnt = d => { const n = ((d && d.values) || []).length; return n === 1 ? tr('1 variable') : tr('%d variables', n); };
  return h('div', { class: 'ctlist' },
    rowFor(null, tr('Globales'), cnt(S.glob)),
    S.envs.map(e => e.data ? rowFor(e.file, nameOf(e.data) || slugOf(e.file), cnt(e.data))
      : h('div', { class: 'ctnode', title: e.err }, h('span', { class: 'ctcar' }, '⚠'), h('span', { class: 'ctnm' }, e.file))),
    S.envs.length ? null : h('div', { class: 'ctempty' }, tr('Ningún entorno todavía.')));
}

function envMenu(anchor, file) {
  const e = envEntry(file);
  openMenu(anchor, [
    slugOf(file) === S.active ? { label: tr('Desactivar'), do: guard(() => setActive('')) } : { label: tr('Activar'), do: guard(() => setActive(slugOf(file))) },
    { label: tr('Editar'), do: () => openEnv(file) },
    { label: tr('Renombrar'), do: guard(() => renameEnv(file)) },
    { label: tr('Duplicar'), do: guard(() => duplicateEnv(file)) },
    { label: tr('Exportar'), do: () => exportEnv(e.data) },
    '-',
    { label: tr('Borrar'), bad: true, do: guard(() => deleteEnv(file)) },
  ]);
}

/* estado.json: which environment the board and the page use */
async function setActive(slug) {
  S.active = slug;
  drawTop();
  drawSideBody();
  varsChanged();
  await ensureDirs();
  let st = null;
  try { st = await readJSON(F_STATE); } catch {}
  st = isObj(st) ? st : {};
  st.entorno = slug;
  await writeJSON(F_STATE, st);
  tellBoard();
}

async function newEnv() {
  const name = prompt(tr('Nombre del entorno'), tr('Entorno nuevo'));
  if (!name || !name.trim()) return;
  const data = { id: uuid(), name: name.trim(), values: [], _postman_variable_scope: 'environment' };
  const file = await writeNewEnv(data);
  openEnv(file);
}

async function writeNewEnv(data) {
  await ensureDirs();
  const file = freeName(S.envs, data.name, 'entorno');
  await writeJSON(D_ENVS + '/' + file, data);
  S.envs.push({ file, mtime: -1, size: -1, data, err: '' });
  sortEnvs();
  tellBoard();
  drawTop();
  drawSideBody();
  return file;
}

async function editEnvFile(file, fn) {
  await ensureDirs();
  const path = D_ENVS + '/' + file;
  const d = await readJSON(path);
  if (!d) throw new Error(tr('El entorno ya no está en la tarjeta'));
  fn(d);
  await writeJSON(path, d);
  const e = envEntry(file);
  if (e) { e.data = d; e.mtime = -1; }
  tellBoard();
  return d;
}

async function renameEnv(file) {
  const e = envEntry(file);
  const name = prompt(tr('Nombre nuevo'), nameOf(e.data));
  if (!name || !name.trim()) return;
  await editEnvFile(file, d => { d.name = name.trim(); });
  if (S.envEd && S.envEd.file === file) { S.envEd.data.name = name.trim(); if (S.view === 'env') drawMain(); }
  sortEnvs();
  drawTop();
  drawSideBody();
}

async function duplicateEnv(file) {
  const e = envEntry(file);
  const data = clone(e.data);
  data.id = uuid();
  data.name = nameOf(e.data) + ' ' + tr('(copia)');
  openEnv(await writeNewEnv(data));
}

async function deleteEnv(file) {
  const e = envEntry(file);
  if (!confirm(tr('¿Borrar el entorno «%s»?', nameOf(e.data)))) return;
  await P.fsDelete(D_ENVS + '/' + file);
  S.envs = S.envs.filter(x => x.file !== file);
  if (S.envEd && S.envEd.file === file) { S.envEd = null; if (S.view === 'env') { S.view = S.cur ? 'req' : 'empty'; drawMain(); } }
  if (slugOf(file) === S.active) await setActive('');
  tellBoard();
  drawTop();
  drawSideBody();
}

function runList() {
  if (!S.runs.length) return h('div', { class: 'ctempty' }, tr('Ninguna corrida guardada.'));
  return h('div', { class: 'ctlist' }, S.runs.map(f => {
    const m = /^(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)-?(.*)\.json$/.exec(f.name);
    const when = m ? `${m[3]}/${m[2]}/${m[1]} ${m[4]}:${m[5]}` : '';
    const sel = S.view === 'runres' && S.runView && S.runView.file === f.name;
    return h('div', { class: 'ctnode' + (sel ? ' sel' : ''), onclick: guard(() => openRunFile(f.name)) },
      h('span', { class: 'ctcar' }, '▶'),
      h('span', { class: 'ctnm' }, m ? m[7] || f.name : f.name, h('small', {}, when)),
      h('button', { class: 'ctmore', title: tr('Borrar'), onclick: guard(async ev => {
        ev.stopPropagation();
        if (!confirm(tr('¿Borrar esta corrida?'))) return;
        await P.fsDelete(D_RUNS + '/' + f.name);
        S.runs = S.runs.filter(x => x.name !== f.name);
        if (S.runView && S.runView.file === f.name) { S.runView = null; if (S.view === 'runres') { S.view = 'empty'; drawMain(); } }
        drawSideBody();
      }) }, '✕'));
  }));
}

/* ---- the editors ----------------------------------------------------------------------
 * A request: method, URL, then Params, Auth, Headers, Body and Scripts, the
 * answer under it. A folder or a collection: its auth, its scripts, the
 * collection's variables and description. An environment or the globals:
 * their table. Each keeps a copy and says when it has something unsaved. */

const AUTH_TYPES = [['inherit', N_('Heredar del padre')], ['noauth', N_('Sin auth')], ['bearer', N_('Bearer token')],
  ['basic', N_('Basic (usuario y contraseña)')], ['apikey', N_('API key')]];
const AUTH_NAME = { noauth: N_('Sin auth'), bearer: N_('Bearer token'), basic: N_('Basic'), apikey: N_('API key'), oauth2: 'OAuth 2.0' };
const BODY_MODES = [['none', N_('ninguno')], ['raw', 'raw'], ['urlencoded', 'x-www-form-urlencoded'], ['formdata', 'form-data'], ['graphql', 'GraphQL']];
const RAW_LANGS = [['json', 'JSON'], ['text', N_('Texto')], ['xml', 'XML'], ['html', 'HTML'], ['javascript', 'JavaScript']];

const SNIPS = {
  prerequest: [
    [N_('Guardar variable de entorno'), "pm.environment.set('clave', 'valor');"],
    [N_('Variable de esta corrida'), "pm.variables.set('ahora', Date.now());"],
    [N_('Leer una variable'), "const v = pm.variables.get('clave');"],
    [N_('Agregar encabezado'), "pm.request.headers.upsert({ key: 'X-Request-Id', value: pm.variables.replaceIn('{{$guid}}') });"],
    [N_('Saltear el pedido'), 'pm.execution.skipRequest();'],
  ],
  test: [
    [N_('Status 200'), "pm.test('status 200', () => {\n  pm.response.to.have.status(200);\n});"],
    [N_('Tiempo < 500 ms'), "pm.test('responde en menos de 500 ms', () => {\n  pm.expect(pm.response.responseTime).to.be.below(500);\n});"],
    [N_('Guardar variable de la respuesta'), "const j = pm.response.json();\npm.environment.set('id', j.id);"],
    [N_('JSON tiene propiedad'), "pm.test('tiene id', () => {\n  pm.expect(pm.response.json()).to.have.property('id');\n});"],
    [N_('Cuerpo contiene texto'), "pm.test('el cuerpo dice ok', () => {\n  pm.expect(pm.response.text()).to.include('ok');\n});"],
    [N_('Es JSON'), "pm.test('es JSON', () => {\n  pm.response.to.have.header('Content-Type');\n  pm.expect(pm.response.headers.get('Content-Type')).to.include('json');\n});"],
    [N_('Siguiente pedido'), "pm.execution.setNextRequest('Nombre del pedido');"],
  ],
};

function markDirty(on = true) {
  if (!S.cur) return;
  S.cur.dirty = on;
  if (S.dom.dirty) S.dom.dirty.classList.toggle('on', on);
}

function confirmCur() {
  return !S.cur || !S.cur.dirty || confirm(tr('«%s» tiene cambios sin guardar. ¿Descartarlos?', nameOf(S.cur.item) || tr('El pedido')));
}

function foldOnNarrow() { if (narrow()) { S.sideFold = true; drawSide(); } }

function openItem(file, path) {
  const c = colEntry(file), node = c && c.data && nodeAt(c.data, path);
  if (!node) return;
  if (S.cur && S.cur.kind === 'request' && S.cur.file === file && samePath(S.cur.path, path)) { S.view = 'req'; drawMain(); drawSideBody(); foldOnNarrow(); return; }
  if (!confirmCur()) return;
  S.cur = { kind: 'request', file, path: path.slice(), origName: nameOf(node), item: normItem(node), dirty: false };
  S.res = null;
  S.view = 'req';
  drawMain();
  drawSideBody();
  foldOnNarrow();
}

/* a request that is in no collection (new, from the history, from cURL) */
function openScratch(item, fromHist) {
  if (!confirmCur()) return;
  S.cur = { kind: 'request', file: null, path: null, origName: '', item: normItem(item), dirty: !fromHist };
  S.res = null;
  S.view = 'req';
  drawMain();
  drawSideBody();
  foldOnNarrow();
}

function openContainer(file, path) {
  const c = colEntry(file), node = c && c.data && nodeAt(c.data, path);
  if (!node) return;
  if (!confirmCur()) return;
  const root = !path.length;
  const item = root ? { info: clone(node.info), auth: clone(node.auth), event: clone(node.event), variable: clone(node.variable || []) }
    : { name: node.name, description: clone(node.description), auth: clone(node.auth), event: clone(node.event) };
  for (const k of Object.keys(item)) if (item[k] === undefined) delete item[k];
  if (root && !item.variable) item.variable = [];
  S.cur = { kind: root ? 'collection' : 'folder', file, path: path.slice(), origName: nameOf(node), item, origVars: clone(item.variable || []), dirty: false };
  S.cTab = S.cTab || 'auth';
  S.res = null;
  S.view = 'req';
  drawMain();
  drawSideBody();
  foldOnNarrow();
}

function crumbsOf(cur) {
  const c = cur.file ? colEntry(cur.file) : null;
  if (!c || !c.data) return tr('Sin colección');
  const names = chainOf(c.data, cur.path).slice(0, -1).map(nameOf);
  return names.length ? names.join(' › ') + ' ›' : '';
}

function editorHead(cur, extra) {
  const it = cur.item;
  S.dom.dirty = h('span', { class: 'ctdirty' + (cur.dirty ? ' on' : ''), title: tr('Cambios sin guardar') });
  const name = h('input', { class: 'ctname', type: 'text', value: nameOf(it), placeholder: tr('Nombre'), 'aria-label': tr('Nombre'),
    oninput: e => { if (it.info) it.info.name = e.target.value; else it.name = e.target.value; markDirty(); } });
  return h('div', { class: 'cthead' },
    h('span', { class: 'ctcrumb', title: crumbsOf(cur) }, crumbsOf(cur)), name, S.dom.dirty,
    h('button', { class: 'btn pri ctsmall', title: tr('Guardar (Ctrl+S)'), onclick: guard(saveCurrent) }, tr('Guardar')),
    extra);
}

function drawEditor(box) {
  if (S.cur.kind === 'request') drawRequest(box); else drawContainer(box);
}

function drawRequest(box) {
  const cur = S.cur, it = cur.item, rq = it.request;
  const more = h('button', { class: 'btn ctsmall', title: tr('Más'), onclick: e => openMenu(e.currentTarget, [
    { label: tr('Guardar como…'), do: guard(saveAs) },
    { label: tr('Copiar como cURL'), do: () => copyText(toCurl(previewFinal()), tr('Comando copiado')) },
    cur.file ? { label: tr('Duplicar'), do: guard(() => duplicateNode(cur.file, cur.path)) } : null,
  ]) }, '⋯');
  const meth = h('select', { class: 'ctmeth ctm-' + rq.method, 'aria-label': tr('Método'), onchange: e => {
    rq.method = e.target.value;
    e.target.className = 'ctmeth ctm-' + rq.method;
    markDirty();
  } }, (METHODS.includes(rq.method) ? METHODS : METHODS.concat(rq.method)).map(m => h('option', { value: m, selected: m === rq.method }, m)));
  const uf = urlField(rq.url, raw => {
    setUrlRaw(rq.url, raw);
    syncPathVars(rq.url);
    markDirty();
    if (S.reqTab === 'params') drawReqTab();
    drawReqTabs();
  });
  S.dom.url = uf;
  S.dom.sendB = h('button', { class: 'btn pri ctsend', title: tr('Mandar (Ctrl+Enter)'), onclick: () => S.sending ? cancelSend() : sendCurrent() });
  S.dom.reqTabs = h('div', {});
  S.dom.reqTab = h('div', { class: 'ctbody' });
  S.dom.res = h('div', { class: 'ctcard ctres' });
  put(box,
    h('div', { class: 'ctcard' },
      editorHead(cur, more),
      h('div', { class: 'ctline' }, meth, uf.el, S.dom.sendB),
      uf.prev,
      h('div', { style: 'margin-top:12px' }, S.dom.reqTabs, S.dom.reqTab)),
    S.dom.res);
  drawSendState();
  drawReqTabs();
  drawReqTab();
  drawResp();
}

/* :name in the path: a row each in url.variable */
function syncPathVars(u) {
  const names = [...splitUrl(u.raw).base.matchAll(/(?:^|\/):([A-Za-z_][\w-]*)(?=\/|$)/g)].map(m => m[1]);
  if (!names.length && !(u.variable || []).length) return;
  const old = u.variable || [];
  u.variable = names.map(k => old.find(v => v.key === k) || { key: k, value: '' });
}

function drawReqTabs() {
  const rq = S.cur.item.request, it = S.cur.item;
  const q = (rq.url.query || []).filter(x => !x.disabled && x.key).length + (rq.url.variable || []).length;
  const hd = (rq.header || []).filter(x => !x.disabled && x.key).length;
  const b = rq.body && rq.body.mode && rq.body.mode !== 'none';
  const a = rq.auth && rq.auth.type && rq.auth.type !== 'inherit';
  const sc = !!(scriptOf(it, 'prerequest').trim() || scriptOf(it, 'test').trim());
  put(S.dom.reqTabs, tabs([
    { id: 'params', label: tr('Params'), count: q },
    { id: 'auth', label: tr('Auth'), dot: a },
    { id: 'headers', label: tr('Encabezados'), count: hd },
    { id: 'body', label: tr('Cuerpo'), dot: b },
    { id: 'scripts', label: tr('Scripts'), dot: sc },
  ], S.reqTab, id => { S.reqTab = id; drawReqTabs(); drawReqTab(); }));
}

function changedTab() { markDirty(); drawReqTabs(); }

function drawReqTab() {
  const box = S.dom.reqTab, cur = S.cur, it = cur.item, rq = it.request;
  if (S.reqTab === 'params') {
    if (!Array.isArray(rq.url.query)) rq.url.query = [];
    const pv = rq.url.variable || [];
    put(box,
      kvTable(rq.url.query, { desc: true, keyPh: N_('Parámetro'), onChange: () => { setUrlQuery(rq.url); S.dom.url.set(rq.url.raw); changedTab(); } }),
      pv.length ? h('div', {}, h('h2', {}, tr('Variables de la ruta')),
        kvTable(pv, { readonly: true, desc: true, onChange: changedTab })) : null,
      h('p', { class: 'note' }, tr('La tabla y la URL van juntas: lo que se cambia en una se ve en la otra. Los desactivados quedan guardados y no van.')));
  } else if (S.reqTab === 'headers') {
    put(box, kvTable(rq.header, { desc: true, keyPh: N_('Encabezado'), onChange: () => { changedTab(); drawAuto(); } }), S.dom.auto = h('div', { class: 'ctauto' }));
    drawAuto();
  } else if (S.reqTab === 'body') {
    drawBodyTab(box, rq);
  } else if (S.reqTab === 'auth') {
    const c = cur.file ? colEntry(cur.file) : null;
    const chain = c && c.data ? chainOf(c.data, cur.path).slice(0, -1) : [];
    put(box, authEditor(rq, true, chain, changedTab));
  } else {
    put(box, scriptsEditor(it, changedTab, tr('Corren después de los de la colección y sus carpetas, en ese orden.')));
  }
}

/* what goes with the request without being in its table */
function previewFinal() {
  const cur = S.cur;
  const c = cur.file ? colEntry(cur.file) : null;
  const chain = c && c.data ? chainOf(c.data, cur.path).slice(0, -1) : [];
  const ctx = { req: cur.item.request, scopes: scopesFor(c && c.data, null) };
  return buildFinal(ctx, chain);
}

function drawAuto() {
  if (!S.dom.auto) return;
  let f;
  try { f = previewFinal(); } catch { return; }
  const rows = f.auto.map(([k, v, why]) => h('div', {}, h('span', { class: 'mono' }, k + ': ' + (/^authorization$/i.test(k) ? v.replace(/^(\S+ ).*$/, '$1••••••') : v)), ' — ', why));
  const by = S.mode === 'board' ? tr('Host, Content-Length, Connection y User-Agent los pone la placa.') : tr('Host, Content-Length, User-Agent, Origin y los demás los pone el navegador.');
  put(S.dom.auto, h('b', {}, tr('Se agregan solos')), rows, h('div', {}, by));
}

function drawBodyTab(box, rq) {
  const b = rq.body || {};
  const mode = b.mode || 'none';
  const setMode = m => {
    if (!rq.body) rq.body = {};
    rq.body.mode = m;
    if (m === 'raw' && !(rq.body.options && rq.body.options.raw && rq.body.options.raw.language)) rq.body.options = Object.assign({}, rq.body.options, { raw: { language: 'json' } });
    if (m === 'urlencoded' && !Array.isArray(rq.body.urlencoded)) rq.body.urlencoded = [];
    if (m === 'formdata' && !Array.isArray(rq.body.formdata)) rq.body.formdata = [];
    if (m === 'graphql' && !isObj(rq.body.graphql)) rq.body.graphql = { query: '', variables: '' };
    changedTab();
    drawAuto();
    drawReqTab();
  };
  const modes = h('div', { class: 'ctrow' }, BODY_MODES.map(([m, label]) => h('label', {},
    h('input', { type: 'radio', name: 'ctbm', checked: m === mode, onchange: () => setMode(m) }), tr(label))));
  let panel = null;
  if (mode === 'raw') {
    const lang = (b.options && b.options.raw && b.options.raw.language) || 'text';
    const ok = h('span', { class: 'ctjs' });
    const check = () => {
      if (lang !== 'json') { put(ok); return; }
      const err = jsonCheck(b.raw || '');
      put(ok, err ? h('span', { class: 'bad', title: err }, '✗ ' + tr('JSON con errores')) : (b.raw || '').trim() ? h('span', { class: 'ok' }, '✓ ' + tr('JSON válido')) : '');
    };
    const ta = codeArea(b.raw || '', v => { b.raw = v; markDirty(); check(); }, { minH: 220, placeholder: lang === 'json' ? '{ "clave": "{{valor}}" }' : '' });
    const sel = h('select', { onchange: e => {
      b.options = Object.assign({}, b.options, { raw: Object.assign({}, b.options && b.options.raw, { language: e.target.value }) });
      changedTab(); drawAuto(); drawReqTab();
    } }, RAW_LANGS.map(([v, l]) => h('option', { value: v, selected: v === lang }, tr(l))));
    panel = h('div', {},
      h('div', { class: 'ctrow' }, sel,
        lang === 'json' ? h('button', { class: 'btn ctsmall', onclick: () => {
          try {
            const f = jsonFormat(ta.value);
            ta.focus();
            ta.select();
            let done = false;
            try { done = document.execCommand('insertText', false, f); } catch {}
            if (!done) ta.value = f;
            b.raw = f;
            markDirty();
            check();
          } catch (e) { toast(tr('No se puede formatear: %s', e.message), true); }
        } }, tr('Formatear JSON')) : null, ok),
      ta);
    check();
  } else if (mode === 'urlencoded') {
    panel = kvTable(b.urlencoded, { desc: true, onChange: changedTab, newRow: () => ({ type: 'text' }) });
  } else if (mode === 'formdata') {
    panel = h('div', {}, kvTable(b.formdata, { desc: true, onChange: changedTab, newRow: () => ({ type: 'text' }) }),
      h('p', { class: 'note' }, tr('Sólo campos de texto: van como multipart/form-data.')));
  } else if (mode === 'graphql') {
    const g = b.graphql;
    const vok = h('span', { class: 'ctjs' });
    const vcheck = () => { const err = jsonCheck(g.variables || ''); put(vok, err ? h('span', { class: 'bad', title: err }, '✗ ' + tr('JSON con errores')) : ''); };
    panel = h('div', {},
      h('label', { class: 'f' }, tr('Consulta')),
      codeArea(g.query || '', v => { g.query = v; markDirty(); }, { minH: 180, placeholder: 'query { usuarios { id nombre } }' }),
      h('div', { class: 'ctrow' }, h('label', { class: 'f', style: 'margin:8px 0 0' }, tr('Variables (JSON)')), vok),
      codeArea(g.variables || '', v => { g.variables = v; markDirty(); vcheck(); }, { minH: 90, placeholder: '{ "id": 1 }' }),
      h('p', { class: 'note' }, tr('Va como JSON: {"query": ..., "variables": ...}.')));
    vcheck();
  } else if (mode !== 'none') {
    panel = h('p', { class: 'note' }, tr('Este cuerpo («%s») no está en Cartero: se guarda como está y no se manda.', mode));
  } else {
    panel = h('p', { class: 'note' }, tr('Sin cuerpo.'));
  }
  put(box, modes, panel);
}

/* the auth of a request, a folder or the collection */
function authEditor(holder, canInherit, chain, onChange) {
  const box = h('div', {});
  const draw = () => {
    const a = holder.auth;
    let type = a && a.type ? a.type : 'inherit';
    if (type === 'inherit' && !canInherit) type = 'noauth';
    const types = AUTH_TYPES.filter(([t]) => canInherit || t !== 'inherit');
    if (!types.some(([t]) => t === type)) types.push([type, AUTH_NAME[type] || type]);
    const sel = h('select', { style: 'width:auto', onchange: e => {
      const t = e.target.value;
      if (t === 'inherit') delete holder.auth;
      else {
        holder.auth = Object.assign(holder.auth || {}, { type: t });
        if (t !== 'noauth' && !holder.auth[t]) holder.auth[t] = [];
        if (t === 'apikey' && !authParams(holder.auth).in) setAuthParams(holder.auth, Object.assign({ key: 'X-Api-Key', value: '', in: 'header' }, authParams(holder.auth)));
      }
      onChange();
      draw();
    } }, types.map(([t, l]) => h('option', { value: t, selected: t === type }, tr(l))));
    const ps = authParams(holder.auth);
    const field = (k, label, attrs = {}) => {
      const inp = h('input', Object.assign({ type: 'text', value: valStr(ps[k]), spellcheck: 'false', autocomplete: 'off', oninput: e => {
        const p = authParams(holder.auth);
        p[k] = e.target.value;
        setAuthParams(holder.auth, p);
        onChange();
      } }, attrs));
      const w = attrs.type === 'password' ? h('div', { class: 'ctpw' }, inp, h('button', { class: 'btn ctsmall', title: tr('Mostrar u ocultar'), onclick: () => { inp.type = inp.type === 'password' ? 'text' : 'password'; } }, '👁')) : inp;
      return [h('label', {}, tr(label)), w];
    };
    let fields = null;
    if (type === 'bearer') fields = h('div', { class: 'ctfield' }, field('token', N_('Token'), { placeholder: '{{token}}', class: 'mono' }));
    else if (type === 'basic') fields = h('div', { class: 'ctfield' }, field('username', N_('Usuario')), field('password', N_('Contraseña'), { type: 'password', autocomplete: 'new-password' }));
    else if (type === 'apikey') {
      const inSel = h('select', { onchange: e => { const p = authParams(holder.auth); p.in = e.target.value; setAuthParams(holder.auth, p); onChange(); } },
        h('option', { value: 'header', selected: ps.in !== 'query' }, tr('Encabezado')), h('option', { value: 'query', selected: ps.in === 'query' }, tr('Parámetro de la URL')));
      fields = h('div', { class: 'ctfield' }, field('key', N_('Nombre'), { placeholder: 'X-Api-Key' }), field('value', N_('Valor'), { class: 'mono' }), h('label', {}, tr('Va en')), inSel);
    } else if (type === 'inherit') {
      const { auth, from } = effectiveAuth(null, chain);
      fields = h('p', { class: 'note' }, auth && from && auth.type !== 'noauth'
        ? tr('Hereda «%s» de «%s».', tr(AUTH_NAME[auth.type] || auth.type), nameOf(from))
        : chain.length ? tr('Nadie más arriba tiene auth: va sin.') : tr('Sin colección no hay de quién heredar: va sin.'));
    } else if (type === 'noauth') {
      fields = h('p', { class: 'note' }, tr('Va sin auth, aunque la carpeta o la colección tengan.'));
    } else {
      fields = h('p', { class: 'note' }, tr('Esta auth («%s») no está en Cartero: se guarda como está y no se manda.', type));
    }
    put(box, h('div', { class: 'ctrow' }, h('span', { class: 'muted small' }, tr('Tipo')), sel), fields,
      h('p', { class: 'note' }, tr('Los valores pueden llevar {{variables}}.')));
  };
  draw();
  return box;
}

function scriptsEditor(node, onChange, note) {
  const box = h('div', {});
  const draw = () => {
    const phase = S.scriptTab;
    const ta = codeArea(scriptOf(node, phase), v => { setScript(node, phase, v); onChange(); }, {
      minH: 240, placeholder: phase === 'prerequest' ? "pm.environment.set('ahora', Date.now());" : "pm.test('status 200', () => pm.response.to.have.status(200));" });
    put(box,
      tabs([{ id: 'prerequest', label: tr('Pre-request'), dot: !!scriptOf(node, 'prerequest').trim() }, { id: 'test', label: tr('Tests'), dot: !!scriptOf(node, 'test').trim() }],
        phase, id => { S.scriptTab = id; draw(); }),
      h('div', { class: 'ctsnips' }, SNIPS[phase].map(([l, code]) => h('button', { class: 'btn', title: code, onclick: () => ta.insert(code) }, tr(l)))),
      ta,
      h('p', { class: 'note' }, note, ' ', tr('El JavaScript del navegador, con pm como en Postman; la placa corre un subconjunto (sin expresiones regulares, clases ni async).')));
  };
  draw();
  return box;
}

function drawContainer(box) {
  const cur = S.cur, it = cur.item;
  const c = colEntry(cur.file);
  const chain = c && c.data ? chainOf(c.data, cur.path).slice(0, -1) : [];
  const tabBox = h('div', { class: 'ctbody' });
  const tabsBox = h('div', {});
  const root = cur.kind === 'collection';
  const onChange = () => { markDirty(); drawT(); };
  const drawT = () => put(tabsBox, tabs([
    { id: 'auth', label: tr('Auth'), dot: !!(it.auth && it.auth.type) },
    { id: 'scripts', label: tr('Scripts'), dot: !!(scriptOf(it, 'prerequest').trim() || scriptOf(it, 'test').trim()) },
    root ? { id: 'vars', label: tr('Variables'), count: (it.variable || []).filter(enabledEntry).length } : null,
    { id: 'desc', label: tr('Descripción') },
  ], S.cTab, id => { S.cTab = id; drawT(); drawB(); }));
  const drawB = () => {
    if (S.cTab === 'vars' && !root) S.cTab = 'auth';
    if (S.cTab === 'auth') put(tabBox, authEditor(it, !root, chain, onChange));
    else if (S.cTab === 'scripts') put(tabBox, scriptsEditor(it, onChange, root ? tr('Corren antes que los de cada carpeta y pedido de la colección.') : tr('Corren después de los de la colección y antes que los de cada pedido de la carpeta.')));
    else if (S.cTab === 'vars') put(tabBox, kvTable(it.variable, { onChange }), h('p', { class: 'note' }, tr('Las variables de la colección: las usa cualquier pedido de ella si el entorno activo no tiene una con el mismo nombre.')));
    else {
      const holder = root ? it.info : it;
      put(tabBox, codeArea(descText(holder.description), v => {
        if (isObj(holder.description)) holder.description.content = v; else if (v) holder.description = v; else delete holder.description;
        markDirty();
      }, { minH: 200, placeholder: tr('Para qué es, en Markdown') }));
    }
  };
  put(box, h('div', { class: 'ctcard' },
    editorHead(cur, h('button', { class: 'btn ctsmall', onclick: () => openRunner(cur.file, cur.path) }, '▶ ' + tr('Correr'))),
    h('p', { class: 'note', style: 'margin-top:0' }, root ? tr('La colección: lo que pongas acá vale para todos sus pedidos.') : tr('La carpeta: lo que pongas acá vale para todos sus pedidos.')),
    tabsBox, tabBox));
  drawT();
  drawB();
}

/* ---- saving ---------------------------------------------------------------------------- */

const OWN_REQ = ['method', 'url', 'header', 'body', 'auth', 'description'];

/* the edited request over the one read again: its own fields from the
 * editor, whatever else the file has (examples, ids, settings) kept */
function mergeItem(fresh, mine) {
  const out = Object.assign({}, fresh, { name: mine.name, request: Object.assign({}, isObj(fresh.request) ? fresh.request : {}) });
  for (const k of OWN_REQ) { if (mine.request[k] === undefined) delete out.request[k]; else out.request[k] = mine.request[k]; }
  if (mine.event === undefined) delete out.event; else out.event = mine.event;
  return out;
}

async function saveCurrent() {
  if (S.view === 'env' && S.envEd) return saveEnv();
  if (S.view !== 'req' || !S.cur) return;
  if (S.cur.kind === 'request') return saveRequest();
  return saveContainer();
}

async function saveRequest() {
  const cur = S.cur;
  if (!cur.file) return saveAs();
  const mine = cleanItem(cur.item);
  await editCollection(cur.file, data => {
    const loc = locate(data, cur.path, cur.origName, false);
    if (!loc) throw new Error(tr('«%s» ya no está en la colección (se cambió en otro lado): usá Guardar como…', cur.origName));
    const parent = nodeAt(data, loc.path.slice(0, -1));
    parent.item[loc.path[loc.path.length - 1]] = mergeItem(loc.node, mine);
    cur.path = loc.path;
  });
  cur.origName = mine.name;
  markDirty(false);
  drawSideBody();
  toast(tr('Guardado'));
}

async function saveContainer() {
  const cur = S.cur, e = cur.item;
  await editCollection(cur.file, data => {
    const loc = locate(data, cur.path, cur.origName, true);
    if (!loc) throw notFound(cur.origName);
    const n = loc.node;
    if (cur.kind === 'collection') {
      n.info = Object.assign({}, n.info, { name: e.info.name, description: e.info.description });
      if (e.info.description === undefined) delete n.info.description;
      n.variable = mergeTable(cur.origVars, (e.variable || []).filter(v => v.key), n.variable || []);
      if (!n.variable.length) delete n.variable;
    } else {
      n.name = e.name;
      if (e.description === undefined) delete n.description; else n.description = e.description;
    }
    for (const k of ['auth', 'event']) { if (e[k] === undefined) delete n[k]; else n[k] = clone(e[k]); }
    cur.path = loc.path;
    cur.origVars = clone(n.variable || []);
  });
  cur.origName = nameOf(e);
  if (cur.kind === 'collection') sortCols();
  markDirty(false);
  drawSide();
  varsChanged();
  toast(tr('Guardado'));
}

/* into any collection or folder, or a new collection */
async function saveAs() {
  const cur = S.cur;
  if (!cur || cur.kind !== 'request') return;
  const name = h('input', { type: 'text', value: cur.item.name || tr('Pedido nuevo') });
  const sel = h('select', {});
  const targets = [];
  for (const c of S.cols) {
    if (!c.data) continue;
    targets.push({ file: c.file, path: [], name: nameOf(c.data), folder: true });
    walk(c.data, (n, p) => { if (isFolder(n)) targets.push({ file: c.file, path: p, name: nameOf(n), folder: true, depth: p.length }); });
  }
  const here = cur.file ? cur.file + ':' + cur.path.slice(0, -1).join('.') : '';
  targets.forEach((t, i) => sel.append(h('option', { value: i, selected: t.file + ':' + t.path.join('.') === here },
    '  '.repeat(t.depth || 0) + (t.path.length ? '▸ ' : '') + t.name)));
  sel.append(h('option', { value: 'new', selected: !targets.length }, '+ ' + tr('Colección nueva…')));
  const newName = h('input', { type: 'text', placeholder: tr('Nombre de la colección'), style: targets.length ? 'display:none' : '' });
  sel.addEventListener('change', () => { newName.style.display = sel.value === 'new' ? '' : 'none'; });
  await modal(tr('Guardar como'), h('div', {}, h('label', {}, tr('Nombre')), name, h('label', {}, tr('Dónde')), sel, newName), tr('Guardar'), async () => {
    const nm = name.value.trim();
    if (!nm) { toast(tr('Falta el nombre'), true); return false; }
    let t;
    if (sel.value === 'new') {
      const cn = newName.value.trim();
      if (!cn) { toast(tr('Falta el nombre de la colección'), true); return false; }
      const file = await createCollection(cn);
      t = { file, path: [], name: cn };
    } else t = targets[+sel.value];
    const item = cleanItem(cur.item);
    item.name = nm;
    stripIds(item);
    let newPath;
    await editCollection(t.file, data => {
      const loc = locate(data, t.path, t.name, true);
      if (!loc) throw notFound(t.name);
      if (!Array.isArray(loc.node.item)) loc.node.item = [];
      loc.node.item.push(item);
      newPath = loc.path.concat(loc.node.item.length - 1);
    });
    S.cur = { kind: 'request', file: t.file, path: newPath, origName: nm, item: normItem(item), dirty: false };
    S.open.add(t.file + ':');
    S.open.add(t.file + ':' + newPath.slice(0, -1).join('.'));
    toast(tr('Guardado en «%s»', t.name));
    drawMain();
    drawSide();
  });
}

function drawEnv(box) {
  const ed = S.envEd;
  const isG = ed.file == null;
  const on = !isG && slugOf(ed.file) === S.active;
  S.dom.envDirty = h('span', { class: 'ctdirty' + (ed.dirty ? ' on' : ''), title: tr('Cambios sin guardar') });
  const mark = () => { ed.dirty = true; S.dom.envDirty.classList.add('on'); };
  const name = isG ? h('span', { class: 'ctname', style: 'font-size:18px;font-weight:650;flex:1;padding:4px 8px' }, tr('Globales'))
    : h('input', { class: 'ctname', type: 'text', value: nameOf(ed.data), oninput: e => { ed.data.name = e.target.value; mark(); } });
  put(box, h('div', { class: 'ctcard' },
    h('div', { class: 'cthead' },
      h('span', { class: 'ctcrumb' }, isG ? tr('Variables de todas las colecciones') : tr('Entorno')), name, S.dom.envDirty,
      isG ? null : on ? h('span', { class: 'pill', style: 'color:var(--green)' }, '● ' + tr('Activo'))
        : h('button', { class: 'btn ctsmall', onclick: guard(() => setActive(slugOf(ed.file))) }, tr('Activar')),
      h('button', { class: 'btn pri ctsmall', title: tr('Guardar (Ctrl+S)'), onclick: guard(saveEnv) }, tr('Guardar')),
      isG ? h('button', { class: 'btn ctsmall', onclick: () => exportEnv(ed.data) }, tr('Exportar'))
        : h('button', { class: 'btn ctsmall', onclick: e => envMenu(e.currentTarget, ed.file) }, '⋯')),
    kvTable(ed.data.values, { env: true, secret: true, onChange: mark, keyPh: N_('Variable nueva') }),
    h('p', { class: 'note' }, isG ? tr('Las globales valen siempre, después del entorno activo y de la colección.')
      : tr('El entorno activo manda sobre la colección y las globales. Cambiarlo acá también lo cambia en la placa.')),
    h('p', { class: 'note' }, tr('Un secreto se ve como puntos, pero queda en la tarjeta tal cual: cualquiera con la tarjeta lo lee.'))));
}

function openEnv(file) {
  if (S.envEd && S.envEd.dirty && S.envEd.file !== file && !confirm(tr('El entorno tiene cambios sin guardar. ¿Descartarlos?'))) return;
  if (!(S.envEd && S.envEd.file === file && S.envEd.dirty)) {
    const src = file == null ? S.glob || { name: 'Globales', values: [], _postman_variable_scope: 'globals' } : envEntry(file) && envEntry(file).data;
    if (!src) return;
    const data = clone(src);
    if (!Array.isArray(data.values)) data.values = [];
    S.envEd = { file, data, orig: clone(data.values), dirty: false };
  }
  S.view = 'env';
  drawMain();
  drawSideBody();
  foldOnNarrow();
}

async function saveEnv() {
  const ed = S.envEd;
  await ensureDirs();
  const isG = ed.file == null;
  const path = isG ? F_GLOB : D_ENVS + '/' + ed.file;
  let fresh = await readJSON(path);
  if (!isObj(fresh)) fresh = isG ? { name: 'Globales', values: [], _postman_variable_scope: 'globals' } : { id: uuid(), name: '', values: [], _postman_variable_scope: 'environment' };
  fresh.values = mergeTable(ed.orig, ed.data.values.filter(v => v.key), fresh.values || []);
  if (!isG) fresh.name = ed.data.name;
  await writeJSON(path, fresh);
  if (isG) S.glob = fresh;
  else { const e = envEntry(ed.file); if (e) { e.data = fresh; e.mtime = -1; } }
  S.envEd = { file: ed.file, data: clone(fresh), orig: clone(fresh.values), dirty: false };
  tellBoard();
  sortEnvs();
  drawMain();
  drawTop();
  drawSideBody();
  varsChanged();
  toast(tr('Guardado'));
}

/* the variables changed (a script, an environment, another active one) */
function varsChanged() {
  if (!S || S.gone) return;
  if (S.view === 'req' && S.cur && S.cur.kind === 'request') {
    if (S.dom.url) S.dom.url.paint();
    if (S.reqTab === 'headers') drawAuto();
  }
  if (S.view === 'env' && S.envEd && !S.envEd.dirty) {
    const src = S.envEd.file == null ? S.glob : envEntry(S.envEd.file) && envEntry(S.envEd.file).data;
    if (src) {
      const data = clone(src);
      if (!Array.isArray(data.values)) data.values = [];
      S.envEd = { file: S.envEd.file, data, orig: clone(data.values), dirty: false };
      drawMain();
    }
  }
  if (S.side === 'env') drawSideBody();
}

/* ---- sending ------------------------------------------------------------------------ */

function drawSendState() {
  const b = S.dom.sendB;
  if (!b || !b.isConnected) return;
  b.textContent = S.sending ? tr('Cancelar') : tr('Enviar');
  b.className = 'btn ctsend ' + (S.sending ? 'red' : 'pri');
}

function cancelSend() {
  if (S.sending && S.sending.token.cancel) S.sending.token.cancel();
}

async function sendCurrent() {
  if (S.sending || !S.cur || S.cur.kind !== 'request' || S.view !== 'req') return;
  if (S.run && S.run.running) { toast(tr('El runner está corriendo'), true); return; }
  const cur = S.cur;
  const c = cur.file ? colEntry(cur.file) : null;
  const item = clone(cur.item);
  const token = {};
  S.sending = { token, mode: S.mode, t0: Date.now() };
  drawSendState();
  drawResp();
  let ctx;
  try {
    ctx = await execute({ col: c && c.data, path: cur.path, item, mode: S.mode, timeout: S.timeout, follow: S.follow, token });
  } catch (e) {
    S.sending = null;
    S.res = e instanceof NotOpenError ? { notOpen: true } : { error: errText(e) };
    drawSendState();
    drawResp();
    return;
  }
  S.sending = null;
  S.res = { ctx, name: item.name, at: Date.now() };
  S.resTab = S.resTab || 'body';
  if (!ctx.resp && ctx.skip) S.resTab = 'tests';
  drawSendState();
  drawResp();
  try { await flushVars(ctx, cur.file); } catch (e) { toast(errText(e), true); }
  if (ctx.final) addHistory(cur.item, ctx);
}

async function openAndRetry() {
  try { await P.openApp(APP); } catch (e) { toast(errText(e), true); return; }
  S.res = { waiting: true };
  drawResp();
  for (let i = 0; i < 40; i++) {
    await sleep(300);
    if (S.gone) return;
    try { if (await boardOpen()) { S.res = null; sendCurrent(); return; } } catch {}
  }
  S.res = { notOpen: true, tried: true };
  drawResp();
}

/* ---- the response ------------------------------------------------------------------------
 * Status, time (with the board's breakdown), size, redirects; the body as a
 * tree, raw or shown; the headers; the tests with the console. */

const TIME_PARTS = [['connect', N_('conexión'), 'var(--orange)'], ['send', N_('envío'), '#bf5af2'], ['wait', N_('espera'), 'var(--accent)'], ['download', N_('descarga'), 'var(--green)']];
const EXT = { json: 'json', xml: 'xml', html: 'html', js: 'js', text: 'txt' };

function timeBar(t) {
  const parts = TIME_PARTS.map(([k, l, c]) => [Math.max(0, +t[k] || 0), l, c]);
  const total = parts.reduce((a, p) => a + p[0], 0);
  if (!total) return null;
  const title = parts.map(([v, l]) => tr(l) + ': ' + fmtMs(v)).join('\n');
  return h('span', { style: 'display:inline-flex;flex-direction:column;gap:3px' },
    h('div', { class: 'cttime', title }, parts.map(([v, , c]) => v ? h('div', { style: `width:${(v / total * 100).toFixed(2)}%;background:${c}` }) : null)),
    h('span', { class: 'ctlegend' }, parts.map(([v, l, c]) => v ? h('span', {}, h('i', { style: 'background:' + c }), tr(l) + ' ' + fmtMs(v)) : null)));
}

function fileNameFor(final, kind, ct) {
  let base = '';
  try { base = decodeURIComponent(new URL(final.url).pathname.split('/').pop() || ''); } catch {}
  if (/\.[a-z0-9]{1,5}$/i.test(base)) return base.replace(/[^A-Za-z0-9._-]+/g, '_');
  const img = /^image\/([a-z0-9]+)/i.exec(ct || '');
  const ext = img ? img[1].replace('jpeg', 'jpg').replace('svg+xml', 'svg') : EXT[kind] || 'bin';
  return (P.fsSlug(base, 40, 'respuesta') || 'respuesta') + '.' + ext;
}

function drawResp() {
  const box = S.dom.res;
  if (!box || !box.isConnected) return;
  if (S.blobUrl) { URL.revokeObjectURL(S.blobUrl); S.blobUrl = null; }
  if (S.sending) {
    put(box, h('p', { class: 'muted' }, h('span', { class: 'ctspin' }),
      S.sending.mode === 'board' ? tr('Mandando vía la placa…') : tr('Mandando desde el navegador…'), ' ',
      h('button', { class: 'btn ctsmall', onclick: cancelSend }, tr('Cancelar'))));
    return;
  }
  const res = S.res;
  if (!res) {
    put(box, h('p', { class: 'muted', style: 'margin:0' }, tr('La respuesta va a aparecer acá. Ctrl+Enter (⌘+Enter) manda, Ctrl+S guarda.')));
    return;
  }
  if (res.waiting) { put(box, h('p', { class: 'muted' }, h('span', { class: 'ctspin' }), tr('Abriendo Cartero en la placa…'))); return; }
  if (res.notOpen) {
    put(box, h('div', { class: 'ctnote warn' },
      h('div', {}, res.tried ? tr('Cartero no terminó de abrir en la placa. Probá de nuevo.') : tr('Cartero no está abierta en la placa, y hace falta para mandar vía la placa.')),
      h('button', { class: 'btn pri', onclick: openAndRetry }, tr('Abrir Cartero en la placa')),
      h('button', { class: 'btn', onclick: () => { setMode('direct'); sendCurrent(); } }, tr('Mandarlo directo desde el navegador'))));
    return;
  }
  if (res.error) { put(box, h('div', { class: 'ctnote bad' }, res.error)); return; }
  const ctx = res.ctx, r = ctx.resp;
  if (!r) {
    put(box, h('div', { class: 'ctnote' }, tr('El pre-request salteó el pedido (pm.execution.skipRequest): no se mandó.')), testsPanel(ctx));
    return;
  }
  const passed = ctx.tests.filter(t => t.ok && !t.skipped).length, total = ctx.tests.filter(t => !t.skipped).length;
  const failed = total - passed;
  const meta = h('div', { class: 'ctmeta' },
    statusPill(r.ok ? r.status : 0, r.ok ? r.reason : ''),
    h('span', {}, tr('Tiempo'), ' ', h('b', {}, fmtMs(r.ms))),
    r.ok ? h('span', {}, tr('Tamaño'), ' ', h('b', {}, fmtSize(r.size))) : null,
    r.times ? timeBar(r.times) : null,
    r.redirects ? h('span', { class: 'muted', title: r.finalUrl }, '↪ ' + (r.via === 'board' ? tr('%d redirecciones', r.redirects) : tr('redirigido')) + (r.finalUrl ? ' → ' + r.finalUrl : '')) : null,
    h('span', { class: 'muted small' }, r.via === 'board' ? tr('vía la placa') : tr('desde el navegador')));
  const kids = [meta];
  if (!r.ok) {
    kids.push(h('div', { class: 'ctnote bad' }, h('div', {}, r.error),
      r.cors ? h('button', { class: 'btn', onclick: () => { setMode('board'); sendCurrent(); } }, tr('Mandarlo vía la placa')) : null));
  }
  if (r.cut) kids.push(h('div', { class: 'ctnote warn' }, tr('La placa guardó sólo una parte del cuerpo (%s): lo que sigue no está.', fmtSize(r.bytes ? r.bytes.length : 0))));
  if (r.note) kids.push(h('div', { class: 'ctnote warn' }, tr('No se pudo leer el cuerpo de la tarjeta: %s', r.note)));
  const tabBox = h('div', {});
  const tabsBox = h('div', {});
  const tabList = () => [
    r.ok ? { id: 'body', label: tr('Cuerpo') } : null,
    r.ok ? { id: 'headers', label: tr('Encabezados'), count: r.headers.length } : null,
    { id: 'tests', label: total ? tr('Tests %d/%d', passed, total) : tr('Tests'), dot: false },
  ];
  if (!r.ok) S.resTab = 'tests';
  const drawTabs = () => {
    put(tabsBox, h('div', { style: 'display:flex;align-items:flex-end;gap:8px;flex-wrap:wrap' },
      h('div', { style: 'flex:1;min-width:200px' }, tabs(tabList(), S.resTab, id => { S.resTab = id; drawTabs(); drawTab(); })),
      r.ok ? respActions(ctx) : h('div', { class: 'ctacts', style: 'margin-bottom:8px' },
        h('button', { class: 'btn ctsmall', onclick: () => copyText(toCurl(ctx.final), tr('Comando copiado')) }, tr('Copiar como cURL')))));
    const tb = tabsBox.querySelector('.cttabs button:last-child');
    if (tb && total) tb.style.color = failed ? 'var(--red)' : 'var(--green)';
  };
  const drawTab = () => {
    if (S.resTab === 'headers') put(tabBox, headersPanel(r));
    else if (S.resTab === 'tests') put(tabBox, testsPanel(ctx));
    else put(tabBox, bodyPanel(r, ctx.final));
  };
  kids.push(tabsBox, tabBox);
  put(box, kids);
  drawTabs();
  drawTab();
}

function respActions(ctx) {
  const r = ctx.resp, ct = hget(r.headers, 'content-type') || '';
  const kind = r.binary ? 'bin' : kindOf(ct, r.text);
  const blob = () => new Blob([r.bytes || r.text || ''], { type: ct || 'application/octet-stream' });
  return h('div', { class: 'ctacts', style: 'margin-bottom:8px' },
    h('button', { class: 'btn ctsmall', disabled: r.binary || !r.text, onclick: () => copyText(r.text, tr('Cuerpo copiado')) }, tr('Copiar')),
    h('button', { class: 'btn ctsmall', disabled: !r.size, onclick: () => P.saveBlob(blob(), fileNameFor(ctx.final, kind, ct)) }, tr('Descargar')),
    h('button', { class: 'btn ctsmall', disabled: !r.size, onclick: guard(async () => {
      const n = prompt(tr('Nombre del archivo en %s', D_RESP), fileNameFor(ctx.final, kind, ct));
      if (!n || !n.trim()) return;
      const name = n.trim().replace(/[^A-Za-z0-9._-]+/g, '_').replace(/^\.+/, '') || 'respuesta';
      await ensureDirs();
      await P.fsPut(D_RESP + '/' + name, blob());
      toast(tr('Guardado en %s', D_RESP + '/' + name));
    }) }, tr('Guardar en la tarjeta')),
    h('button', { class: 'btn ctsmall', onclick: () => copyText(toCurl(ctx.final), tr('Comando copiado')) }, tr('Copiar como cURL')));
}

function headersPanel(r) {
  const grid = h('div', { class: 'cthdrs' });
  for (const [k, v] of r.headers) grid.append(h('div', {}, k), h('div', {}, v));
  return h('div', {}, r.headers.length ? grid : h('p', { class: 'muted' }, tr('Sin encabezados.')),
    r.via === 'direct' ? h('p', { class: 'note' }, tr('Desde el navegador se ven sólo los encabezados que el servidor deja ver (CORS, Access-Control-Expose-Headers). Vía la placa se ven todos.')) : null);
}

function testsPanel(ctx) {
  const box = h('div', {});
  const tests = ctx.tests;
  const ok = tests.filter(t => t.ok && !t.skipped).length, bad = tests.filter(t => !t.ok).length, sk = tests.filter(t => t.skipped).length;
  if (tests.length) {
    box.append(h('div', { class: 'ctmeta' },
      h('span', { class: 'ok' }, tr('%d pasaron', ok)), h('span', { class: bad ? 'bad' : 'muted' }, tr('%d fallaron', bad)),
      sk ? h('span', { class: 'muted' }, tr('%d salteados', sk)) : null));
    box.append(h('div', { class: 'cttests' }, tests.map(t => h('div', { class: 'cttest' },
      h('span', { class: t.skipped ? 'ctsk' : t.ok ? 'ctok' : 'ctko' }, t.skipped ? '–' : t.ok ? '✓' : '✗'),
      h('div', {}, h('div', {}, t.name), t.error ? h('div', { class: 'ctterr' }, t.error) : null)))));
  } else if (ctx.resp && !ctx.resp.ok) {
    box.append(h('p', { class: 'muted' }, tr('Sin respuesta: los tests no corrieron.')));
  } else if (!ctx.errors.length) {
    box.append(h('p', { class: 'muted' }, tr('Este pedido no tiene tests. Se escriben en Scripts, Tests.')));
  }
  for (const e of ctx.errors) box.append(h('div', { class: 'ctscripterr' }, '⚠ ' + tr('Error en el script (%s, %s): %s', e.phase === 'test' ? tr('tests') : tr('pre-request'), e.where, e.error)));
  if (ctx.console.length) {
    box.append(h('div', { class: 'ctcons' }, ctx.console.map(l => h('div', { class: 'ctl-' + l.level }, l.where ? h('span', { class: 'ctwh' }, l.where) : null, l.text))));
  }
  return box;
}

function bodyPanel(r, final) {
  const ct = hget(r.headers, 'content-type') || '';
  const kind = r.binary ? (/^image\//i.test(ct) ? 'image' : 'bin') : kindOf(ct, r.text);
  if (!r.size && !r.text) return h('p', { class: 'muted' }, tr('Sin cuerpo.'));
  const views = [['pretty', tr('Lindo')], ['raw', tr('Crudo')], ['preview', tr('Vista')]];
  const seg = () => h('div', { class: 'ctseg', style: 'margin-bottom:8px' }, views.map(([v, l]) =>
    h('button', { class: S.bodyView === v ? 'on' : null, onclick: () => { S.bodyView = v; draw(); } }, l)));
  const big = r.text.length > 2000000;
  const pre = t => h('pre', { class: 'ctpre' }, big ? t.slice(0, 2000000) : t);
  const bigNote = () => big ? h('p', { class: 'note' }, tr('Se muestran los primeros 2 MB: el resto está en Descargar.')) : null;
  const image = () => {
    if (!S.blobUrl) S.blobUrl = URL.createObjectURL(new Blob([r.bytes], { type: ct }));
    return h('img', { class: 'ctimg', src: S.blobUrl, alt: '' });
  };
  const view = () => {
    const v = S.bodyView;
    if (kind === 'image') return v === 'raw' ? h('p', { class: 'muted' }, tr('Una imagen de %s (%s).', fmtSize(r.size), ct)) : image();
    if (kind === 'bin') return h('p', { class: 'muted' }, tr('Binario, %s (%s): se puede descargar o guardar en la tarjeta.', fmtSize(r.size), ct || '?'));
    if (v === 'raw') return h('div', {}, pre(r.text), bigNote());
    if (v === 'preview') {
      if (kind === 'html') return h('iframe', { class: 'ctframe', sandbox: '', referrerpolicy: 'no-referrer', srcdoc: r.text, title: tr('Vista') });
      if (kind === 'json') { try { return pre(JSON.stringify(JSON.parse(r.text), null, 2)); } catch {} }
      return h('div', {}, pre(r.text), bigNote());
    }
    if (kind === 'json') {
      let j;
      try { j = JSON.parse(r.text.replace(/^﻿/, '')); } catch (e) {
        return h('div', {}, h('p', { class: 'note bad' }, tr('Dice ser JSON pero no lo es: %s', e.message)), pre(r.text));
      }
      return jsonView(j, final);
    }
    if (kind === 'xml' || kind === 'html') return h('div', {}, pre(xmlPretty(big ? r.text.slice(0, 2000000) : r.text)), bigNote());
    return h('div', {}, pre(r.text), bigNote());
  };
  const wrap = h('div', {});
  const draw = () => {
    if (S.blobUrl) { URL.revokeObjectURL(S.blobUrl); S.blobUrl = null; }
    put(wrap, seg(), view());
  };
  draw();
  return wrap;
}

/* JSON as a tree: open to two levels, a filter, a click on a value copies
 * its path for a script, "→ variable" keeps it in the environment */
function jsonView(root) {
  const st = { all: false, none: false, opened: new Set(), closed: new Set(), q: '', limit: new Map(), keep: null, full: null };
  const pk = p => p.join('\u0001');
  const isOpen = (p, depth) => {
    if (st.keep) return st.keep.has(pk(p)) || st.full.has(pk(p));
    const k = pk(p);
    if (st.opened.has(k)) return true;
    if (st.closed.has(k)) return false;
    return st.all || (!st.none && depth < 2);
  };
  const toggle = (p, open) => {
    const k = pk(p);
    if (open) { st.closed.add(k); st.opened.delete(k); } else { st.opened.add(k); st.closed.delete(k); }
    draw();
  };
  const tree = h('div', { class: 'ctjt' });
  const mark = txt => {
    if (!st.q) return txt;
    const lo = txt.toLowerCase(), out = [];
    let i = 0, j;
    while ((j = lo.indexOf(st.q, i)) >= 0) { out.push(txt.slice(i, j), h('mark', { class: 'ctjhit' }, txt.slice(j, j + st.q.length))); i = j + st.q.length; }
    out.push(txt.slice(i));
    return out;
  };
  const scan = () => {
    const q = st.q, keep = new Set(), full = new Set();
    let budget = 300000;
    const go = (v, p) => {
      if (--budget < 0) return false;
      let hit = false;
      if (v && typeof v === 'object') {
        const ents = Array.isArray(v) ? v.map((x, i) => [i, x]) : Object.entries(v);
        for (const [k, x] of ents) {
          const cp = p.concat(k);
          const kh = typeof k === 'string' && k.toLowerCase().includes(q);
          if (kh) full.add(pk(cp));
          if (go(x, cp) || kh) { hit = true; keep.add(pk(cp)); }
        }
      } else hit = String(v).toLowerCase().includes(q);
      return hit;
    };
    go(root, []);
    keep.add(pk([]));
    st.keep = keep;
    st.full = full;
  };
  const node = (k, v, p, depth, inFull) => {
    const row = h('div', { class: 'ctjr' });
    const keyEl = k === null ? null : h('span', { class: 'ctjk', title: jsPath(p), style: 'cursor:copy',
      onclick: () => copyText(jsPath(p), tr('Copiado: %s', jsPath(p))) }, typeof k === 'number' ? String(k) : mark(JSON.stringify(k)), ':');
    if (v && typeof v === 'object') {
      const arr = Array.isArray(v), n = arr ? v.length : Object.keys(v).length;
      const open = n > 0 && (inFull || isOpen(p, depth));
      row.append(h('span', { class: 'ctcar', onclick: () => n && toggle(p, open) }, n ? open ? '▾' : '▸' : ''), keyEl || '',
        h('span', { class: 'ctj-m', style: n ? 'cursor:pointer' : '', onclick: () => n && toggle(p, open) },
          open ? (arr ? '[' : '{') : arr ? (n ? `[ ${tr('%d elementos', n)} ]` : '[]') : (n ? `{ ${tr('%d claves', n)} }` : '{}')));
      if (!open) return row;
      const wrap = h('div', {}, row);
      const kids = h('div', { class: 'ctjn' });
      const ents = arr ? v.map((x, i) => [i, x]) : Object.entries(v);
      const lim = st.limit.get(pk(p)) || 200;
      let shown = 0, i = 0;
      for (; i < ents.length; i++) {
        const [ck, cv] = ents[i];
        const cp = p.concat(ck);
        if (st.keep && !inFull && !st.keep.has(pk(cp))) continue;
        if (shown >= lim) break;
        kids.append(node(ck, cv, cp, depth + 1, inFull || (st.full && st.full.has(pk(cp)))));
        shown++;
      }
      if (i < ents.length) kids.append(h('button', { class: 'ctjmore', onclick: () => { st.limit.set(pk(p), lim + 500); draw(); } }, tr('… %d más', ents.length - i)));
      wrap.append(kids, h('div', { class: 'ctj-m', style: 'padding-left:15px' }, arr ? ']' : '}'));
      return wrap;
    }
    const cls = v === null ? 'ctj-z' : typeof v === 'string' ? 'ctj-s' : typeof v === 'number' ? 'ctj-n' : typeof v === 'boolean' ? 'ctj-b' : '';
    const txt = typeof v === 'string' ? JSON.stringify(v) : String(v);
    row.append(h('span', { class: 'ctcar' }), keyEl || '',
      h('span', { class: 'ctjv ' + cls, title: tr('Clic: copia %s', jsPath(p)), onclick: () => copyText(jsPath(p), tr('Copiado: %s', jsPath(p))) }, mark(txt)),
      h('button', { class: 'ctjvar', title: tr('Guardar este valor en una variable del entorno activo'), onclick: guard(() => toVar(p, v)) }, '→ ' + tr('variable')));
    return row;
  };
  const draw = () => put(tree, node(null, root, [], 0, false));
  const filter = h('input', { type: 'search', placeholder: tr('Filtrar claves y valores'), style: 'flex:1;min-width:140px;padding:6px 10px;font-size:13px',
    oninput: debounce(() => { st.q = filter.value.trim().toLowerCase(); if (st.q) scan(); else { st.keep = st.full = null; } draw(); }, 200) });
  draw();
  return h('div', {},
    h('div', { class: 'ctrow' }, filter,
      h('button', { class: 'btn ctsmall', onclick: () => { st.all = false; st.none = true; st.opened.clear(); st.closed.clear(); draw(); } }, tr('Cerrar todo')),
      h('button', { class: 'btn ctsmall', onclick: () => { st.all = true; st.none = false; st.opened.clear(); st.closed.clear(); draw(); } }, tr('Abrir todo'))),
    tree);
}

/* a value of the answer into a variable: the active environment's, or the
 * globals when there is none */
async function toVar(p, v) {
  const last = [...p].reverse().find(k => typeof k === 'string') || 'valor';
  const e = activeEnvEntry();
  const name = prompt(e ? tr('Nombre de la variable en «%s»', nameOf(e.data)) : tr('No hay entorno activo: va a las globales. Nombre de la variable'), last);
  if (!name || !name.trim()) return;
  const ops = new Map([[name.trim(), { v }]]);
  await ensureDirs();
  if (e) {
    applyOps(e.data.values || (e.data.values = []), ops, true);
    e.data = await mergeVarsFile(D_ENVS + '/' + e.file, ops, true, 'values', () => e.data);
  } else {
    if (!S.glob) S.glob = { name: 'Globales', values: [], _postman_variable_scope: 'globals' };
    applyOps(S.glob.values || (S.glob.values = []), ops, true);
    S.glob = await mergeVarsFile(F_GLOB, ops, true, 'values', () => S.glob);
  }
  tellBoard();
  varsChanged();
  toast(tr('{{%s}} = %s', name.trim(), fileVal(v).slice(0, 60)));
}

/* ---- the runner ---------------------------------------------------------------------------
 * A collection or a folder, its requests in order (depth first), as many
 * times as asked, honouring setNextRequest (null ends the iteration). Each
 * step is the same execute() as a request sent by hand; the variables its
 * scripts change are merged before the next one. The result goes to
 * corridas/<yyyymmdd-hhmmss>-<slug>.json, in the README's format. */

function flatten(node, base) {
  const out = [];
  const go = (n, p) => (n.item || []).forEach((it, i) => {
    const cp = p.concat(i);
    if (isFolder(it)) go(it, cp); else out.push({ path: cp, item: it });
  });
  go(node, base);
  return out;
}

function openRunner(file, path) {
  const first = S.cols.find(c => c.data);
  const cfg = S.runCfg || { iterations: 1, delay: 0, stopOnFail: false };
  if (file !== undefined) { cfg.file = file; cfg.path = path || []; }
  if (!cfg.file || !colEntry(cfg.file)) { cfg.file = first ? first.file : null; cfg.path = []; }
  S.runCfg = cfg;
  S.view = 'run';
  drawMain();
  drawSideBody();
  foldOnNarrow();
}

function drawRunner(box) {
  const cfg = S.runCfg;
  const running = S.run && S.run.running;
  const sel = h('select', { disabled: running, onchange: e => { const [f, p] = e.target.value.split('|'); cfg.file = f; cfg.path = p ? p.split('.').map(Number) : []; } });
  for (const c of S.cols) {
    if (!c.data) continue;
    sel.append(h('option', { value: c.file + '|', selected: cfg.file === c.file && !cfg.path.length }, nameOf(c.data)));
    walk(c.data, (n, p) => {
      if (isFolder(n)) sel.append(h('option', { value: c.file + '|' + p.join('.'), selected: cfg.file === c.file && samePath(cfg.path, p) },
        '  '.repeat(p.length) + '▸ ' + nameOf(n)));
    });
  }
  const it = h('input', { type: 'number', min: 1, max: 1000, value: cfg.iterations, disabled: running, onchange: e => { cfg.iterations = Math.max(1, Math.min(1000, parseInt(e.target.value, 10) || 1)); } });
  const dl = h('input', { type: 'number', min: 0, max: 600000, step: 100, value: cfg.delay, disabled: running, onchange: e => { cfg.delay = Math.max(0, parseInt(e.target.value, 10) || 0); } });
  const sf = h('input', { type: 'checkbox', checked: cfg.stopOnFail, disabled: running, onchange: e => { cfg.stopOnFail = e.target.checked; } });
  const e = activeEnvEntry();
  S.dom.runRes = h('div', {});
  put(box,
    h('div', { class: 'ctcard' },
      h('div', { class: 'cthead' }, h('span', { class: 'ctname', style: 'font-size:18px;font-weight:650;flex:1;padding:4px 0' }, tr('Runner')),
        running ? h('button', { class: 'btn red', onclick: stopRun }, '■ ' + tr('Detener'))
          : h('button', { class: 'btn pri', disabled: !cfg.file, onclick: guard(startRun) }, '▶ ' + tr('Correr'))),
      S.cols.some(c => c.data) ? h('div', { class: 'ctcfg' },
        h('label', { style: 'grid-column:1/-1' }, tr('Colección o carpeta'), sel),
        h('label', {}, tr('Iteraciones'), it),
        h('label', {}, tr('Demora entre pedidos (ms)'), dl),
        h('label', { class: 'chk' }, sf, tr('Detener al primer fallo'))) : h('p', { class: 'muted' }, tr('No hay colecciones para correr.')),
      h('p', { class: 'note' }, tr('Con el entorno %s, %s. Los cambios de variables de los scripts se guardan; los pedidos no van al historial.',
        e ? '«' + nameOf(e.data) + '»' : tr('ninguno'), S.mode === 'board' ? tr('vía la placa') : tr('desde el navegador')))),
    S.dom.runRes);
  drawRunRes();
}

function drawRunRes() {
  const box = S.dom.runRes;
  if (!box || !box.isConnected) return;
  const run = S.run;
  if (!run) { put(box); return; }
  put(box, runTable(run.result, run.running, run));
}

/* the table of a run, live or read from its file */
function runTable(res, running, run) {
  const items = res.items || [];
  const fails = it => !!it.error || (it.tests || []).some(t => !t.ok);
  const open = (run || S.runView || {}).openRows || new Set();
  const tbody = h('tbody', {});
  items.forEach((it, i) => {
    const okT = (it.tests || []).filter(t => t.ok && !t.skipped).length, badT = (it.tests || []).filter(t => !t.ok).length;
    const tr_ = h('tr', { class: 'ctrr', onclick: () => { open.has(i) ? open.delete(i) : open.add(i); if (run) drawRunRes(); else drawMain(); } },
      h('td', { class: 'num muted hide-n' }, String(i + 1)),
      h('td', {}, h('div', { style: 'display:flex;gap:6px;align-items:baseline' }, methodBadge(it.method || 'GET'), h('span', {}, it.name)),
        it.iteration ? h('div', { class: 'muted small' }, tr('iteración %d', it.iteration)) : null),
      h('td', {}, it.running ? h('span', { class: 'ctspin' }) : it.skipped ? h('span', { class: 'muted small' }, tr('salteado')) : statusPill(it.status, '')),
      h('td', { class: 'num' }, it.running ? '' : fmtMs(it.ms)),
      h('td', { class: 'num' }, it.running ? '' : h('span', {}, h('span', { class: 'ok' }, String(okT)), ' / ', h('span', { class: badT ? 'bad' : 'muted' }, String(badT)))),
      h('td', { class: 'num' }, fails(it) ? h('span', { class: 'bad' }, '✗') : it.running || it.skipped ? '' : h('span', { class: 'ok' }, '✓')));
    tbody.append(tr_);
    if (open.has(i)) {
      tbody.append(h('tr', { class: 'ctrdet' }, h('td', { colspan: 6 },
        h('div', { class: 'mono small', style: 'margin-bottom:4px' }, it.url || ''),
        it.error ? h('div', { class: 'ctscripterr' }, it.error) : null,
        (it.tests || []).map(t => h('div', { class: 'cttest' }, h('span', { class: t.skipped ? 'ctsk' : t.ok ? 'ctok' : 'ctko' }, t.skipped ? '–' : t.ok ? '✓' : '✗'),
          h('div', {}, t.name, t.error ? h('div', { class: 'ctterr' }, t.error) : null))),
        (it.console || []).length ? h('div', { class: 'ctcons' }, it.console.map(l => h('div', { class: 'ctl-' + l.level }, l.text))) : null)));
    }
  });
  const pct = running && run && run.expected ? Math.min(100, items.filter(x => !x.running).length / run.expected * 100) : 100;
  return h('div', { class: 'ctcard' },
    h('div', { class: 'cthead' }, h('b', { style: 'flex:1' }, res.collection + (res.folder ? ' › ' + res.folder : '')),
      h('span', { class: 'muted small' }, (res.environment ? tr('entorno «%s»', res.environment) + ' · ' : '') + (res.t ? new Date(res.t * 1000).toLocaleString(P.lang ? P.lang() : undefined) : '')),
      run && run.saved ? h('span', { class: 'muted small' }, tr('guardada en %s', run.saved)) : null),
    running ? h('div', { class: 'prog' }, h('div', { style: `width:${pct}%` })) : null,
    h('div', { class: 'ctsum' },
      h('div', {}, h('b', {}, String(res.requests)), h('span', {}, tr('pedidos'))),
      h('div', {}, h('b', {}, String(res.tests)), h('span', {}, tr('tests'))),
      h('div', {}, h('b', { class: 'ok' }, String(res.passed)), h('span', {}, tr('pasaron'))),
      h('div', {}, h('b', { class: res.failed ? 'bad' : '' }, String(res.failed)), h('span', {}, tr('fallaron'))),
      h('div', {}, h('b', {}, fmtMs(res.ms)), h('span', {}, tr('en total')))),
    run && run.stopped ? h('div', { class: 'ctnote warn' }, run.stopped) : null,
    items.length ? h('div', { style: 'overflow-x:auto' }, h('table', { class: 'ctrun' },
      h('thead', {}, h('tr', {}, h('th', { class: 'hide-n' }, '#'), h('th', {}, tr('Pedido')), h('th', {}, tr('Estado')), h('th', { class: 'num' }, 'ms'), h('th', { class: 'num' }, tr('Tests')), h('th', {}, ''))),
      tbody)) : h('p', { class: 'muted' }, running ? tr('Empezando…') : tr('Nada que correr.')));
}

function sumUp(res) {
  res.requests = res.items.filter(i => !i.skipped && !i.running).length;
  res.tests = res.items.reduce((a, i) => a + (i.tests || []).filter(t => !t.skipped).length, 0);
  res.passed = res.items.reduce((a, i) => a + (i.tests || []).filter(t => t.ok && !t.skipped).length, 0);
  res.failed = res.items.reduce((a, i) => a + (i.tests || []).filter(t => !t.ok).length, 0);
}

function stopRun() {
  if (!S.run || !S.run.running) return;
  S.run.stop = true;
  if (S.run.token.cancel) S.run.token.cancel();
  if (!S.run.stopped) S.run.stopped = tr('Detenida a mano.');
}

async function startRun() {
  if (S.run && S.run.running) return;
  if (S.sending) { toast(tr('Hay un pedido en camino'), true); return; }
  const cfg = S.runCfg, c = colEntry(cfg.file);
  if (!c || !c.data) return;
  const folderNode = cfg.path.length ? nodeAt(c.data, cfg.path) : null;
  if (cfg.path.length && !folderNode) { toast(tr('Esa carpeta ya no está'), true); return; }
  const e = activeEnvEntry();
  const t0 = Date.now();
  const res = { collection: nameOf(c.data), folder: folderNode ? nameOf(folderNode) : '', environment: e ? nameOf(e.data) : '',
    t: Math.floor(t0 / 1000), ms: 0, requests: 0, tests: 0, passed: 0, failed: 0, items: [] };
  const run = S.run = { running: true, stop: false, token: {}, result: res, openRows: new Set(), expected: 0, stopped: '', saved: '' };
  const local = new Map();
  if (S.view === 'run') drawMain();
  try {
    outer:
    for (let iter = 1; iter <= cfg.iterations && !run.stop; iter++) {
      /* each iteration reads the collection as it is now (a script may have changed its variables) */
      const col = colEntry(cfg.file).data;
      const list = flatten(cfg.path.length ? nodeAt(col, cfg.path) : col, cfg.path);
      if (iter === 1) run.expected = list.length * cfg.iterations;
      let i = 0, steps = 0;
      while (i < list.length && !run.stop) {
        if (++steps > 10000) { run.stopped = tr('Más de 10000 pasos en una iteración: ¿un setNextRequest en ronda?'); run.stop = true; break; }
        const { path, item } = list[i];
        const row = { name: nameOf(item), method: reqMethod(item), url: '', status: 0, ms: 0, size: 0, error: '', tests: [], running: true };
        if (cfg.iterations > 1) row.iteration = iter;
        res.items.push(row);
        drawRunRes();
        let ctx;
        try {
          run.token = {};
          ctx = await execute({ col: colEntry(cfg.file).data, path, item: normItem(item), local, iteration: iter - 1, iterations: cfg.iterations,
            mode: S.mode, timeout: S.timeout, follow: S.follow, token: run.token });
        } catch (err) {
          delete row.running;
          row.error = errText(err);
          if (err instanceof NotOpenError) { run.stopped = tr('Cartero no está abierta en la placa: la corrida se detuvo.'); run.stop = true; }
          sumUp(res);
          drawRunRes();
          break outer;
        }
        delete row.running;
        if (ctx.skip && !ctx.resp) row.skipped = true;
        const r = ctx.resp;
        if (ctx.final) row.url = ctx.final.url;
        if (r) { row.status = r.ok ? r.status : 0; row.ms = Math.round(r.ms || 0); row.size = r.size || 0; if (!r.ok) row.error = r.error || ''; }
        row.tests = ctx.tests.map(t => Object.assign({ name: t.name, ok: t.ok, error: t.error || '' }, t.skipped ? { skipped: true } : {}));
        if (ctx.errors.length) row.error = [row.error].concat(ctx.errors.map(x => tr('Error en el script (%s, %s): %s', x.phase === 'test' ? tr('tests') : tr('pre-request'), x.where, x.error))).filter(Boolean).join('\n');
        if (ctx.console.length) row.console = ctx.console.map(l => ({ level: l.level, text: l.text }));
        sumUp(res);
        res.ms = Date.now() - t0;
        try { await flushVars(ctx, cfg.file); } catch {}
        drawRunRes();
        if (cfg.stopOnFail && (row.error || row.tests.some(t => !t.ok))) { run.stopped = tr('Detenida en el primer fallo.'); run.stop = true; break; }
        if (ctx.next !== undefined) {
          if (ctx.next === null) break;
          const j = list.findIndex(x => nameOf(x.item) === ctx.next || x.item.id === ctx.next);
          if (j < 0) { run.stopped = tr('setNextRequest: no hay un pedido «%s»; la iteración terminó.', ctx.next); break; }
          i = j;
        } else i++;
        if (cfg.delay && !run.stop && (i < list.length || iter < cfg.iterations)) {
          for (let w = 0; w < cfg.delay && !run.stop; w += 100) await sleep(Math.min(100, cfg.delay - w));
        }
      }
    }
  } finally {
    run.running = false;
    res.items = res.items.filter(x => !x.running);
    sumUp(res);
    res.ms = Date.now() - t0;
  }
  const file = stamp(new Date(t0)) + '-' + P.fsSlug(res.collection + (res.folder ? '-' + res.folder : ''), 40, 'corrida') + '.json';
  try {
    await ensureDirs();
    const out = clone(res);
    for (const it of out.items) delete it.console;
    await writeJSON(D_RUNS + '/' + file, out);
    run.saved = D_RUNS + '/' + file;
    S.runs = [{ name: file, dir: false, size: 0, mtime: res.t }].concat(S.runs.filter(x => x.name !== file));
  } catch (err) {
    toast(tr('No se pudo guardar la corrida: %s', errText(err)), true);
  }
  if (S.gone) return;
  if (S.view === 'run') drawMain(); else toast(tr('La corrida terminó: %d de %d tests pasaron', res.passed, res.tests));
  if (S.side === 'runs') drawSideBody();
}

async function openRunFile(name) {
  const data = await readJSON(D_RUNS + '/' + name);
  if (!data) throw new Error(tr('La corrida ya no está'));
  data.items = Array.isArray(data.items) ? data.items : [];
  S.runView = { file: name, data, openRows: new Set() };
  S.view = 'runres';
  drawMain();
  drawSideBody();
  foldOnNarrow();
}

/* ---- import and export ---------------------------------------------------------------- */

function drawImport(box) {
  const ta = h('textarea', { class: 'ctcode', style: 'min-height:260px', spellcheck: 'false',
    placeholder: tr('Pegá acá una colección o un entorno de Postman (JSON), o un comando curl') });
  const out = h('div', {});
  const file = h('input', { type: 'file', accept: '.json,.txt,.sh,application/json,text/plain', style: 'display:none', onchange: async () => {
    const f = file.files[0];
    if (!f) return;
    ta.value = await f.text();
    file.value = '';
    guard(() => doImport(ta.value, out))();
  } });
  put(box, h('div', { class: 'ctcard' },
    h('div', { class: 'cthead' }, h('span', { class: 'ctname', style: 'font-size:18px;font-weight:650;flex:1;padding:4px 0' }, tr('Importar'))),
    h('p', { class: 'note', style: 'margin-top:0' }, tr('Una colección de Postman (v2.0 o v2.1) queda en la tarjeta como un archivo más; un entorno, también; un comando curl se abre como un pedido nuevo, para guardarlo donde quieras.')),
    ta,
    h('div', { class: 'btns', style: 'margin-top:10px' },
      h('button', { class: 'btn pri', onclick: guard(() => doImport(ta.value, out)) }, tr('Importar')),
      h('button', { class: 'btn', onclick: () => file.click() }, tr('Elegir un archivo…')), file),
    out));
}

async function doImport(text, out) {
  const t = (text || '').trim();
  if (!t) { toast(tr('No hay nada que importar'), true); return; }
  if (/^curl(\.exe)?\s/i.test(t)) {
    const item = parseCurl(t);
    openScratch(item);
    toast(tr('El comando quedó como un pedido nuevo: guardalo con Guardar como…'));
    return;
  }
  let j;
  try { j = JSON.parse(t); } catch (e) { throw new Error(tr('No es JSON ni un comando curl: %s', e.message)); }
  if (j && j.collection && j.collection.info) j = j.collection;
  if (j && j.environment && Array.isArray(j.environment.values)) j = j.environment;
  if (j && j.info && (Array.isArray(j.item) || j.info.schema)) {
    const schema = String(j.info.schema || '');
    if (/v1\.0/.test(schema) || (!schema && j.requests)) throw new Error(tr('Es una colección de Postman v1: exportala de nuevo como v2.1.'));
    if (/v2\.0/.test(schema)) upgradeV20(j);
    else j.info.schema = SCHEMA;
    if (!Array.isArray(j.item)) j.item = [];
    if (!j.info.name) j.info.name = tr('Importada');
    if (!j.info._postman_id) j.info._postman_id = uuid();
    const file = await createCollection(j.info.name, j);
    S.open.add(file + ':');
    S.side = 'col';
    S.dom.sideFor = null;
    drawSide();
    let n = 0;
    walk(j, it => { if (!isFolder(it)) n++; });
    put(out, h('div', { class: 'ctnote' }, tr('Colección «%s» importada: %d pedidos, en %s.', j.info.name, n, D_COLS + '/' + file)));
    return;
  }
  if (j && Array.isArray(j.values)) {
    if (j._postman_variable_scope === 'globals') {
      if (!confirm(tr('Son globales: ¿agregarlas a las de la tarjeta? Las que tengan el mismo nombre se reemplazan.'))) return;
      await ensureDirs();
      let g = null;
      try { g = await readJSON(F_GLOB); } catch {}
      if (!isObj(g)) g = { name: 'Globales', values: [], _postman_variable_scope: 'globals' };
      const ops = new Map(j.values.filter(v => v && v.key).map(v => [v.key, { v: v.value }]));
      applyOps(g.values || (g.values = []), ops, true);
      await writeJSON(F_GLOB, g);
      S.glob = g;
      tellBoard();
      put(out, h('div', { class: 'ctnote' }, tr('%d globales importadas.', ops.size)));
      return;
    }
    const data = { id: j.id || uuid(), name: j.name || tr('Importado'), values: j.values.filter(v => v && v.key != null).map(v => ({
      key: v.key, value: valStr(v.value), type: v.type === 'secret' ? 'secret' : 'default', enabled: v.enabled !== false })),
      _postman_variable_scope: 'environment' };
    const file = await writeNewEnv(data);
    S.side = 'env';
    S.dom.sideFor = null;
    drawSide();
    put(out, h('div', { class: 'ctnote' }, tr('Entorno «%s» importado: %d variables.', data.name, data.values.length),
      ' ', h('button', { class: 'btn ctsmall', onclick: () => openEnv(file) }, tr('Abrirlo'))));
    return;
  }
  throw new Error(tr('No reconozco ese JSON: ni una colección ni un entorno de Postman.'));
}

function exportCollection(c) {
  const data = clone(c.data);
  if (data.info) data.info.schema = data.info.schema || SCHEMA;
  P.saveBlob(new Blob([JSON.stringify(data, null, '\t')], { type: 'application/json' }), P.fsSlug(nameOf(c.data), 60, 'coleccion') + '.postman_collection.json');
}

function exportEnv(data) {
  const d = clone(data);
  if (!d._postman_variable_scope) d._postman_variable_scope = 'environment';
  P.saveBlob(new Blob([JSON.stringify(d, null, '\t')], { type: 'application/json' }), P.fsSlug(nameOf(d) || 'globales', 60, 'entorno') + '.postman_environment.json');
}

/* ---- the page ------------------------------------------------------------------------------ */

function setMode(m) {
  S.mode = m;
  lsSet('mode', m);
  drawTop();
  if (S.view === 'req' && S.cur && S.cur.kind === 'request' && S.reqTab === 'headers') drawAuto();
}

function drawTop() {
  const d = S.dom;
  if (!d.envSel) return;
  put(d.envSel, h('option', { value: '' }, tr('Sin entorno')),
    S.envs.filter(e => e.data).map(e => h('option', { value: slugOf(e.file), selected: slugOf(e.file) === S.active }, nameOf(e.data) || slugOf(e.file))));
  d.envSel.value = S.envs.some(e => slugOf(e.file) === S.active) ? S.active : '';
  put(d.mode,
    h('button', { class: S.mode === 'board' ? 'on' : null, title: tr('La placa hace el pedido: sin CORS, desde su red'), onclick: () => setMode('board') }, tr('Vía la placa')),
    h('button', { class: S.mode === 'direct' ? 'on' : null, title: tr('El navegador hace el pedido: hace falta que el servidor permita CORS'), onclick: () => setMode('direct') }, tr('Directo desde el navegador')));
}

function settings() {
  const to = h('input', { type: 'text', inputmode: 'numeric', value: String(S.timeout) });
  const fo = h('input', { type: 'checkbox', checked: S.follow });
  modal(tr('Ajustes de los pedidos'), h('div', {},
    h('label', {}, tr('Tiempo de espera (ms)')), to,
    h('label', { style: 'display:flex;gap:8px;align-items:center;color:var(--text);margin-top:14px' }, fo, tr('Seguir redirecciones')),
    h('p', { class: 'note' }, tr('Se guardan en este navegador.'))), tr('Listo'), () => {
    const v = parseInt(to.value, 10);
    if (!(v >= 500 && v <= 300000)) { toast(tr('El tiempo va de 500 a 300000 ms'), true); return false; }
    S.timeout = v;
    S.follow = fo.checked;
    lsSet('timeout', v);
    lsSet('follow', fo.checked);
    return true;
  });
}

function drawMain() {
  const box = S.dom.main;
  closeMenu();
  S.dom.url = null;
  S.dom.auto = null;
  if (!S.loaded) { put(box, h('div', { class: 'ctcard' }, h('p', { class: 'muted' }, tr('Leyendo la tarjeta...')))); return; }
  if (S.view === 'req' && S.cur) return drawEditor(box);
  if (S.view === 'env' && S.envEd) return drawEnv(box);
  if (S.view === 'run') return drawRunner(box);
  if (S.view === 'runres' && S.runView) {
    const d = S.runView.data;
    return put(box, h('div', { class: 'btns', style: 'margin-bottom:10px' },
      h('button', { class: 'btn ctsmall', onclick: () => {
        const c = S.cols.find(x => x.data && nameOf(x.data) === d.collection);
        if (c) { let p = []; if (d.folder) walk(c.data, (n, q) => { if (isFolder(n) && nameOf(n) === d.folder && !p.length) p = q; }); openRunner(c.file, p); } else openRunner();
      } }, '▶ ' + tr('Correr de nuevo')),
      h('button', { class: 'btn ctsmall', onclick: () => P.saveBlob(new Blob([JSON.stringify(d, null, '\t')], { type: 'application/json' }), S.runView.file) }, tr('Descargar'))),
      runTable(d, false, null));
  }
  if (S.view === 'import') return drawImport(box);
  put(box, h('div', { class: 'ctcard' },
    h('h2', { style: 'margin-top:0' }, tr('Cartero')),
    h('p', {}, tr('Un cliente de APIs como Postman: los pedidos en colecciones y carpetas, entornos con sus variables, tests en JavaScript y un runner. Todo queda en la tarjeta, en el formato de Postman, y la app de la placa lo usa igual.')),
    h('p', { class: 'muted small' }, tr('Elegí un pedido de la izquierda, o:')),
    h('div', { class: 'btns' },
      h('button', { class: 'btn pri', onclick: () => openScratch(newRequest(tr('Pedido nuevo'))) }, tr('Pedido nuevo')),
      h('button', { class: 'btn', onclick: guard(newCollection) }, tr('Colección nueva')),
      h('button', { class: 'btn', onclick: () => { S.view = 'import'; drawMain(); } }, tr('Importar de Postman o cURL')))));
}

/* the card again: what changed elsewhere (the board, another tab) */
async function refresh(quiet) {
  try {
    await ensureDirs();
    const [cols, envs] = await Promise.all([loadFolder(D_COLS, S.cols), loadFolder(D_ENVS, S.envs)]);
    S.cols = cols;
    sortCols();
    S.envs = envs;
    sortEnvs();
    try { S.glob = await readJSON(F_GLOB); } catch { }
    let st = null;
    try { st = await readJSON(F_STATE); } catch {}
    S.active = st && typeof st.entorno === 'string' ? st.entorno : '';
    try { const hh = await readJSON(F_HIST); S.hist = Array.isArray(hh) ? hh : []; } catch { S.hist = []; }
    S.runs = (await P.fsList(D_RUNS)).filter(e => !e.dir && /\.json$/i.test(e.name)).sort((a, b) => b.name.localeCompare(a.name));
  } catch (e) {
    if (!quiet) toast(errText(e), true);
  }
  S.loaded = true;
}

async function refreshOnFocus() {
  if (!S || S.gone || S.busyRefresh || (S.run && S.run.running) || S.sending) return;
  if ((S.cur && S.cur.dirty) || (S.envEd && S.envEd.dirty)) return;
  S.busyRefresh = true;
  const before = JSON.stringify([S.cols.map(c => [c.file, c.mtime, c.size]), S.envs.map(e => [e.file, e.mtime, e.size]), S.active, S.glob]);
  await refresh(true);
  S.busyRefresh = false;
  if (S.gone) return;
  const after = JSON.stringify([S.cols.map(c => [c.file, c.mtime, c.size]), S.envs.map(e => [e.file, e.mtime, e.size]), S.active, S.glob]);
  drawSideBody();
  if (before === after) return;
  drawTop();
  if (S.cur && !S.cur.dirty && S.cur.file) {
    const c = colEntry(S.cur.file);
    const loc = c && c.data && locate(c.data, S.cur.path, S.cur.origName, S.cur.kind !== 'request');
    if (loc) {
      const fresh = S.cur.kind === 'request' ? normItem(loc.node) : null;
      S.cur.path = loc.path;
      if (fresh && JSON.stringify(fresh) !== JSON.stringify(S.cur.item)) { S.cur.item = fresh; if (S.view === 'req') drawMain(); }
      else if (S.view === 'req' && S.dom.url) S.dom.url.paint();
    }
  }
  if (S.view === 'env' || S.view === 'req') varsChanged();
}

P.registerPage({
  id: 'cartero',
  name: tr('Cartero'),
  icon: '✉',
  render(main) {
    style();
    if (S) S.gone = true;
    S = {
      gone: false, loaded: false, cols: [], envs: [], glob: null, active: '', hist: [], runs: [],
      side: lsGet('side', 'col'), sideFold: false, filter: '', open: new Set(lsGet('open', [])),
      view: 'empty', cur: null, res: null, reqTab: 'params', resTab: 'body', bodyView: 'pretty', scriptTab: 'test', cTab: 'auth',
      envEd: null, run: null, runView: null, runCfg: null,
      mode: lsGet('mode', 'board') === 'direct' ? 'direct' : 'board', follow: lsGet('follow', true) !== false, timeout: +lsGet('timeout', 30000) || 30000,
      sending: null, blobUrl: null, dom: {},
    };
    if (!SIDE_TABS.some(([id]) => id === S.side)) S.side = 'col';
    const d = S.dom;
    d.envSel = h('select', { title: tr('El entorno activo (también en la placa)'), onchange: e => guard(() => setActive(e.target.value))() });
    d.mode = h('div', { class: 'ctseg' });
    d.sideTabs = h('div', { class: 'cttabs', style: 'border:0;margin:0' });
    d.sideToggle = h('button', { class: 'btn ctsmall ctsidetoggle', onclick: () => { S.sideFold = !S.sideFold; drawSide(); } });
    d.sideBar = h('div', { class: 'ctsidebar' });
    d.sideBody = h('div', { class: 'ctsidebody' });
    d.side = h('aside', { class: 'card ctside' }, h('div', { class: 'ctsidehead' }, d.sideTabs, d.sideToggle), d.sideBar, d.sideBody);
    d.main = h('section', { style: 'min-width:0' });
    d.root = h('div', { class: 'ct' },
      h('div', { class: 'cttop' }, h('h1', {}, tr('Cartero')),
        d.envSel, d.mode,
        h('button', { class: 'btn ctsmall', title: tr('Importar una colección, un entorno o un comando curl'), onclick: () => { S.view = 'import'; drawMain(); foldOnNarrow(); } }, tr('Importar')),
        h('button', { class: 'btn ctsmall', onclick: () => openRunner() }, '▶ ' + tr('Runner')),
        h('button', { class: 'btn ctsmall', title: tr('Ajustes de los pedidos'), 'aria-label': tr('Ajustes de los pedidos'), onclick: settings }, '⚙')),
      h('div', { class: 'ctlay' }, d.side, d.main));
    put(main, d.root);
    drawTop();
    drawSide();
    drawMain();
    const myS = S;
    refresh().then(() => {
      if (myS.gone) return;
      drawTop();
      drawSide();
      drawMain();
    });

    const onKey = e => {
      if (myS.gone || !d.root.isConnected) return;
      const mod = e.ctrlKey || e.metaKey;
      if (mod && !e.shiftKey && !e.altKey && (e.key === 's' || e.key === 'S')) {
        if (S.view === 'req' || S.view === 'env') { e.preventDefault(); guard(saveCurrent)(); }
      } else if (mod && e.key === 'Enter') {
        if (S.view === 'req' && S.cur && S.cur.kind === 'request') { e.preventDefault(); if (!S.sending) sendCurrent(); }
      } else if (e.key === 'Escape') closeMenu();
    };
    const onFocus = () => { refreshOnFocus(); };
    const onVis = () => { if (document.visibilityState === 'visible') refreshOnFocus(); };
    const leave = e => {
      if ((myS.cur && myS.cur.dirty) || (myS.envEd && myS.envEd.dirty) || (myS.run && myS.run.running)) { e.preventDefault(); e.returnValue = ''; }
    };
    document.addEventListener('keydown', onKey);
    window.addEventListener('focus', onFocus);
    document.addEventListener('visibilitychange', onVis);
    window.addEventListener('beforeunload', leave);
    return () => {
      myS.gone = true;
      if (myS.run && myS.run.running) { myS.run.stop = true; if (myS.run.token.cancel) myS.run.token.cancel(); }
      if (myS.sending && myS.sending.token.cancel) myS.sending.token.cancel();
      if (myS.blobUrl) URL.revokeObjectURL(myS.blobUrl);
      closeMenu();
      document.removeEventListener('keydown', onKey);
      window.removeEventListener('focus', onFocus);
      document.removeEventListener('visibilitychange', onVis);
      window.removeEventListener('beforeunload', leave);
    };
  },
});
