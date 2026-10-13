#!/usr/bin/env python3
"""
A make-believe micropython-lib index and a web server to test the Python
app's network (socket, tls, requests, mip) with no outside server.

    tools/fake_mip.py [--port 8094] [--tls --cert server.pem --key server.key]

The index answers as https://micropython.org/pi/v2 does, for a few small
packages made up here (one depends on another, as mip has to follow):

    /pi/v2/package/<mpy>/<name>/<version>.json    hashes, urls, deps
    /pi/v2/file/<xx>/<hash>                       a file by its short hash

    import mip
    mip.install("greet", index="http://<this-mac>:8094/pi/v2")

And for requests:

    /get            JSON with the method, the path and the headers it got
    /post           the body back, with its length (any method)
    /chunked        a body in Transfer-Encoding: chunked
    /redirect       302 to /get (/redirect/301, /303, /307, /308 too)
    /status/<n>     that status, with a short body
    /big?n=<bytes>  that many bytes (a pattern), with Content-Length
    /slow?s=<sec>   the answer after that long

HTTPS as fake_ha.py: a throwaway CA made in a scratch dir (its docstring has
the openssl lines), the simulator told to trust it with P4_SIM_EXTRA_CA.
The standard library only.
"""
import argparse
import hashlib
import json
import ssl
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# name -> (version, {path in the package: source}, [dependencies])
PACKAGES = {
    "hello": ("1.0.0", {
        "hello/__init__.py": 'def hi(who="P4OS"):\n    return "hola, " + who\n',
        "hello/extra.py": "ANSWER = 42\n",
    }, []),
    "greet": ("0.2.0", {
        "greet.py": "import hello\n\n\ndef greet(names):\n    return [hello.hi(n) for n in names]\n",
    }, [["hello", "latest"]]),
}


def short_hash(data):
    return hashlib.sha256(data).hexdigest()[:8]


FILES = {}
for _, (_, files, _) in PACKAGES.items():
    for _, src in files.items():
        b = src.encode()
        FILES[short_hash(b)] = b


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("%s %s" % (self.address_string(), fmt % args), flush=True)

    def send(self, status, body, ctype="application/json", extra=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def route(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        p = u.path
        if p.startswith("/pi/v2/package/"):
            parts = p.split("/")              # '', pi, v2, package, mpy, name, version.json
            if len(parts) != 7 or parts[5] not in PACKAGES:
                return self.send(404, "not found", "text/plain")
            version, files, deps = PACKAGES[parts[5]]
            want = parts[6][:-5] if parts[6].endswith(".json") else parts[6]
            if want not in ("latest", version):
                return self.send(404, "no such version", "text/plain")
            return self.send(200, {
                "v": 1, "version": version, "deps": deps,
                "hashes": [[path, short_hash(src.encode())] for path, src in files.items()],
            })
        if p.startswith("/pi/v2/file/"):
            h = p.rsplit("/", 1)[-1]
            if h in FILES:
                return self.send(200, FILES[h], "application/octet-stream")
            return self.send(404, "no such file", "text/plain")
        if p == "/get":
            return self.send(200, {"method": self.command, "path": self.path,
                                   "headers": dict(self.headers)})
        if p == "/post":
            b = self.body()
            return self.send(200, {"method": self.command, "len": len(b),
                                   "body": b.decode("utf-8", "replace"),
                                   "type": self.headers.get("Content-Type")})
        if p == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(5):
                c = ("chunk %d of 5\n" % (i + 1)).encode()
                self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
            self.wfile.write(b"0\r\n\r\n")
            return
        if p.startswith("/redirect"):
            code = int(p.split("/")[2]) if p.count("/") > 1 else 302
            return self.send(code, "", "text/plain", [("Location", "/get")])
        if p.startswith("/status/"):
            code = int(p.split("/")[2])
            return self.send(code, "status %d" % code, "text/plain")
        if p == "/big":
            n = int(q.get("n", ["100000"])[0])
            pattern = bytes(range(256))
            data = (pattern * (n // 256 + 1))[:n]
            return self.send(200, data, "application/octet-stream")
        if p == "/slow":
            time.sleep(float(q.get("s", ["3"])[0]))
            return self.send(200, {"slept": float(q.get("s", ["3"])[0])})
        return self.send(404, "not found", "text/plain")

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = route


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8094)
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    a = ap.parse_args()
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    if a.tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(a.cert, a.key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("fake micropython-lib index on %s://0.0.0.0:%d/pi/v2" % ("https" if a.tls else "http", a.port), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
