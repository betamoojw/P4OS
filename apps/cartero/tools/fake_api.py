#!/usr/bin/env python3
"""
A make-believe API for P4OS's Cartero (apps/cartero), on 127.0.0.1 only, so
the app and its portal page are built and tried without touching anybody's
real server.

    apps/cartero/tools/fake_api.py [--port 8470] [--seed CARD_DIR]

Everything answers with CORS open (the portal's "straight from the browser"
mode needs it). Any path may start with /staging: the same API, with
"env": "staging" in what it says, which is how the demo's two environments
differ.

    /get /post /put /patch /delete /anything[/...]   the request echoed back:
                         method, path, args, headers, body, json, form
    /status/<n>          that status, with a small JSON body (204 and 304 none)
    /delay/<ms>          answers after that long (at most 30 s)
    /users               GET a page (?page=, ?per_page=), POST one (201)
    /users/<id>          GET, PUT, PATCH, DELETE (404 when it is not there)
    /login               POST {"user": "ana", "password": "secreto"} -> {"token"}
    /me                  needs "Authorization: Bearer <that token>"
    /basic               needs Basic ana:secreto
    /apikey              needs X-Api-Key: demo-key, or ?api_key=demo-key
    /redirect/<n>        n redirects (302) and then /get
    /chunked             a JSON body sent chunked
    /big?kb=<n>          a JSON array of about n KB (default 256)
    /xml /html /text     those types
    /image.png           a small picture (binary)
    /close               HTTP/1.0 with no length: read until it closes
    /cookies/set?k=v     Set-Cookie for each argument

--seed CARD_DIR writes into CARD_DIR/data/cartero/ a demo collection that
uses all of that (with tests), two environments (Desarrollo and Staging)
and the globals, and makes Desarrollo the active one.
"""
import argparse
import base64
import json
import os
import struct
import threading
import time
import uuid
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

TOKENS = set()
USERS = {}
LOCK = threading.Lock()


def seed_users():
    names = ["Ana", "Bruno", "Carla", "Diego", "Elena", "Federico", "Gabriela", "Hernán", "Inés", "Juan",
             "Karina", "Lucas"]
    for i, n in enumerate(names, 1):
        USERS[i] = {"id": i, "name": n, "email": n.lower().replace("é", "e").replace("á", "a") + "@example.com",
                    "active": i % 3 != 0, "roles": ["user"] + (["admin"] if i == 1 else []),
                    "address": {"city": ["Rosario", "Córdoba", "Mendoza"][i % 3], "zip": str(2000 + i)}}


def tiny_png():
    w, h = 16, 16
    raw = b"".join(b"\x00" + b"".join(bytes((x * 16, y * 16, 160)) for x in range(w)) for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PNG = tiny_png()


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FakeAPI/1.0"

    def log_message(self, fmt, *args):
        print("[fake_api] " + fmt % args, flush=True)

    # ---- helpers ----

    def cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Expose-Headers", "*")

    def send(self, code, body=b"", ctype="application/json", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False, indent=2).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.cors()
        if code not in (204, 304):
            self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith(("application/json", "text")) else ""))
            self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or []):
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD" and code not in (204, 304):
            self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            data = b""
            while True:
                size = int(self.rfile.readline().strip() or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    break
                data += self.rfile.read(size)
                self.rfile.readline()
            return data
        return self.rfile.read(n) if n else b""

    def echo(self, path, args, raw):
        text = raw.decode("utf-8", "replace")
        js = None
        try:
            js = json.loads(text) if text else None
        except ValueError:
            pass
        form = {}
        ct = self.headers.get("Content-Type", "")
        if ct.startswith("application/x-www-form-urlencoded"):
            form = {k: v[0] if len(v) == 1 else v for k, v in parse_qs(text).items()}
        return {"method": self.command, "path": path, "args": args, "headers": dict(self.headers),
                "body": text, "json": js, "form": form, "env": self.env, "origin": self.client_address[0]}

    def auth_bearer(self):
        a = self.headers.get("Authorization", "")
        return a.startswith("Bearer ") and a[7:] in TOKENS

    # ---- dispatch ----

    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.send_header("Access-Control-Max-Age", "600")
        self.send_header("Allow", "GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS")
        self.end_headers()

    def do_GET(self):
        self.route()

    do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_GET

    def route(self):
        u = urlparse(self.path)
        path = u.path
        self.env = "development"
        if path.startswith("/staging"):
            self.env = "staging"
            path = path[len("/staging"):] or "/"
        args = {k: v[0] if len(v) == 1 else v for k, v in parse_qs(u.query).items()}
        raw = self.body()
        parts = [p for p in path.split("/") if p]
        m = self.command
        head = parts[0] if parts else ""

        if path in ("/", ""):
            return self.send(200, {"name": "fake_api", "env": self.env, "see": "apps/cartero/tools/fake_api.py"})
        if head in ("get", "post", "put", "patch", "delete", "anything"):
            return self.send(200, self.echo(path, args, raw))
        if head == "headers":
            return self.send(200, {"headers": dict(self.headers)})
        if head == "status" and len(parts) == 2 and parts[1].isdigit():
            code = int(parts[1])
            return self.send(code, {"status": code, "env": self.env},
                             extra=[("Location", "/get")] if 300 <= code < 400 else None)
        if head == "delay" and len(parts) == 2 and parts[1].isdigit():
            time.sleep(min(int(parts[1]), 30000) / 1000)
            return self.send(200, {"delayed_ms": int(parts[1]), "env": self.env})
        if head == "redirect" and len(parts) == 2 and parts[1].isdigit():
            n = int(parts[1])
            pre = "/staging" if self.env == "staging" else ""
            loc = pre + (f"/redirect/{n - 1}" if n > 1 else "/get")
            return self.send(302, {"redirect": loc}, extra=[("Location", loc)])
        if head == "users":
            return self.users(m, parts, args, raw)
        if head == "login" and m == "POST":
            try:
                j = json.loads(raw or b"{}")
            except ValueError:
                return self.send(400, {"error": "the body is not JSON"})
            if j.get("user") == "ana" and j.get("password") == "secreto":
                t = uuid.uuid4().hex
                TOKENS.add(t)
                return self.send(200, {"token": t, "expires_in": 3600, "user": {"id": 1, "name": "Ana"}})
            return self.send(401, {"error": "wrong user or password"})
        if head == "me":
            if not self.auth_bearer():
                return self.send(401, {"error": "a Bearer token from /login is needed"},
                                 extra=[("WWW-Authenticate", 'Bearer realm="fake"')])
            return self.send(200, {"id": 1, "name": "Ana", "env": self.env})
        if head == "basic":
            a = self.headers.get("Authorization", "")
            ok = a.startswith("Basic ") and base64.b64decode(a[6:] + "==").decode("utf-8", "replace") == "ana:secreto"
            if not ok:
                return self.send(401, {"authenticated": False}, extra=[("WWW-Authenticate", 'Basic realm="fake"')])
            return self.send(200, {"authenticated": True, "user": "ana"})
        if head == "apikey":
            if self.headers.get("X-Api-Key") == "demo-key" or args.get("api_key") == "demo-key":
                return self.send(200, {"ok": True})
            return self.send(403, {"ok": False, "error": "X-Api-Key: demo-key"})
        if head == "chunked":
            items = [{"i": i, "word": w} for i, w in enumerate(["uno", "dos", "tres", "cuatro", "cinco"])]
            data = json.dumps({"items": items}, indent=2).encode()
            self.send_response(200)
            self.cors()
            self.send_header("Content-Type", "application/json")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(data), 40):
                c = data[i:i + 40]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
                self.wfile.flush()
                time.sleep(0.01)
            self.wfile.write(b"0\r\n\r\n")
            return
        if head == "big":
            kb = max(1, min(int(args.get("kb", "256")), 16384))
            out, n = [], 0
            while n < kb * 1024:
                row = {"id": len(out), "name": f"item {len(out)}", "value": len(out) * 3.25, "tags": ["a", "b"]}
                out.append(row)
                n += 70
            return self.send(200, out)
        if head == "xml":
            return self.send(200, '<?xml version="1.0"?>\n<users><user id="1">Ana</user><user id="2">Bruno</user></users>\n',
                             "application/xml")
        if head == "html":
            return self.send(200, "<!doctype html><html><body><h1>Hola</h1><p>fake_api</p></body></html>\n", "text/html")
        if head == "text":
            return self.send(200, "Hola desde fake_api.\nSegunda línea.\n", "text/plain")
        if head == "image.png":
            return self.send(200, PNG, "image/png")
        if head == "close":
            body = b'{"closed": true, "note": "no Content-Length: read until the end"}\n'
            self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                             b"Access-Control-Allow-Origin: *\r\n\r\n" + body)
            self.close_connection = True
            return
        if head == "cookies" and len(parts) == 2 and parts[1] == "set":
            return self.send(200, {"cookies": args}, extra=[("Set-Cookie", f"{k}={v}; Path=/") for k, v in args.items()])
        return self.send(404, {"error": "no such path", "path": path})

    def users(self, m, parts, args, raw):
        with LOCK:
            if len(parts) == 1:
                if m in ("GET", "HEAD"):
                    page = max(1, int(args.get("page", "1")))
                    per = max(1, min(int(args.get("per_page", "5")), 50))
                    ids = sorted(USERS)
                    chunk = [USERS[i] for i in ids[(page - 1) * per:page * per]]
                    return self.send(200, {"page": page, "per_page": per, "total": len(ids), "env": self.env,
                                           "data": chunk})
                if m == "POST":
                    try:
                        j = json.loads(raw or b"{}")
                    except ValueError:
                        return self.send(400, {"error": "the body is not JSON"})
                    if not isinstance(j, dict) or not j.get("name"):
                        return self.send(422, {"error": "name is required"})
                    nid = max(USERS) + 1 if USERS else 1
                    USERS[nid] = {"id": nid, "name": j["name"], "email": j.get("email", ""), "active": True,
                                  "roles": ["user"], "address": j.get("address", {})}
                    return self.send(201, USERS[nid], extra=[("Location", f"/users/{nid}")])
                return self.send(405, {"error": "method not allowed"})
            if not parts[1].isdigit() or int(parts[1]) not in USERS:
                return self.send(404, {"error": "no such user"})
            uid = int(parts[1])
            if m in ("GET", "HEAD"):
                return self.send(200, USERS[uid])
            if m in ("PUT", "PATCH"):
                try:
                    j = json.loads(raw or b"{}")
                except ValueError:
                    return self.send(400, {"error": "the body is not JSON"})
                if m == "PUT":
                    USERS[uid] = {"id": uid, **{k: v for k, v in j.items() if k != "id"}}
                else:
                    USERS[uid].update({k: v for k, v in j.items() if k != "id"})
                return self.send(200, USERS[uid])
            if m == "DELETE":
                del USERS[uid]
                return self.send(204)
            return self.send(405, {"error": "method not allowed"})


# ---- the demo files ----

def script(*lines):
    return {"type": "text/javascript", "exec": list(lines)}


def req(name, method, url, header=None, body=None, tests=None, pre=None, auth=None):
    r = {"name": name, "request": {"method": method, "header": header or [], "url": url}}
    if body is not None:
        r["request"]["body"] = body
    if auth is not None:
        r["request"]["auth"] = auth
    ev = []
    if pre:
        ev.append({"listen": "prerequest", "script": script(*pre)})
    if tests:
        ev.append({"listen": "test", "script": script(*tests)})
    if ev:
        r["event"] = ev
    r["response"] = []
    return r


def raw_json(obj):
    return {"mode": "raw", "raw": json.dumps(obj, ensure_ascii=False, indent=2),
            "options": {"raw": {"language": "json"}}}


def demo_collection():
    return {
        "info": {"_postman_id": str(uuid.uuid4()), "name": "Demo fake_api",
                 "description": "Todo lo que hace tools/fake_api.py, con tests.",
                 "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},
        "variable": [{"key": "per_page", "value": "5"}],
        "event": [{"listen": "test", "script": script(
            "pm.test('responde en menos de 2 s', () => pm.expect(pm.response.responseTime).to.be.below(2000));")}],
        "item": [
            {"name": "Usuarios", "item": [
                req("Listar", "GET", "{{base}}/users?page=1&per_page={{per_page}}",
                    header=[{"key": "Accept", "value": "application/json"}],
                    tests=["pm.test('status 200', () => pm.response.to.have.status(200));",
                           "const j = pm.response.json();",
                           "pm.test('trae una página', () => {",
                           "  pm.expect(j.data).to.be.an('array');",
                           "  pm.expect(j.data.length).to.equal(Number(pm.variables.get('per_page')));",
                           "});",
                           "pm.environment.set('first_id', j.data[0].id);",
                           "console.log('primer usuario', j.data[0].name);"]),
                req("Ver uno", "GET", "{{base}}/users/{{first_id}}",
                    tests=["pm.test('es el primero', () => pm.expect(pm.response.json().id).to.eql(Number(pm.environment.get('first_id'))));",
                           "pm.test('tiene email', () => pm.expect(pm.response.json()).to.have.property('email'));"]),
                req("Crear", "POST", "{{base}}/users",
                    header=[{"key": "Content-Type", "value": "application/json"}],
                    body=raw_json({"name": "Zoe {{$randomInt}}", "email": "zoe@example.com"}),
                    tests=["pm.test('creado', () => pm.response.to.have.status(201));",
                           "pm.environment.set('new_id', pm.response.json().id);"]),
                req("Cambiar", "PATCH", "{{base}}/users/{{new_id}}",
                    header=[{"key": "Content-Type", "value": "application/json"}],
                    body=raw_json({"active": False}),
                    tests=["pm.test('inactivo', () => pm.expect(pm.response.json().active).to.be.false);"]),
                req("Borrar", "DELETE", "{{base}}/users/{{new_id}}",
                    tests=["pm.test('204', () => pm.response.to.have.status(204));"]),
            ]},
            {"name": "Autenticación", "item": [
                req("Login", "POST", "{{base}}/login",
                    header=[{"key": "Content-Type", "value": "application/json"}],
                    body=raw_json({"user": "{{user}}", "password": "{{password}}"}),
                    tests=["pm.test('token', () => pm.expect(pm.response.json().token).to.be.a('string'));",
                           "pm.environment.set('token', pm.response.json().token);"]),
                req("Yo (Bearer)", "GET", "{{base}}/me",
                    auth={"type": "bearer", "bearer": [{"key": "token", "value": "{{token}}", "type": "string"}]},
                    tests=["pm.test('soy Ana', () => pm.expect(pm.response.json().name).to.equal('Ana'));"]),
                req("Basic", "GET", "{{base}}/basic",
                    auth={"type": "basic", "basic": [{"key": "username", "value": "{{user}}"},
                                                     {"key": "password", "value": "{{password}}"}]},
                    tests=["pm.test('autenticado', () => pm.expect(pm.response.json().authenticated).to.be.true);"]),
                req("API key", "GET", "{{base}}/apikey",
                    auth={"type": "apikey", "apikey": [{"key": "key", "value": "X-Api-Key"},
                                                       {"key": "value", "value": "demo-key"},
                                                       {"key": "in", "value": "header"}]},
                    tests=["pm.test('ok', () => pm.response.to.be.ok);"]),
            ]},
            {"name": "Varios", "item": [
                req("Eco con pre-request", "POST", "{{base}}/anything/eco?x=1",
                    header=[{"key": "Content-Type", "value": "application/json"},
                            {"key": "X-Desactivado", "value": "no va", "disabled": True}],
                    body=raw_json({"hora": "{{ahora}}", "id": "{{$guid}}"}),
                    pre=["pm.variables.set('ahora', Date.now());",
                         "pm.request.headers.add({ key: 'X-Firma', value: btoa('cartero') });"],
                    tests=["const j = pm.response.json();",
                           "pm.test('llegó la firma', () => pm.expect(j.headers['X-Firma']).to.equal('Y2FydGVybw=='));",
                           "pm.test('sin el desactivado', () => pm.expect(j.headers).to.not.have.property('X-Desactivado'));",
                           "pm.test('query', () => pm.expect(j.args.x).to.equal('1'));"]),
                req("Formulario", "POST", "{{base}}/post",
                    body={"mode": "urlencoded", "urlencoded": [{"key": "nombre", "value": "Ana"},
                                                               {"key": "ciudad", "value": "Rosario"}]},
                    tests=["pm.test('form', () => pm.expect(pm.response.json().form.ciudad).to.equal('Rosario'));"]),
                req("404", "GET", "{{base}}/status/404",
                    tests=["pm.test('no está', () => pm.response.to.have.status(404));"]),
                req("Redirecciones", "GET", "{{base}}/redirect/3",
                    tests=["pm.test('llegó', () => pm.response.to.have.status(200));"]),
                req("Chunked", "GET", "{{base}}/chunked",
                    tests=["pm.test('cinco', () => pm.expect(pm.response.json().items).to.have.lengthOf(5));"]),
                req("Lento", "GET", "{{base}}/delay/800"),
                req("XML", "GET", "{{base}}/xml"),
                req("Imagen", "GET", "{{base}}/image.png"),
                req("Grande", "GET", "{{base}}/big?kb=512"),
            ]},
        ],
    }


def env(name, values):
    return {"id": str(uuid.uuid4()), "name": name,
            "values": [{"key": k, "value": v, "type": "secret" if k == "password" else "default", "enabled": True}
                       for k, v in values],
            "_postman_variable_scope": "environment"}


def seed(card, port):
    base = os.path.join(card, "data", "cartero")
    for sub in ("colecciones", "entornos", "corridas", "respuestas", "web"):
        os.makedirs(os.path.join(base, sub), exist_ok=True)

    def write(rel, obj):
        with open(os.path.join(base, rel), "w", encoding="utf-8") as f:
            json.dump(obj, f, ensure_ascii=False, indent="\t")
            f.write("\n")
    write("colecciones/demo.json", demo_collection())
    write("entornos/desarrollo.json", env("Desarrollo", [("base", f"http://127.0.0.1:{port}"), ("user", "ana"),
                                                        ("password", "secreto")]))
    write("entornos/staging.json", env("Staging", [("base", f"http://127.0.0.1:{port}/staging"), ("user", "ana"),
                                                  ("password", "secreto")]))
    write("globales.json", {"name": "Globales", "values": [{"key": "app", "value": "cartero", "enabled": True}],
                            "_postman_variable_scope": "globals"})
    write("estado.json", {"entorno": "desarrollo"})
    print(f"seeded {base}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8470)
    ap.add_argument("--seed", metavar="CARD_DIR")
    a = ap.parse_args()
    seed_users()
    if a.seed:
        seed(a.seed, a.port)
        return
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), H)
    print(f"fake_api on http://127.0.0.1:{a.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
