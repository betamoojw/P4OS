# Cartero (`aos.cartero`)

An API client in the manner of Postman, on the board and in the portal:
requests by hand (GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS), the
response looked at in detail (status, times, headers, the body as a JSON
tree or raw), tests written in JavaScript, requests kept in collections and
folders that run in sequence, and environments whose variables change the
server without rewriting the requests.

The files are **Postman's own formats** (Collection v2.1, environments), so
a collection exported from Postman is copied to the card and opens here, and
one made here opens in Postman.

## On the board

| Screen | What it has |
|---|---|
| Inicio | the collections as a tree (folders open and close, a request opens in the editor, a long press or ⋯ gives run, new request or folder here, duplicate, delete), ▶ runs a whole collection, the environment in use as a chip, the history, the environments, + for a loose request or a new collection |
| Pedido | the method, the URL with its `{{variables}}` resolved under it (the unknown ones in orange), Send; tabs Parámetros (the query, both ways with the URL), Encabezados (with quick ones), Cuerpo (none, JSON with Formatear and a validity mark, text, XML, form, GraphQL), Auth (inherited and where from, none, Bearer, Basic, API key in a header or the query), Scripts (pre-request and tests, with snippets). Save writes it back into its collection; a loose one asks which collection or folder |
| Respuesta | status, time, size, the four times as a bar, redirects and where it ended; tabs Cuerpo (a JSON tree that opens by levels, laid out, raw; a value tapped goes into a variable of the environment or the globals), Encabezados, Tests (with the scripts' errors), Consola, Enviado (method, URL and headers as they went, the body); ⬇ saves the body in `respuestas/` |
| Corrida | the requests as they are sent (status, time, tests), each opens its response; then run again with more iterations, a pause, stopping at the first failure |
| Entornos | which one is in use, new, delete, the variables of each and of the globals (on, name, value, hidden on screen); the settings: the timeout, the largest body kept, following redirects |

Measured in the simulator against `tools/fake_api.py`: the demo collection
(18 requests, 36 tests, with pre-request scripts, the three auths, a form,
redirects, chunked, a 512 KB body) runs on the board's interpreter in
under a second, and the variables its scripts set land in the
environment's file. The `.so` is 216 KB.

On the board (2026-10-26) it has been used against a real HTTPS test
server: a 125 KB collection put on the card from the portal,
and a POST that logs in and gets a token back, from the board's screen and
from the portal through the board. That server is what found the TLS
problem below.

## The two halves

| | The board (`main/`) | The portal (`web/cartero.js`) |
|---|---|---|
| Editing | method, URL, params, headers, body, auth, scripts; on the screen's keyboard or a physical one | the same, with a mouse and a real keyboard; import a Postman collection or a cURL command, export |
| Sending | the app's own HTTP/1.1 client over the HAL's connection (`aos_tls_*`), in a thread | through the board (the app does the request: no CORS, the board's network), or straight from the browser |
| Scripts | `ct_js.c`, a small JavaScript interpreter (the subset below) | the browser's own JavaScript, with the same `pm` |
| Runner | a collection or a folder, in order, with `setNextRequest` | the same |

They meet on the card (`/data/cartero/`, the board's
`aos_hal_path_data()/cartero`) and, for sending through the board, on the
live channel.

## The files

| Path (portal) | What |
|---|---|
| `/data/cartero/colecciones/<slug>.json` | a collection, Postman Collection v2.1.0 |
| `/data/cartero/entornos/<slug>.json` | an environment, Postman's format |
| `/data/cartero/globales.json` | the globals, the same format |
| `/data/cartero/estado.json` | `{"entorno": "<slug of the active environment>"}` (`""`: none) |
| `/data/cartero/historial.json` | the last 50 requests sent, newest first |
| `/data/cartero/corridas/<date>-<slug>.json` | a runner's results |
| `/data/cartero/respuestas/<name>` | response bodies saved by hand |
| `/data/cartero/web/` | the portal's request through the board and its answer |

Both sides read a file again when its date changes and, before writing one,
read it again and change only what they edited (a request in a collection,
a variable): the board and an open portal do not undo each other.

### A collection

Postman's v2.1 (`https://schema.getpostman.com/json/collection/v2.1.0/collection.json`).
What Cartero reads and writes; anything else in the file is kept as it is:

```json
{
  "info": { "name": "Demo", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "variable": [ { "key": "base", "value": "http://127.0.0.1:8470" } ],
  "auth": { "type": "bearer", "bearer": [ { "key": "token", "value": "{{token}}" } ] },
  "event": [ { "listen": "prerequest", "script": { "exec": [ "..." ] } } ],
  "item": [
    { "name": "Users", "item": [
      { "name": "List", "request": {
          "method": "GET",
          "url": { "raw": "{{base}}/users?page=1" },
          "header": [ { "key": "Accept", "value": "application/json" } ],
          "auth": { "type": "noauth" } },
        "event": [ { "listen": "test", "script": { "exec": [
          "pm.test('200', () => pm.response.to.have.status(200));" ] } } ] },
      { "name": "Create", "request": {
          "method": "POST", "url": "{{base}}/users",
          "body": { "mode": "raw", "raw": "{\"name\":\"Ana\"}", "options": { "raw": { "language": "json" } } } } }
    ] }
  ]
}
```

- `url` is a string or an object; Cartero reads `raw` (or rebuilds it from
  `protocol`, `host`, `port`, `path`, `query`) and writes the object with
  `raw` and the parts made again from it.
- `header[].disabled`, `url.query[].disabled`: kept and honoured.
- `body.mode`: `raw` (with `options.raw.language` json, text, xml, html,
  javascript), `urlencoded`, `formdata` (text fields only; sent as
  multipart), `graphql` (`query`, `variables`, sent as JSON), `none`.
- `auth.type`: `noauth`, `bearer` (`token`), `basic` (`username`,
  `password`), `apikey` (`key`, `value`, `in` header or query). A request
  without `auth` takes its folder's, then the collection's.
- `event[].listen`: `prerequest` and `test`, at the collection, each
  folder and the request, run in that order (as Postman does).

### An environment

```json
{ "name": "Desarrollo", "values": [
  { "key": "base", "value": "http://127.0.0.1:8470", "enabled": true },
  { "key": "token", "value": "abc", "enabled": true, "type": "secret" } ],
  "_postman_variable_scope": "environment" }
```

### Variables

`{{name}}` anywhere in the URL, the headers, the body and the auth. The
first that has it: the script's own (`pm.variables.set`), the active
environment, the collection's, the globals. Unknown ones are left as they
are (and the URL shows them in orange). Dynamic ones: `{{$guid}}`,
`{{$randomUUID}}`, `{{$timestamp}}` (Unix seconds), `{{$isoTimestamp}}`,
`{{$randomInt}}` (0-1000).

### The history

```json
[ { "t": 1792000000, "name": "List", "method": "GET", "url": "http://127.0.0.1:8470/users?page=1",
    "status": 200, "ms": 42, "size": 812,
    "request": { "method": "GET", "url": "{{base}}/users?page=1", "header": [], "body": {} } } ]
```

`request` is the request as it was written (with its variables), so it
opens again to be sent once more. `status` 0 is a request that failed
(no answer); `t` 0 when the board had no time yet.

### A run

```json
{ "collection": "Demo", "folder": "", "environment": "Desarrollo", "t": 1792000000, "ms": 512,
  "requests": 4, "tests": 6, "passed": 5, "failed": 1,
  "items": [ { "name": "List", "method": "GET", "url": "http://...", "status": 200, "ms": 42, "size": 812,
               "error": "", "tests": [ { "name": "200", "ok": true, "error": "" } ] } ] }
```

## Sending through the board (the live channel)

The browser cannot ask most APIs (CORS) and is not on the board's network;
the board can. The page does everything a request needs (variables, its
scripts in the browser's JavaScript, auth) and hands the board only the
final request:

1. It writes `/data/cartero/web/pedido.json`:

   ```json
   { "n": 7, "method": "POST", "url": "http://127.0.0.1:8470/post",
     "headers": [ ["Content-Type", "application/json"] ], "body": "{\"a\":1}",
     "timeout": 30000, "follow": true }
   ```

   `body` is text (UTF-8). `n` is the page's own number for it (it starts
   at a random number per page load, so two open tabs do not take each
   other's answers). The page puts in the headers it wants, auth and
   Content-Type included; the board adds Host, User-Agent, Accept,
   Accept-Encoding: identity, Connection and Content-Length when missing.
2. It posts `send 7` to `/api/live?app=aos.cartero`.
3. The app does it and puts the key `respuesta`, JSON:

   ```json
   { "n": 7, "ok": true, "status": 200, "reason": "OK", "ms": 85,
     "times": { "connect": 12, "send": 1, "wait": 60, "download": 12 },
     "headers": [ ["Content-Type", "application/json"], ["Content-Length", "51"] ],
     "size": 51, "cut": false, "binary": false, "redirects": 0, "final_url": "http://127.0.0.1:8470/post",
     "body": "{...}", "file": "/data/cartero/web/respuesta.bin", "error": "" }
   ```

   The body always goes to `respuesta.bin` too; `body` is in the JSON only
   when it is text and under 48 KB (otherwise `""`, and the page reads the
   file). `ok` false with `error` is a request that got no answer (DNS,
   connect, TLS, timeout).

The app also puts `estado`, `{"v":1,"busy":false,"n":7}`, when it opens
and whenever that changes: a 404 for it means the app is not open, and the
page offers to open it (`P.openApp('aos.cartero')`). The app lives in the
background, so it keeps answering while another app is in front. Other
messages: `cancel` (the request in flight), `reload` (the page changed the
collections or the environments).

## Scripts

The same `pm` on both sides. The page runs the browser's JavaScript; the
board runs `main/ct_js.c`, which takes the JavaScript tests and
pre-request scripts are usually written in:

- `let`, `const`, `var`; functions, arrow functions and closures; `if`,
  `for` (`;;`, `of`, `in`), `while`, `do`, `break`, `continue`, `return`,
  `try`/`catch`/`finally`, `throw`; the ternary, `&&`, `||`, `??`, `?.`;
  `+ - * / %`, `== != === !==`, `< > <= >=`, `! typeof`, `++ --`,
  `+= -= *= /=`; object and array literals (shorthand keys too), template
  strings, `.` and `[]`.
- `JSON.parse`, `JSON.stringify` (with indentation), `Math` (`floor`,
  `ceil`, `round`, `abs`, `min`, `max`, `pow`, `sqrt`, `random`),
  `Date.now()`, `parseInt`, `parseFloat`, `Number`, `String`, `Boolean`,
  `isNaN`, `Array.isArray`, `Object.keys/values/entries/assign`,
  `btoa`, `atob`, `encodeURIComponent`, `decodeURIComponent`.
- Strings: `length`, `includes`, `startsWith`, `endsWith`, `indexOf`,
  `lastIndexOf`, `slice`, `substring`, `split`, `replace`, `replaceAll`
  (with a string), `trim`, `toUpperCase`, `toLowerCase`, `padStart`,
  `padEnd`, `charAt`, `repeat`. Arrays: `length`, `push`, `pop`, `shift`,
  `unshift`, `includes`, `indexOf`, `join`, `slice`, `concat`, `reverse`,
  `map`, `filter`, `forEach`, `find`, `findIndex`, `some`, `every`,
  `reduce`, `sort`. Numbers: `toFixed`, `toString`.
- Destructuring in declarations and parameters (`const { data } =
  pm.response.json()`), spread in arrays, objects and calls, `new Date()`
  and `new Error()`.
- Not there: regular expressions, classes, getters, `async`, labels,
  `pm.sendRequest`, `require`, `jsonSchema`. The board says which in a
  SyntaxError or TypeError when it meets one; the portal runs them.
- A script is stopped after 6 s or 20 million steps, and at 48 nested
  calls: the job's thread has 96 KB of stack (in PSRAM) and nothing may
  outgrow it. Everything is allocated in the job's arena (12 MB of
  PSRAM), freed whole when the next request replaces it.

`pm`:

| | |
|---|---|
| `pm.test(name, fn)` | a test: it passes if `fn` does not throw |
| `pm.expect(v)` | `.to.equal(x)`, `.eql(x)` (deep), `.be.above/below/least/most(n)`, `.include(x)`, `.have.property(k[, v])`, `.have.lengthOf(n)`, `.be.a/an(type)`, `.be.oneOf([...])`, `.be.true/false/null/undefined/ok/empty`, `.exist`, `.not` before any |
| `pm.response` | `.code`, `.status` (the reason), `.responseTime` (ms), `.responseSize`, `.headers.get(name)`, `.headers.has(name)`, `.json()`, `.text()`; `.to.have.status(n)`, `.to.have.header(name[, value])`, `.to.be.ok` (2xx), `.success`, `.clientError`, `.serverError`, `.notFound`, `.to.have.jsonBody([path])` |
| `pm.environment`, `pm.globals`, `pm.collectionVariables` | `.get(k)`, `.set(k, v)`, `.unset(k)`, `.has(k)`, `.toObject()`, `.replaceIn(text)` |
| `pm.variables` | `.get(k)` (resolved through every scope), `.set(k, v)` (this run only), `.has(k)`, `.replaceIn(text)` |
| `pm.request` | `.url` (text), `.method`, `.headers.add({key, value})`, `.upsert`, `.remove(k)`, `.get(k)`, `.body.raw` |
| `pm.info` | `.requestName`, `.eventName` (`prerequest`, `test`), `.iteration` |
| `pm.execution.setNextRequest(name)` | the runner goes to that request next; `null` ends the iteration. `postman.setNextRequest` too |
| `console.log/info/warn/error` | lines under the tests |

And the old ones collections still carry: `tests["name"] = bool`,
`responseBody`, `responseCode.code`, `responseTime`, `responseHeaders`,
`postman.setEnvironmentVariable`, `postman.getEnvironmentVariable`,
`postman.setGlobalVariable`, `postman.clearEnvironmentVariable`.

## HTTPS

The board verifies every certificate, always, against the authorities the
firmware carries (ESP-IDF's bundle) and the board's clock; there is no
"turn off SSL verification" as in Postman, whose desktop app ships with it
off, so a server that "works in Postman" may still be refused here. When a
request gets no answer over TLS the screen says which of three it was,
with the system's own reason in grey under it:

| On the screen | What it was |
|---|---|
| La placa todavía no tiene hora | no valid time yet (no NTP since the boot), or the certificate is from the future |
| El certificado del servidor no se pudo verificar | the reason speaks of the certificate: not for that name, expired, revoked, not signed by an authority the board knows |
| No se pudo establecer la conexión segura (TLS) | everything else: a handshake the board's mbedtls could not finish, with mbedtls's code (`-0x....`) |

Until 2026-10-26 the second and third were one message, "the certificate",
and that misled the first test on the board: a server with a valid
certificate (`openssl` verified it) failed with `-0x6a00`,
`MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL`. It handed out a 5.5 KB session ticket,
the HAL kept it, and the next ClientHello carrying it did not fit the
board's 4 KB output buffer; every connection after that failed until a
restart. The firmware fixed it in `components/aos_hal/aos_tls.c` (tickets
that would not fit are not kept, and a failed resume is retried without
the session; the CHANGELOG has it), so with that server each connection
is a full handshake (~1.7 s) and `/api/log` says `a 5504-byte session
ticket does not fit a ClientHello here; not kept`. Any other server goes
on resuming: in the log, `esp-x509-crt-bundle: Certificate validated`
marks each full handshake, and a resumed one has none.

Two things to know when reading the times bar: a full handshake is
1.6-1.8 s on the board and a resumed one ~0.6 s, and a connection the app
keeps open (15 s between requests to the same host) costs nothing. Some
servers do not resume at all, through no fault of the board: open-meteo,
tried with `openssl s_client -tls1_2 -reconnect`, did not resume the first
session from a Mac either.

## Inside

| File | What |
|---|---|
| `cartero.c` | the app: life cycle, screens, jobs ending, the live channel |
| `ct_val.c`, `ct_json.c` | the values (arenas in PSRAM), JSON in and out (the parser keeps its own stack) |
| `ct_js.c`, `ct_pm.c` | the JavaScript interpreter (a tree walker, throws are `longjmp`) and what scripts see: `pm`, chai's `expect`, console, the old globals, `{{variables}}` |
| `ct_http.c` | HTTP/1.1 over `aos_tls_*`: headers back, keep-alive, chunked, redirects counted, the four times |
| `ct_store.c` | the files: Postman's collections and environments read whole and written back keeping what Cartero does not know; read again before a change |
| `ct_job.c` | a send or a run in a thread: copies of everything, the scripts' changes back as a list |
| `ct_ui*.c` | the screens |

The core is tried on the Mac too: `ct_val/json/js/pm/http.c` build with
plain clang and a few stubs of the HAL (they include nothing of LVGL).
`tools/test_core.sh` runs 28 scripts (assertions, closures, destructuring,
the old globals, the errors and the limits) under the sanitizers.

## Trying it

`tools/fake_api.py` is a small API on 127.0.0.1 with every case the app has
to handle (JSON, echo, statuses, delays, redirects, chunked, auth, a big
body, CORS for the portal's straight mode), and `--seed DIR` writes a demo
collection and two environments against it into a card folder:

    apps/cartero/tools/fake_api.py --seed sim/sim_fs
    apps/cartero/tools/fake_api.py &
    cd sim && cmake -B build-cartero -DP4OS_SIM_APPS="hello_app;cartero" && cmake --build build-cartero -j8
    cp ../apps/cartero/web/cartero.js sim_fs/web/
    P4_SIM_PORTAL_PORT=8472 CT_OPEN=demo/0/0 ./build-cartero/p4os_sim    # http://127.0.0.1:8472/#cartero

`CT_OPEN=<collection>/<i>[/<j>]` opens a request at once, `CT_SCREEN=env`,
`hist` or `new` a screen.
