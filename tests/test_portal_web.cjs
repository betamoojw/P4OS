// Offline tests of the production portal helpers. No device or network access.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../components/aos_portal/web/app.js'), 'utf8');
const pointSource = source.slice(source.indexOf('function screenPoint('), source.indexOf('function pagePantalla('));
const uploadSource = source.slice(source.indexOf('function upload('), source.indexOf('async function pageArchivos('));

test('screen coordinates stay within every supported panel and orientation', () => {
  const ctx = vm.createContext({ Date });
  vm.runInContext(pointSource, ctx);
  for (const [width, height] of [[1280, 800], [800, 1280], [1280, 720], [720, 1280]]) {
    const img = { naturalWidth: width, naturalHeight: height,
      getBoundingClientRect: () => ({ left: 10, top: 20, width: width / 2, height: height / 2 }) };
    for (const [x, y, expectedX, expectedY] of [
      [10, 20, 0, 0], [-100, -100, 0, 0],
      [10 + width / 2, 20 + height / 2, width - 1, height - 1],
      [10000, 10000, width - 1, height - 1],
      [10 + width / 4, 20 + height / 4, width / 2, height / 2]]) {
      const p = ctx.screenPoint(img, { clientX: x, clientY: y });
      assert.equal(p.x, expectedX); assert.equal(p.y, expectedY);
    }
    img.naturalWidth = 0;
    assert.equal(ctx.screenPoint(img, { clientX: 0, clientY: 0 }), null);
  }
});

function fixture(outcomes) {
  const requests = [], notices = [];
  let refreshes = 0;
  const ctx = vm.createContext({ cwd: '/original',
    toast: (message, bad) => notices.push({ message, bad: !!bad }),
    tr: (s, ...args) => s.replace(/%s/g, () => args.shift()),
    pageArchivos: () => { refreshes++; },
    XMLHttpRequest: class {
      constructor() { this.upload = {}; }
      open(method, url) { requests.push({ method, url }); }
      send() {
        const result = outcomes.shift();
        queueMicrotask(() => {
          ctx.cwd = '/changed';
          if (result.event) this[result.event]();
          else { Object.assign(this, result); this.onload(); }
        });
      }
    }
  });
  vm.runInContext(uploadSource, ctx);
  return { ctx, requests, notices, refreshes: () => refreshes,
    run: files => ctx.upload(files, { firstChild: { style: {} }, dataset: {} }) };
}

test('successful batch keeps its destination when navigation changes', async () => {
  const f = fixture([{ status: 200 }, { status: 201 }]);
  await f.run([{ name: 'one.txt' }, { name: 'two.txt' }]);
  assert.deepEqual(f.requests.map(r => r.url), [
    '/api/fs/put?path=%2Foriginal%2Fone.txt', '/api/fs/put?path=%2Foriginal%2Ftwo.txt']);
  assert.equal(f.notices.length, 1); assert.equal(f.notices[0].bad, false);
  assert.equal(f.refreshes(), 1);
});

test('JSON and non-JSON upload failures settle without a false success', async () => {
  for (const responseText of ['{"error":"no card"}', '<html>error</html>', '']) {
    const f = fixture([{ status: 500, statusText: 'Server error', responseText }, { status: 200 }]);
    await f.run([{ name: 'one.txt' }, { name: 'two.txt' }]);
    assert.equal(f.requests.length, 2);
    assert.equal(f.notices.length, 1); assert.equal(f.notices[0].bad, true);
    assert.equal(f.refreshes(), 1);
  }
});

test('network error, cancellation and timeout never report upload success', async () => {
  for (const event of ['onerror', 'onabort', 'ontimeout']) {
    const f = fixture([{ event }]);
    await f.run([{ name: 'one.txt' }]);
    assert.equal(f.notices.length, 1); assert.equal(f.notices[0].bad, true);
  }
});

test('empty file selection does not report success or navigate', async () => {
  const f = fixture([]);
  await f.run([]);
  assert.equal(f.requests.length, 0); assert.equal(f.notices.length, 0);
  assert.equal(f.refreshes(), 0);
});
