const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');
const root = path.join(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'components/aos_portal/web/app.js'), 'utf8');
const language = source.slice(source.indexOf("let LANG = 'es'"), source.indexOf('function h('));

function fixture(card = '') {
  const nav = { textContent: 'Pantalla', dataset: {} };
  const ctx = vm.createContext({
    document: { documentElement: {}, querySelectorAll: () => [nav], addEventListener() {} },
    fetch: async url => {
      if (url.startsWith('/lang/')) return { ok: !!card, text: async () => card };
      const relative = url.replace('/builtin-lang/', 'lang/');
      const file = path.join(root, relative);
      return { ok: fs.existsSync(file), text: async () => fs.readFileSync(file, 'utf8') };
    }
  });
  vm.runInContext(language, ctx);
  return { ctx, nav };
}

test('embedded English and German work without SD, including login text', async () => {
  for (const code of ['en', 'de', 'zh-Hans', 'zh-Hant']) {
    const { ctx, nav } = fixture();
    await ctx.setLang(code);
    assert.equal(ctx.document.documentElement.lang, code);
    assert.notEqual(nav.textContent, 'Pantalla');
    assert.notEqual(ctx.tr('Esta red pide la contraseña del portal.'), 'Esta red pide la contraseña del portal.');
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
  }
});
test('app catalogues cannot overwrite portal or another app translations', async () => {
  const { ctx } = fixture(); await ctx.setLang('en');
  ctx.fetch = async url => ({ ok: url.startsWith('/builtin-lang/'), text: async () => url.includes('web.one') ? 'Pantalla\tFirst app\n' : 'Pantalla\tSecond app\n' });
  await Promise.all([ctx.loadCatalog('web.one.lang'), ctx.loadCatalog('web.two.lang')]);
  assert.equal(ctx.tr('Pantalla'), 'Display');
  assert.equal(ctx.pageTranslator('one')('Pantalla'), 'First app');
  assert.equal(ctx.pageTranslator('two')('Pantalla'), 'Second app');
  assert.equal(ctx.pageTranslator('one')('Inicio'), 'Home');
});
test('legacy SD app pages retain translations without replacing portal words', async () => {
  const { ctx } = fixture(); await ctx.setLang('en');
  ctx.fetch = async url => ({ ok: url.startsWith('/builtin-lang/'), text: async () => 'Legacy only\tLegacy translation\nPantalla\tApp display\n' });
  await ctx.loadCatalog('web.legacy.lang');
  assert.equal(ctx.tr('Legacy only'), 'Legacy translation');
  assert.equal(ctx.tr('Pantalla'), 'Display');
  assert.equal(ctx.pageTranslator('other')('Legacy only'), 'Legacy only');
});
