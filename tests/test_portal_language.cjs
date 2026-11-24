const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { createServer } = require('node:http');
const { once } = require('node:events');
const { gunzipSync } = require('node:zlib');
const vm = require('node:vm');
const { test } = require('node:test');
const root = path.join(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'components/aos_portal/web/app.js'), 'utf8');
const language = source.slice(source.indexOf("let LANG = 'es'"), source.indexOf('function h('));

function generatedCatalogs() {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'p4os-portal-'));
  const file = path.join(temp, 'assets.c');
  try {
    execFileSync(process.env.PYTHON || (process.platform === 'win32' ? 'python' : 'python3'),
      [path.join(root, 'tools/gen_web_assets.py'), file]);
    const c = fs.readFileSync(file, 'utf8'), arrays = new Map(), routes = new Map();
    for (const [, name, data] of c.matchAll(/uint8_t (a\d+)\[\d+\] = \{([^}]+)\}/g)) {
      arrays.set(name, Buffer.from(data.split(',').map(Number)));
    }
    for (const [, url, name] of c.matchAll(/\{ "([^"]+)", "[^"]+", (a\d+), \d+ \}/g)) {
      routes.set(url, arrays.get(name));
    }
    return routes;
  } finally {
    if (fs.existsSync(file)) fs.unlinkSync(file);
    fs.rmdirSync(temp);
  }
}
const builtin = generatedCatalogs();

function fixture(card = '') {
  const nav = { textContent: 'Pantalla', dataset: {} };
  const ctx = vm.createContext({
    document: { documentElement: {}, querySelectorAll: () => [nav], addEventListener() {} },
    fetch: async url => {
      if (url.startsWith('/lang/')) return { ok: !!card, text: async () => card };
      return { ok: builtin.has(url), text: async () => gunzipSync(builtin.get(url)).toString('utf8') };
    }
  });
  vm.runInContext(language, ctx);
  return { ctx, nav };
}

test('all seven embedded web languages work without SD, including login text', async () => {
  for (const code of ['en', 'de', 'fr', 'it', 'pt', 'zh-Hans', 'zh-Hant']) {
    const { ctx, nav } = fixture();
    await ctx.setLang(code);
    assert.equal(ctx.document.documentElement.lang, code);
    assert.notEqual(nav.textContent, 'Pantalla');
    assert.notEqual(ctx.tr('Esta red pide la contraseña del portal.'), 'Esta red pide la contraseña del portal.');
  }
});

test('browser fetch reads compressed firmware tables and a plain-text SD override over HTTP', async () => {
  const server = createServer((req, res) => {
    if (builtin.has(req.url)) {
      res.writeHead(200, { 'Content-Encoding': 'gzip', 'Content-Type': 'text/plain; charset=utf-8' });
      res.end(builtin.get(req.url));
    } else if (req.url === '/lang/zh-Hant/_portal.lang') {
      res.end('Pantalla\t自訂畫面\r\n');
    } else {
      res.writeHead(404).end();
    }
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  try {
    const origin = 'http://127.0.0.1:' + server.address().port;
    const { ctx, nav } = fixture();
    ctx.fetch = (url, options) => fetch(new URL(url, origin), options);
    await ctx.setLang('zh-Hant');
    assert.equal(nav.textContent, '自訂畫面');
    assert.equal(ctx.tr('Ajustes'), '設定');
    await ctx.setLang('zh-Hans');
    assert.equal(ctx.tr('Ajustes'), '设置');
  } finally {
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
});

test('partial SD translations override built-ins without clearing missing keys', async () => {
  const { ctx } = fixture('Pantalla\tCustom display\r\nInicio\t\r\n');
  await ctx.setLang('en');
  assert.equal(ctx.tr('Pantalla'), 'Custom display');
  assert.equal(ctx.tr('Inicio'), 'Home');
});

test('changing languages clears stale catalogues and restores Spanish navigation', async () => {
  const { ctx, nav } = fixture();
  await ctx.setLang('en');
  assert.equal(nav.textContent, 'Display');
  await ctx.setLang('de');
  assert.notEqual(nav.textContent, 'Display');
  await ctx.setLang('es');
  assert.equal(nav.textContent, 'Pantalla');
  assert.equal(ctx.tr('Inicio'), 'Inicio');
});

test('unsupported or invalid language falls back to source text safely', async () => {
  const { ctx } = fixture();
  await ctx.setLang('en');
  await ctx.setLang('xx');
  assert.equal(ctx.tr('Inicio'), 'Inicio');
  await ctx.setLang('../en');
  assert.equal(ctx.document.documentElement.lang, 'es');
});

test('device language changes reload untouched pages once and preserve edits', async () => {
  const { ctx } = fixture();
  let reloads = 0, notices = 0;
  ctx.location = { reload() { reloads++; } };
  ctx.toast = () => { notices++; };
  vm.runInContext(source.slice(source.indexOf('let info = {};'), source.indexOf('async function refreshInfo()')), ctx);
  await ctx.setLang('en');
  ctx.syncDeviceLanguage('en'); ctx.syncDeviceLanguage(undefined);
  assert.equal(reloads, 0);
  vm.runInContext('portalEdited = true', ctx);
  ctx.syncDeviceLanguage('de'); ctx.syncDeviceLanguage('de');
  assert.equal(reloads, 0); assert.equal(notices, 1);
  vm.runInContext('portalEdited = false', ctx);
  ctx.syncDeviceLanguage('de'); ctx.syncDeviceLanguage('de');
  assert.equal(reloads, 1);
});
const languageChange = source.slice(source.indexOf('async function changeLanguage('), source.indexOf('async function pageAjustes('));

test('firmware diagnostics use reviewed Simplified and Traditional terminology', async () => {
  for (const [code, slot, dump] of [['zh-Hans', '固件分区', '崩溃转储'], ['zh-Hant', '韌體分割區', '當機傾印']]) {
    const { ctx } = fixture(); await ctx.setLang(code);
    assert.equal(ctx.tr('Ranura'), slot);
    assert.equal(ctx.tr('Volcado'), dump);
    assert.equal(ctx.tr('Compilado'), code === 'zh-Hans' ? '编译时间' : '編譯時間');
    const advice = ctx.tr('Tiene que ser el .bin del firmware (%s)', 'build/lcd10_1/rev3_x/p4os.bin');
    assert.ok(advice.includes('build/lcd10_1/rev3_x/p4os.bin'));
    assert.ok(!ctx.tr('a prueba').includes('审判'));
  }
});
test('language selection waits for the device to finish applying the change', async () => {
  const { ctx } = fixture();
  let polls = 0, waits = 0, posted;
  ctx.post = async (route, body) => { posted = [route, body.lang]; };
  ctx.api = async () => ({ lang: ++polls < 3 ? 'en' : 'zh-Hant' });
  ctx.setTimeout = fn => { waits++; fn(); };
  vm.runInContext(languageChange, ctx);
  await ctx.changeLanguage('zh-Hant');
  assert.deepEqual(posted, ['settings', 'zh-Hant']);
  assert.equal(polls, 3); assert.equal(waits, 2);
});
test('failed language requests stop immediately without a false confirmation', async () => {
  const { ctx } = fixture();
  let polls = 0;
  ctx.post = async () => { throw new Error('offline'); };
  ctx.api = async () => { polls++; return {}; };
  vm.runInContext(languageChange, ctx);
  await assert.rejects(ctx.changeLanguage('zh-Hans'), /offline/);
  assert.equal(polls, 0);
});
test('a language the device cannot apply produces an error after a bounded wait', async () => {
  const { ctx } = fixture();
  let polls = 0;
  ctx.post = async () => {};
  ctx.api = async () => { polls++; return { lang: 'en' }; };
  ctx.setTimeout = fn => fn();
  vm.runInContext(languageChange, ctx);
  await assert.rejects(ctx.changeLanguage('zh-Hans'), /fuentes/);
  assert.equal(polls, 40);
});
test('Simplified and Traditional Chinese use the device locale offline', async () => {
  for (const [code, text, locale] of [['zh-Hans', '设置', 'zh-CN'], ['zh-Hant', '設定', 'zh-TW']]) {
    const { ctx } = fixture(); await ctx.setLang(code);
    assert.equal(ctx.tr('Ajustes'), text);
    assert.equal(vm.runInContext('LOCALE', ctx), locale);
    const minute = vm.runInContext("fmtUnit(2, 'minute')", ctx);
    const hour = vm.runInContext("fmtUnit(3, 'hour')", ctx);
    assert.match(minute, code === 'zh-Hans' ? /分钟/ : /分鐘/);
    assert.match(hour, code === 'zh-Hans' ? /小时/ : /小時/);
    assert.doesNotMatch(minute + hour, /\b(?:min|h)\b/);
  }
});
test('app catalogues cannot overwrite portal or another app translations', async () => {
  const { ctx } = fixture(); await ctx.setLang('en');
  ctx.fetch = async url => ({
    ok: url.startsWith('/builtin-lang/'),
    text: async () => url.includes('/keys/') ? 'Pantalla' : url.includes('web.one') ? 'First app' : 'Second app'
  });
  await Promise.all([ctx.loadCatalog('web.one.lang'), ctx.loadCatalog('web.two.lang')]);
  assert.equal(ctx.tr('Pantalla'), 'Display');
  assert.equal(ctx.pageTranslator('one')('Pantalla'), 'First app');
  assert.equal(ctx.pageTranslator('two')('Pantalla'), 'Second app');
  assert.equal(ctx.pageTranslator('one')('Inicio'), 'Home');
});
test('app pages bind their scoped catalogues, including newly merged pages', () => {
  for (const page of ['cartero', 'impresoras', 'can', 'luces', 'modbus', 'bolsa', 'estado', 'estudio', 'logica', 'metegol', 'oficina', 'plotter', 'traductor', 'vuelos']) {
    const pageSource = fs.readFileSync(path.join(root, 'apps', page, 'web', `${page}.js`), 'utf8');
    assert.match(pageSource, new RegExp(`window\\.P4OS\\.forPage\\('${page}'\\)`));
  }
});
test('legacy SD app pages retain translations without replacing portal words', async () => {
  const { ctx } = fixture(); await ctx.setLang('en');
  ctx.fetch = async url => ({ ok: url.startsWith('/lang/'), text: async () => 'Legacy only\tLegacy translation\nPantalla\tApp display\n' });
  await ctx.loadCatalog('web.legacy.lang');
  assert.equal(ctx.tr('Legacy only'), 'Legacy translation');
  assert.equal(ctx.tr('Pantalla'), 'Display');
  assert.equal(ctx.pageTranslator('other')('Legacy only'), 'Legacy only');
});

test('late responses from an earlier selection cannot replace the current language', async () => {
  const { ctx, nav } = fixture();
  const fetch = ctx.fetch;
  let release;
  ctx.fetch = async url => {
    if (!release && url === '/lang/en/_portal.lang') {
      return { ok: true, text: () => new Promise(resolve => { release = resolve; }) };
    }
    return fetch(url);
  };
  const slow = ctx.setLang('en');
  await new Promise(resolve => setImmediate(resolve));
  await ctx.setLang('de');
  await ctx.setLang('en');
  release('Pantalla\tStale response');
  await slow;
  assert.equal(ctx.tr('Pantalla'), 'Display');
  assert.equal(nav.textContent, 'Display');
});

test('unavailable built-in tables still allow a card-only language', async () => {
  const { ctx, nav } = fixture('Pantalla\tCard language\r\n\n');
  await ctx.setLang('xx');
  assert.equal(nav.textContent, 'Card language');
  assert.equal(ctx.tr('Inicio'), 'Inicio');
});
