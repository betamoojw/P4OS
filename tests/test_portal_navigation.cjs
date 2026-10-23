// Exercise production page handlers with delayed responses and navigation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../components/aos_portal/web/app.js'), 'utf8');
const helpers = source.slice(source.indexOf('let routeEpoch'), source.indexOf('/* replaceChildren'));
const settingsPage = source.slice(source.indexOf('async function pageAjustes('), source.indexOf('/* ---- Registro'));
const firmwarePage = source.slice(source.indexOf('async function pageFirmware('), source.indexOf('/* ----', source.indexOf('async function pageFirmware(')));
function deferred() { let resolve, reject; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return { promise, resolve, reject }; }
const settings = { name: 'p4os', tz: 'UTC', langs: [], wallpapers: [], landscape: true };
function fixture() {
  const renders = [], buttons = [], timers = [];
  const ctx = vm.createContext({ location: { hash: '#ajustes' }, main: {}, info: {}, TZS: [],
    api: async () => settings, post: async () => {}, toast() {},
    tr: s => s, setTimeout: f => timers.push(f),
    h: (tag, props, ...children) => { const el = { tag, props, children }; if (tag === 'button') buttons.push(el); return el; },
    put: (...args) => renders.push(args),
  });
  vm.runInContext(helpers + settingsPage + firmwarePage, ctx);
  const navigate = hash => { ctx.location.hash = hash; vm.runInContext('routeEpoch++', ctx); };
  return { ctx, renders, buttons, timers, navigate };
}

test('late settings response cannot replace the newly selected page', async () => {
  const f = fixture(), pending = deferred();
  f.ctx.api = () => pending.promise;
  const work = f.ctx.pageAjustes();
  f.navigate('#pantalla'); pending.resolve(settings); await work;
  assert.equal(f.renders.length, 0);
});

test('orientation refresh stays on settings but never overwrites navigation, including away and back', async () => {
  for (const destination of ['stay', '#pantalla', 'away-back']) {
    const f = fixture(); await f.ctx.pageAjustes();
    const post = deferred(); f.ctx.post = () => post.promise;
    const work = f.buttons.find(b => b.children.includes('Vertical')).props.onclick();
    assert.equal(f.timers.length, 0, 'wait for device acknowledgement');
    if (destination !== 'stay') f.navigate('#pantalla');
    if (destination === 'away-back') f.navigate('#ajustes');
    post.resolve(); await work;
    f.timers.forEach(fn => fn()); await new Promise(setImmediate);
    assert.equal(f.renders.length, destination === 'stay' ? 2 : 1, destination);
  }
});

test('delayed firmware results and failures cannot overwrite another page', async () => {
  for (const failure of [false, true]) {
    const f = fixture(), pending = deferred(); f.navigate('#firmware');
    f.ctx.api = () => pending.promise;
    const work = f.ctx.pageFirmware();
    f.navigate('#pantalla');
    if (failure) pending.reject(new Error('offline')); else pending.resolve({});
    await work; assert.equal(f.renders.length, 0);
  }
});

test('firmware advice accepts all four boards and both silicon profiles with a safe fallback', () => {
  const { ctx } = fixture();
  for (const board of ['lcd5', 'lcd7', 'lcd8', 'lcd10_1']) for (const silicon of ['rev1_3', 'rev3_x']) {
    const firmware_path = `build/${board}/${silicon}/p4os.bin`;
    assert.equal(ctx.firmwarePath({ firmware_path }), firmware_path);
  }
  for (const firmware_path of [undefined, '', 'build/rev1_3/p4os.bin', '../../wrong.bin', '<script>'])
    assert.equal(ctx.firmwarePath({ firmware_path }), 'p4os.bin');
});
