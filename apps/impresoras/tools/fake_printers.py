#!/usr/bin/env python3
"""
Make-believe 3D printers for P4OS's Impresoras 3D app (apps/impresoras), one
of each protocol the app speaks, on 127.0.0.1 only, so the app is built and
tried without touching anybody's real printer.

    apps/impresoras/tools/fake_printers.py [--base 8300] [--speed 20]
        [--certs DIR] [--config FILE]

Each printer runs a job that goes on by itself (--speed times faster than
real), with temperatures that follow their targets, and takes the app's
commands (pause, resume, cancel, temperatures, moves, G-code, files):

    base+1  OctoPrint      X-Api-Key "testkey"; a camera (snapshot and MJPEG)
                           and a job picture (the PrusaSlicer Thumbnails plugin)
    base+2  PrusaLink      Digest, user "maker", password "testpass"
    base+3  Moonraker      no key; a webcam; the console (gcode_store)
    base+4  Duet (RRF 3)   password "testpass"; rr_reply answers
    base+5  FlashForge     /detail on HTTP, serial "SNFF0001", check code "1234";
            and its G-code port, 8899 (fixed by the printer)
    base+6  Elegoo SDCP    ws://.../websocket, mainboard "e1e2e3e4e5e6"
    base+7  Creality       ws://..:base+7
    base+8  Bambu Lab      MQTT over TLS, serial "01P00A000000001", access
            code "12345678"; and its camera on port 6000 (TLS, fixed)

TLS needs a certificate the simulator trusts: --certs DIR makes a throwaway
CA and a certificate for 127.0.0.1 there (openssl), and says what to put in
P4_SIM_EXTRA_CA. --config FILE writes an impresoras.txt with the eight.

The standard library, Pillow for the camera's pictures and openssl for the
certificate.
"""
import argparse
import base64
import hashlib
import io
import json
import math
import os
import random
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from PIL import Image, ImageDraw
except ImportError:                         # the camera then sends a fixed grey picture
    Image = None

SPEED = 20.0
LOCK = threading.Lock()


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# --------------------------------------------------------------------------- a printer

class Printer:
    def __init__(self, name, total=3600, start=0.35, file="benchy.gcode"):
        self.name = name
        self.total = total
        self.elapsed = total * start
        self.state = "printing"          # idle printing paused done cancelled error
        self.file = file
        self.tool = [190.0, 215.0]
        self.tool1 = None
        self.bed = [55.0, 60.0]
        self.chamber = [31.0, 0.0]
        self.fan = 100
        self.speed = 100
        self.flow = 100
        self.light = 1
        self.pos = [110.0, 95.0, 0.2]
        self.homed = True
        self.layers = 240
        self.last = time.time()
        self.console = []
        self.files = [
            ("benchy.gcode", 1_843_200, 3600),
            ("calibration_cube.gcode", 412_000, 1500),
            ("phone_stand_PETG.gcode", 3_400_000, 7300),
            ("gears/planetary_set.gcode", 2_100_000, 5400),
        ]
        self.job_id = 101

    def step(self):
        with LOCK:
            now = time.time()
            dt = now - self.last
            self.last = now
            for t in (self.tool, self.bed, self.chamber) + ((self.tool1,) if self.tool1 else ()):
                goal = t[1] if t[1] > 0 else 24.0
                t[0] += (goal - t[0]) * min(1.0, dt * 0.25) + random.uniform(-0.15, 0.15)
            if self.state == "printing":
                self.elapsed += dt * SPEED * self.speed / 100.0
                z = 0.2 + self.progress() * 0.48
                self.pos = [100 + 40 * math.sin(now * 1.3), 100 + 40 * math.cos(now * 0.9), z]
                if self.elapsed >= self.total:
                    self.elapsed = self.total
                    self.state = "done"
                    self.tool[1] = 0
                    self.bed[1] = 0
                    log(self.name, "finished", self.file)

    def progress(self):
        return max(0.0, min(1.0, self.elapsed / self.total)) if self.total else 0.0

    def remaining(self):
        return max(0, int((self.total - self.elapsed) / max(SPEED, 1e-6) * 100 / max(self.speed, 1)))

    def active(self):
        return self.state in ("printing", "paused")

    def layer(self):
        return int(self.progress() * self.layers)

    # commands, in the printer's own words
    def pause(self):
        with LOCK:
            if self.state == "printing":
                self.state = "paused"
                log(self.name, "paused")

    def resume(self):
        with LOCK:
            if self.state == "paused":
                self.state = "printing"
                log(self.name, "resumed")

    def cancel(self):
        with LOCK:
            if self.active():
                self.state = "cancelled"
                self.tool[1] = 0
                self.bed[1] = 0
                log(self.name, "cancelled")

    def start(self, path):
        with LOCK:
            name = path.split("/")[-1]
            match = [f for f in self.files if f[0].endswith(name) or f[0] == path]
            total = match[0][2] if match else 2400
            self.file = name
            self.total = total
            self.elapsed = 0
            self.state = "printing"
            self.tool[1] = 215
            self.bed[1] = 60
            self.job_id += 1
            log(self.name, "start", path)

    def delete(self, path):
        with LOCK:
            self.files = [f for f in self.files if not (f[0] == path or f[0].endswith("/" + path) or path.endswith(f[0]))]
            log(self.name, "delete", path)

    def gcode(self, text):
        """the G-code the drivers send; returns what a printer would answer"""
        out = []
        for line in text.replace("\r", "").split("\n"):
            line = line.strip()
            if not line:
                continue
            with LOCK:
                self.console.append((time.time(), "command", line))
            words = line.split()
            cmd = words[0].upper()
            args = {w[0].upper(): w[1:] for w in words[1:] if w}
            def num(k, d=0.0):
                try:
                    return float(args.get(k, d))
                except ValueError:
                    return d
            with LOCK:
                if cmd == "M104" or cmd == "G10":
                    tool = int(num("T", num("P", 0)))
                    if tool == 1 and self.tool1:
                        self.tool1[1] = num("S")
                    else:
                        self.tool[1] = num("S")
                elif cmd == "M140":
                    self.bed[1] = num("S")
                elif cmd == "M141":
                    self.chamber[1] = num("S")
                elif cmd == "M106":
                    self.fan = int(round(num("S", 255) * 100 / 255))
                elif cmd == "M107":
                    self.fan = 0
                elif cmd == "M220":
                    self.speed = int(num("S", 100))
                elif cmd == "M221":
                    self.flow = int(num("S", 100))
                elif cmd == "G28":
                    self.pos = [0.0, 0.0, 0.0] if len(words) == 1 else [
                        0.0 if "X" in args else self.pos[0], 0.0 if "Y" in args else self.pos[1],
                        0.0 if "Z" in args else self.pos[2]]
                elif cmd == "G1" and not self.active():
                    for i, a in enumerate("XYZ"):
                        if a in args:
                            self.pos[i] += num(a)
                elif cmd == "M114":
                    out.append("X:%.2f Y:%.2f Z:%.2f E:0.00" % tuple(self.pos))
                elif cmd == "M105":
                    out.append("T:%.1f /%.1f B:%.1f /%.1f" % (self.tool[0], self.tool[1], self.bed[0], self.bed[1]))
                elif cmd == "M112":
                    self.state = "error"
            out.append("ok")
        with LOCK:
            for o in out:
                self.console.append((time.time(), "response", o))
            self.console = self.console[-200:]
        log(self.name, "gcode", text.replace("\n", " | "))
        return "\n".join(out)


# --------------------------------------------------------------------------- pictures

def picture(p, w=640, h=480):
    if Image is None:
        return bytes.fromhex("ffd8ffe000104a46494600010100000100010000ffdb004300") + b"\x08" * 64 + b"\xff\xd9"
    img = Image.new("RGB", (w, h), (34, 36, 40))
    d = ImageDraw.Draw(img)
    d.rectangle([60, h - 120, w - 60, h - 90], fill=(70, 72, 78))           # the bed
    done = p.progress()
    ph = int(160 * done)
    d.rectangle([w // 2 - 70, h - 120 - ph, w // 2 + 70, h - 120], fill=(48, 209, 88))   # the part
    x = int(w // 2 - 70 + (math.sin(time.time() * 2) + 1) * 70)
    d.rectangle([60, h - 150 - ph, w - 60, h - 140 - ph], fill=(150, 150, 150))          # the gantry
    d.rectangle([x - 18, h - 172 - ph, x + 18, h - 130 - ph], fill=(255, 159, 10))       # the head
    d.text((16, 14), "%s  %s  %d%%" % (p.name, time.strftime("%H:%M:%S"), int(done * 100)), fill=(255, 255, 255))
    out = io.BytesIO()
    img.save(out, "JPEG", quality=70)
    return out.getvalue()


def thumbnail():
    if Image is None:
        return b""
    img = Image.new("RGB", (220, 220), (28, 28, 30))
    d = ImageDraw.Draw(img)
    d.polygon([(110, 30), (190, 75), (110, 120), (30, 75)], fill=(120, 230, 140))
    d.polygon([(30, 75), (110, 120), (110, 200), (30, 155)], fill=(48, 170, 80))
    d.polygon([(190, 75), (110, 120), (110, 200), (190, 155)], fill=(30, 130, 60))
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


THUMB = thumbnail()


# --------------------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    printer = None
    kind = ""

    def log_message(self, fmt, *a):
        pass

    def reply(self, code, body=b"", ctype="application/json", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def jbody(self):
        try:
            return json.loads(self.body() or b"{}")
        except ValueError:
            return {}

    def mjpeg(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            while True:
                jpg = picture(self.printer)
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpg))
                self.wfile.write(jpg + b"\r\n")
                time.sleep(0.2)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    def route(self, method):
        self.printer.step()
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query, keep_blank_values=True).items()}
        if u.path.startswith("/webcam/"):
            if "stream" in u.query:
                return self.mjpeg()
            return self.reply(200, picture(self.printer), "image/jpeg")
        fn = getattr(self, self.kind, None)
        return fn(method, urllib.parse.unquote(u.path), q)

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_PUT(self):
        self.route("PUT")

    def do_DELETE(self):
        self.route("DELETE")

    # ---- OctoPrint
    def octoprint(self, m, path, q):
        p = self.printer
        if self.headers.get("X-Api-Key") != "testkey":
            return self.reply(403, {"error": "Invalid API key"})
        flags = {"operational": True, "printing": p.state == "printing", "paused": p.state == "paused",
                 "pausing": False, "cancelling": False, "error": p.state == "error", "ready": not p.active(),
                 "closedOrError": False, "sdReady": True}
        texts = {"printing": "Printing", "paused": "Paused", "error": "Error"}
        if path == "/api/version":
            return self.reply(200, {"api": "0.1", "server": "1.10.3", "text": "OctoPrint 1.10.3"})
        if path == "/api/settings":
            return self.reply(200, {"webcam": {"webcamEnabled": True,
                                               "snapshotUrl": "http://127.0.0.1:%d/webcam/?action=snapshot" % self.server.server_port,
                                               "streamUrl": "/webcam/?action=stream"}})
        if path == "/api/printer" and m == "GET":
            return self.reply(200, {"temperature": {"tool0": {"actual": p.tool[0], "target": p.tool[1]},
                                                    "bed": {"actual": p.bed[0], "target": p.bed[1]}},
                                    "state": {"text": texts.get(p.state, "Operational"), "flags": flags}})
        if path == "/api/job" and m == "GET":
            job = {"file": {"name": p.file, "display": p.file, "origin": "local", "path": p.file}} if p.file else {"file": {}}
            st = {"printing": "Printing", "paused": "Paused", "cancelled": "Cancelled"}.get(p.state, "Operational")
            return self.reply(200, {"job": job, "state": st,
                                    "progress": {"completion": p.progress() * 100 if p.file else None,
                                                 "printTime": int(p.elapsed / SPEED),
                                                 "printTimeLeft": p.remaining() if p.active() else None}})
        if path == "/api/job":
            b = self.jbody()
            c = b.get("command")
            if c == "pause":
                (p.resume if b.get("action") == "resume" else p.pause)()
            elif c == "cancel":
                p.cancel()
            return self.reply(204)
        if path == "/api/connection":
            if m == "POST":
                log(p.name, "connection", self.jbody())
                return self.reply(204)
            return self.reply(200, {"current": {"state": "Operational", "port": "/dev/ttyACM0"}})
        if path == "/api/printer/printhead":
            b = self.jbody()
            if b.get("command") == "jog":
                p.gcode("G91\nG1 X%s Y%s Z%s\nG90" % (b.get("x", 0), b.get("y", 0), b.get("z", 0)))
            elif b.get("command") == "home":
                p.gcode("G28 " + " ".join(a.upper() for a in b.get("axes", [])))
            elif b.get("command") == "feedrate":
                p.gcode("M220 S%d" % b.get("factor", 100))
            return self.reply(204)
        if path == "/api/printer/tool":
            b = self.jbody()
            if b.get("command") == "target":
                for k, v in b.get("targets", {}).items():
                    p.gcode("M104 T%s S%s" % (k[-1], v))
            elif b.get("command") == "flowrate":
                p.gcode("M221 S%d" % b.get("factor", 100))
            elif b.get("command") == "extrude":
                p.gcode("G1 E%s" % b.get("amount"))
            return self.reply(204)
        if path == "/api/printer/bed":
            p.gcode("M140 S%s" % self.jbody().get("target", 0))
            return self.reply(204)
        if path == "/api/printer/command":
            b = self.jbody()
            p.gcode("\n".join(b.get("commands", [b.get("command", "")])))
            return self.reply(204)
        if path == "/api/files" or path == "/api/files/local" and m == "GET":
            files, folders = [], {}
            for name, size, est in p.files:
                e = {"name": name.split("/")[-1], "display": name.split("/")[-1], "path": name, "type": "machinecode",
                     "origin": "local", "size": size, "date": 1761300000 + size % 9999,
                     "gcodeAnalysis": {"estimatedPrintTime": est}}
                if "/" in name:
                    folders.setdefault(name.split("/")[0], []).append(e)
                else:
                    files.append(e)
            for f, ch in folders.items():
                files.append({"name": f, "path": f, "type": "folder", "children": ch})
            return self.reply(200, {"files": files, "free": 1 << 30})
        if path.startswith("/api/files/local/"):
            name = path[len("/api/files/local/"):]
            if m == "POST":
                p.start(name)
                return self.reply(204)
            if m == "DELETE":
                p.delete(name)
                return self.reply(204)
            return self.reply(200, {"name": name, "thumbnail": "plugin/prusaslicerthumbnails/thumbnail/%s.png?20251025" % name})
        if path.startswith("/plugin/prusaslicerthumbnails/thumbnail/"):
            return self.reply(200, THUMB, "image/png")
        return self.reply(404, {"error": "not found"})

    # ---- PrusaLink
    def digest_ok(self, m):
        a = self.headers.get("Authorization", "")
        if not a.startswith("Digest "):
            return False
        f = dict()
        for part in a[7:].split(","):
            if "=" in part:
                k, v = part.strip().split("=", 1)
                f[k] = v.strip('"')
        ha1 = hashlib.md5(("maker:Printer API:testpass").encode()).hexdigest()
        ha2 = hashlib.md5(("%s:%s" % (m, f.get("uri", ""))).encode()).hexdigest()
        if "qop" in f:
            want = hashlib.md5(("%s:%s:%s:%s:%s:%s" % (ha1, f["nonce"], f["nc"], f["cnonce"], f["qop"], ha2)).encode()).hexdigest()
        else:
            want = hashlib.md5(("%s:%s:%s" % (ha1, f.get("nonce"), ha2)).encode()).hexdigest()
        return f.get("username") == "maker" and f.get("response") == want and f.get("nonce") == "p4osnonce42"

    def prusalink(self, m, path, q):
        p = self.printer
        if not self.digest_ok(m):
            return self.reply(401, {"title": "Unauthorized"},
                              headers={"WWW-Authenticate": 'Digest realm="Printer API", nonce="p4osnonce42"'})
        if path.startswith("/thumb/"):
            return self.reply(200, THUMB, "image/png")
        st = {"printing": "PRINTING", "paused": "PAUSED", "done": "FINISHED", "cancelled": "STOPPED",
              "error": "ERROR"}.get(p.state, "IDLE")
        if path == "/api/version":
            return self.reply(200, {"api": "2.0.0", "server": "2.1.2", "text": "PrusaLink", "hostname": "core-one",
                                    "printer": {"type": "CORE One"}})
        if path == "/api/v1/status":
            out = {"printer": {"state": st, "temp_nozzle": p.tool[0], "target_nozzle": p.tool[1],
                               "temp_bed": p.bed[0], "target_bed": p.bed[1], "axis_x": p.pos[0], "axis_y": p.pos[1],
                               "axis_z": p.pos[2], "flow": p.flow, "speed": p.speed, "fan_print": p.fan * 70}}
            if p.state in ("printing", "paused", "done"):
                out["job"] = {"id": p.job_id, "progress": round(p.progress() * 100, 1),
                              "time_remaining": p.remaining(), "time_printing": int(p.elapsed / SPEED)}
            return self.reply(200, out)
        if path == "/api/v1/job":
            if not p.active() and p.state != "done":
                return self.reply(204)
            return self.reply(200, {"id": p.job_id, "state": st, "file": {
                "name": p.file.upper()[:8], "display_name": p.file.replace(".gcode", ".bgcode"), "path": "/usb",
                "refs": {"thumbnail": "/thumb/l/usb/" + p.file}}})
        if path.startswith("/api/v1/job/"):
            parts = path.split("/")
            if m == "DELETE":
                p.cancel()
            elif parts[-1] == "pause":
                p.pause()
            elif parts[-1] == "resume":
                p.resume()
            return self.reply(204)
        if path == "/api/v1/storage":
            return self.reply(200, {"storage_list": [{"name": "usb", "path": "/usb", "type": "USB", "available": True}]})
        if path.startswith("/api/v1/files/usb"):
            rest = path[len("/api/v1/files/usb"):].strip("/")
            if m == "POST":
                p.start(rest)
                return self.reply(201)
            if m == "DELETE":
                p.delete(rest)
                return self.reply(204)
            children = []
            names = [f for f in p.files if (rest == "" and "/" not in f[0]) or f[0].startswith(rest + "/")]
            for name, size, est in names:
                short = name.split("/")[-1]
                children.append({"name": short.upper()[:8] + "~1.BGC", "display_name": short.replace(".gcode", ".bgcode"),
                                 "type": "PRINT_FILE", "size": size, "m_timestamp": 1761300000 + size % 9999,
                                 "meta": {"estimated print time": est}})
            if rest == "":
                children.append({"name": "gears", "display_name": "gears", "type": "FOLDER"})
            return self.reply(200, {"type": "FOLDER", "name": rest or "usb", "children": children})
        return self.reply(404, {"title": "Not Found"})

    # ---- Moonraker
    def moonraker(self, m, path, q):
        p = self.printer
        ps = {"printing": "printing", "paused": "paused", "done": "complete", "cancelled": "cancelled",
              "error": "error"}.get(p.state, "standby")
        if path == "/printer/info":
            return self.reply(200, {"result": {"state": "ready", "hostname": "voron24", "software_version": "v0.12.0-301"}})
        if path == "/printer/objects/query":
            st = {"webhooks": {"state": "ready", "state_message": "Printer is ready"},
                  "print_stats": {"filename": p.file, "state": ps, "print_duration": p.elapsed / SPEED,
                                  "message": "", "info": {"current_layer": p.layer(), "total_layer": p.layers}},
                  "virtual_sdcard": {"progress": p.progress(), "is_active": p.state == "printing"},
                  "display_status": {"progress": p.progress()},
                  "extruder": {"temperature": p.tool[0], "target": p.tool[1]},
                  "heater_bed": {"temperature": p.bed[0], "target": p.bed[1]},
                  "temperature_sensor chamber": {"temperature": p.chamber[0]},
                  "fan": {"speed": p.fan / 100.0},
                  "gcode_move": {"speed_factor": p.speed / 100.0, "extrude_factor": p.flow / 100.0},
                  "toolhead": {"position": p.pos + [0.0], "homed_axes": "xyz"}}
            return self.reply(200, {"result": {"eventtime": time.time(), "status": st}})
        if path == "/server/files/metadata":
            return self.reply(200, {"result": {"filename": q.get("filename"), "estimated_time": p.total / SPEED,
                                               "layer_count": p.layers,
                                               "thumbnails": [{"width": 32, "height": 32, "relative_path": ".thumbs/a-32x32.png"},
                                                              {"width": 220, "height": 220, "relative_path": ".thumbs/a-220x220.png"}]}})
        if path.startswith("/server/files/gcodes/") and m == "GET":
            return self.reply(200, THUMB, "image/png")
        if path.startswith("/server/files/gcodes/") and m == "DELETE":
            p.delete(path[len("/server/files/gcodes/"):])
            return self.reply(200, {"result": {}})
        if path == "/server/webcams/list":
            # a real one says "/webcam/...", served by nginx on port 80; this one has no port 80
            base = "http://127.0.0.1:%d" % self.server.server_port
            return self.reply(200, {"result": {"webcams": [{"name": "cam", "enabled": True,
                                                            "snapshot_url": base + "/webcam/?action=snapshot",
                                                            "stream_url": base + "/webcam/?action=stream"}]}})
        if path == "/server/gcode_store":
            n = int(q.get("count", 50))
            with LOCK:
                items = [{"message": msg, "time": t, "type": ty} for t, ty, msg in p.console[-n:]]
            return self.reply(200, {"result": {"gcode_store": items}})
        if path == "/server/files/list":
            return self.reply(200, {"result": [{"path": n, "modified": 1761300000 + s % 9999, "size": s}
                                               for n, s, e in p.files]})
        if path == "/printer/print/pause":
            p.pause()
        elif path == "/printer/print/resume":
            p.resume()
        elif path == "/printer/print/cancel":
            p.cancel()
        elif path == "/printer/print/start":
            p.start(q.get("filename", ""))
        elif path == "/printer/emergency_stop":
            p.gcode("M112")
        elif path == "/printer/gcode/script":
            script = q.get("script", "")
            if script.startswith("SET_HEATER_TEMPERATURE"):
                a = dict(w.split("=") for w in script.split()[1:])
                h = {"extruder": "M104 S", "heater_bed": "M140 S", "chamber": "M141 S"}.get(a.get("HEATER"), "M104 S")
                p.gcode(h + a.get("TARGET", "0"))
            else:
                p.gcode(script)
        else:
            return self.reply(404, {"error": {"code": 404, "message": "Not Found"}})
        return self.reply(200, {"result": "ok"})

    # ---- Duet
    def duet(self, m, path, q):
        p = self.printer
        if path == "/rr_connect":
            if q.get("password", "") != "testpass":
                return self.reply(200, {"err": 1})
            return self.reply(200, {"err": 0, "sessionTimeout": 8000, "boardType": "duet3mb6hc", "apiLevel": 1,
                                    "sessionKey": 4242})
        if path in ("/rr_disconnect",):
            return self.reply(200, {"err": 0})
        if self.headers.get("X-Session-Key") != "4242":
            return self.reply(401, "")
        st = {"printing": "processing", "paused": "paused", "error": "halted"}.get(p.state, "idle")
        if path == "/rr_model":
            key = q.get("key", "")
            heaters = [{"current": p.bed[0], "active": p.bed[1], "state": "active" if p.bed[1] else "off"},
                       {"current": p.tool[0], "active": p.tool[1], "state": "active" if p.tool[1] else "off"}]
            parts = {
                "state": {"status": st, "machineMode": "FFF", "displayMessage": ""},
                "heat": {"bedHeaters": [0, -1], "chamberHeaters": [-1], "heaters": heaters},
                "job": {"file": {"fileName": "0:/gcodes/" + p.file, "size": 100000, "numLayers": p.layers},
                        "filePosition": int(p.progress() * 100000), "duration": int(p.elapsed / SPEED),
                        "layer": p.layer(), "timesLeft": {"slicer": p.remaining(), "file": p.remaining()}},
                "move": {"axes": [{"userPosition": p.pos[0]}, {"userPosition": p.pos[1]}, {"userPosition": p.pos[2]}],
                         "speedFactor": p.speed / 100.0, "extruders": [{"factor": p.flow / 100.0}]},
                "tools": [{"heaters": [1]}],
                "fans": [{"requestedValue": p.fan / 100.0}],
            }
            return self.reply(200, {"key": key, "flags": q.get("flags", ""), "result": parts.get(key, {})})
        if path == "/rr_gcode":
            g = q.get("gcode", "")
            if g == "M25":
                p.pause()
            elif g == "M24":
                p.resume()
            elif g == "M0":
                p.cancel()
            elif g.startswith("M32"):
                p.start(g[3:].strip().strip('"').replace("0:/gcodes/", ""))
            else:
                self.server.reply_text = p.gcode(g)
                return self.reply(200, {"buff": 255})
            self.server.reply_text = "ok"
            return self.reply(200, {"buff": 255})
        if path == "/rr_reply":
            t = getattr(self.server, "reply_text", "")
            self.server.reply_text = ""
            return self.reply(200, t, "text/plain")
        if path == "/rr_filelist":
            return self.reply(200, {"dir": "0:/gcodes", "first": 0, "next": 0,
                                    "files": [{"type": "f", "name": n.split("/")[-1], "size": s} for n, s, e in p.files]})
        if path == "/rr_delete":
            p.delete(q.get("name", "").replace("0:/gcodes/", ""))
            return self.reply(200, {"err": 0})
        return self.reply(404, "")

    # ---- FlashForge
    def flashforge(self, m, path, q):
        p = self.printer
        b = self.jbody()
        if b.get("serialNumber") != "SNFF0001" or b.get("checkCode") != "1234":
            return self.reply(200, {"code": 1, "message": "check code error"})
        st = {"printing": "printing", "paused": "paused", "done": "completed", "cancelled": "cancel",
              "error": "error"}.get(p.state, "ready")
        if path == "/detail":
            return self.reply(200, {"code": 0, "message": "Success", "detail": {
                "status": st, "printFileName": p.file, "printProgress": p.progress(),
                "printDuration": int(p.elapsed / SPEED), "estimatedTime": p.remaining(),
                "rightTemp": p.tool[0], "rightTargetTemp": p.tool[1], "platTemp": p.bed[0],
                "platTargetTemp": p.bed[1], "chamberTemp": p.chamber[0], "printLayer": p.layer(),
                "targetPrintLayer": p.layers, "coolingFanSpeed": p.fan, "printSpeedAdjust": p.speed,
                "lightStatus": "open" if p.light else "close", "name": "Adventurer 5M Pro",
                "firmwareVersion": "2.7.5", "errorCode": "",
                "cameraStreamUrl": "http://127.0.0.1:%d/webcam/?action=stream" % self.server.server_port}})
        if path == "/control":
            pl = b.get("payload", {})
            cmd, args = pl.get("cmd"), pl.get("args", {})
            if cmd == "jobCtl_cmd":
                {"pause": p.pause, "continue": p.resume, "cancel": p.cancel}.get(args.get("action"), lambda: None)()
            elif cmd == "lightControl_cmd":
                p.light = 1 if args.get("status") == "open" else 0
            elif cmd == "printerCtl_cmd":
                p.speed = int(args.get("speed", p.speed))
                p.fan = int(args.get("coolingFan", p.fan))
            log(p.name, "control", cmd, args)
            return self.reply(200, {"code": 0, "message": "Success"})
        if path == "/gcodeList":
            return self.reply(200, {"code": 0, "gcodeList": [n.split("/")[-1] for n, s, e in p.files]})
        if path == "/printGcode":
            p.start(b.get("fileName", ""))
            return self.reply(200, {"code": 0, "message": "Success"})
        return self.reply(404, {"code": 404})


def serve_http(port, kind, printer):
    h = type("H_" + kind, (Handler,), {"printer": printer, "kind": kind})
    srv = ThreadingHTTPServer(("127.0.0.1", port), h)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log("%-11s http://127.0.0.1:%d" % (kind, port))


# --------------------------------------------------------------------------- FlashForge's G-code port

def serve_ff_tcp(printer, port=8899):
    def client(c):
        f = c.makefile("rb")
        try:
            for raw in f:
                line = raw.decode(errors="replace").strip().lstrip("~")
                printer.step()
                if line.startswith("M601"):
                    out = "CMD M601 Received.\r\nControl Success V2.1.\r\nok\r\n"
                elif line.startswith("M119"):
                    st = {"printing": "BUILDING_FROM_SD", "paused": "PAUSED"}.get(printer.state, "READY")
                    out = ("CMD M119 Received.\r\nEndstop: X-max:0 Y-max:0 Z-min:0\r\nMachineStatus: %s\r\n"
                           "MoveMode: READY\r\nStatus: S:1 L:0 J:0 F:0\r\nLED: 1\r\nCurrentFile: %s\r\nok\r\n" % (st, printer.file))
                elif line.startswith("M105"):
                    out = "CMD M105 Received.\r\nT0:%d /%d B:%d /%d\r\nok\r\n" % (printer.tool[0], printer.tool[1], printer.bed[0], printer.bed[1])
                elif line.startswith("M27"):
                    out = "CMD M27 Received.\r\nSD printing byte %d/100\r\nLayer: %d/%d\r\nok\r\n" % (
                        int(printer.progress() * 100), printer.layer(), printer.layers)
                elif line.startswith("M115"):
                    out = "CMD M115 Received.\r\nMachine Type: Adventurer 5M Pro\r\nMachine Name: AD5M\r\nFirmware: v2.7.5\r\nok\r\n"
                elif line.startswith("M25"):
                    printer.pause(); out = "CMD M25 Received.\r\nok\r\n"
                elif line.startswith("M24"):
                    printer.resume(); out = "CMD M24 Received.\r\nok\r\n"
                elif line.startswith("M26"):
                    printer.cancel(); out = "CMD M26 Received.\r\nok\r\n"
                else:
                    ans = printer.gcode(line)
                    out = "CMD %s Received.\r\n%s\r\n" % (line.split()[0] if line else "", ans.replace("\n", "\r\n"))
                c.sendall(out.encode())
        except OSError:
            pass
        c.close()

    try:
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", port))
        s.listen(4)
    except OSError as e:
        log("flashforge  G-code port %d not available: %s" % (port, e))
        return
    log("flashforge  tcp://127.0.0.1:%d (G-code)" % port)

    def loop():
        while True:
            c, _ = s.accept()
            threading.Thread(target=client, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()


# --------------------------------------------------------------------------- WebSocket

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_accept(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(4096)
        if not chunk:
            return False
        data += chunk
    key = ""
    for line in data.decode(errors="replace").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
    return True


def ws_send(c, text, opcode=1):
    b = text.encode() if isinstance(text, str) else text
    n = len(b)
    if n < 126:
        head = struct.pack("!BB", 0x80 | opcode, n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    c.sendall(head + b)


def ws_recv(c, buf):
    """one whole message from buf (bytearray filled by the caller): (opcode, payload) or None"""
    if len(buf) < 2:
        return None
    b0, b1 = buf[0], buf[1]
    n = b1 & 0x7F
    off = 2
    if n == 126:
        if len(buf) < 4:
            return None
        n = struct.unpack("!H", bytes(buf[2:4]))[0]
        off = 4
    elif n == 127:
        if len(buf) < 10:
            return None
        n = struct.unpack("!Q", bytes(buf[2:10]))[0]
        off = 10
    mask = b""
    if b1 & 0x80:
        mask = bytes(buf[off:off + 4])
        off += 4
    if len(buf) < off + n:
        return None
    data = bytes(buf[off:off + n])
    if mask:
        data = bytes(x ^ mask[i % 4] for i, x in enumerate(data))
    del buf[:off + n]
    return b0 & 0x0F, data


def serve_ws(port, kind, printer, handler):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    log("%-11s ws://127.0.0.1:%d" % (kind, port))

    def client(c):
        try:
            if not ws_accept(c):
                return
            c.settimeout(0.3)
            buf = bytearray()
            state = {"last": 0.0}
            handler(c, None, state)
            while True:
                try:
                    chunk = c.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                except socket.timeout:
                    pass
                while True:
                    m = ws_recv(c, buf)
                    if not m:
                        break
                    op, data = m
                    if op == 8:
                        return
                    if op == 9:
                        ws_send(c, data, 10)
                        continue
                    if op == 1:
                        handler(c, data.decode(errors="replace"), state)
                printer.step()
                handler(c, None, state)
        except OSError:
            pass
        finally:
            c.close()

    def loop():
        while True:
            c, _ = s.accept()
            threading.Thread(target=client, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()


def elegoo_handler(printer, board):
    def status_msg():
        p = printer
        ps = {"printing": 13, "paused": 6, "done": 9, "cancelled": 8}.get(p.state, 0)
        cur = 1 if p.active() else 0
        return json.dumps({"Status": {
            "CurrentStatus": [cur], "PreviousStatus": 0, "TempOfNozzle": p.tool[0], "TempTargetNozzle": p.tool[1],
            "TempOfHotbed": p.bed[0], "TempTargetHotbed": p.bed[1], "TempOfBox": p.chamber[0], "TempTargetBox": 0,
            "CurrenCoord": "%.2f,%.2f,%.2f" % tuple(p.pos), "CurrentFanSpeed": {"ModelFan": p.fan, "AuxiliaryFan": 0, "BoxFan": 0},
            "LightStatus": {"SecondLight": p.light}, "PrintSpeedPct": p.speed,
            "PrintInfo": {"Status": ps, "CurrentLayer": p.layer(), "TotalLayer": p.layers,
                          "CurrentTicks": int(p.elapsed / SPEED), "TotalTicks": int(p.elapsed / SPEED) + p.remaining(),
                          "Filename": "/local/" + p.file, "ErrorNumber": 0, "TaskId": "t1", "Progress": int(p.progress() * 100)}},
            "MainboardID": board, "TimeStamp": int(time.time()), "Topic": "sdcp/status/" + board})

    def h(c, text, state):
        now = time.time()
        if text is None:
            if now - state["last"] > 2:
                state["last"] = now
                ws_send(c, status_msg())
            return
        if text == "ping":
            ws_send(c, "pong")
            return
        try:
            j = json.loads(text)
        except ValueError:
            return
        d = j.get("Data", {})
        cmd, data = d.get("Cmd"), d.get("Data", {})
        if d.get("MainboardID") != board:
            log("elegoo: request for", d.get("MainboardID"))
        resp = {"Ack": 0}
        if cmd == 0:
            ws_send(c, status_msg())
        elif cmd == 1:
            ws_send(c, json.dumps({"Attributes": {"Name": "Centauri Carbon", "MachineName": "Centauri Carbon",
                                                  "FirmwareVersion": "V1.1.29", "MainboardID": board},
                                   "MainboardID": board, "Topic": "sdcp/attributes/" + board}))
        elif cmd == 129:
            printer.pause()
        elif cmd == 131:
            printer.resume()
        elif cmd == 130:
            printer.cancel()
        elif cmd == 128:
            printer.start(data.get("Filename", ""))
        elif cmd == 258:
            resp["FileList"] = [{"name": "/local/" + n.split("/")[-1], "usedSize": s, "type": 1} for n, s, e in printer.files]
        elif cmd == 259:
            for f in data.get("FileList", []):
                printer.delete(f.replace("/local/", ""))
        elif cmd == 386:
            resp["VideoUrl"] = "127.0.0.1:%d/webcam/?action=stream" % (BASE + 1)
        elif cmd == 403:
            g = []
            if "TempTargetNozzle" in data:
                g.append("M104 S%s" % data["TempTargetNozzle"])
            if "TempTargetHotbed" in data:
                g.append("M140 S%s" % data["TempTargetHotbed"])
            if "TargetFanSpeed" in data:
                printer.fan = data["TargetFanSpeed"].get("ModelFan", printer.fan)
            if "PrintSpeedPct" in data:
                printer.speed = data["PrintSpeedPct"]
            if "LightStatus" in data:
                printer.light = data["LightStatus"].get("SecondLight", printer.light)
            if g:
                printer.gcode("\n".join(g))
        elif cmd == 402:
            printer.gcode("G28 " + " ".join(data.get("Axis", "")))
        elif cmd == 401:
            printer.gcode("G91\nG1 %s%s\nG90" % (data.get("Axis"), data.get("Step")))
        log("elegoo cmd", cmd, data)
        ws_send(c, json.dumps({"Id": "", "Data": {"Cmd": cmd, "Data": resp, "RequestID": d.get("RequestID"),
                                                  "MainboardID": board}, "Topic": "sdcp/response/" + board}))
    return h


def creality_handler(printer):
    def full():
        p = printer
        st = {"printing": 1, "done": 2, "error": 3, "cancelled": 4, "paused": 5}.get(p.state, 0)
        return json.dumps({"nozzleTemp": "%.2f" % p.tool[0], "targetNozzleTemp": int(p.tool[1]),
                           "bedTemp0": "%.2f" % p.bed[0], "targetBedTemp0": int(p.bed[1]), "boxTemp": int(p.chamber[0]),
                           "printProgress": int(p.progress() * 100), "printJobTime": int(p.elapsed / SPEED),
                           "printLeftTime": p.remaining(), "printFileName": "/usr/data/printer_data/gcodes/" + p.file,
                           "state": st, "deviceState": 0, "layer": p.layer(), "TotalLayer": p.layers,
                           "curPosition": "X:%.2f Y:%.2f Z:%.2f" % tuple(p.pos), "curFeedratePct": p.speed,
                           "curFlowratePct": p.flow, "modelFanPct": p.fan, "lightSw": p.light, "model": "K1C",
                           "modelVersion": "1.3.3.46", "err": {"errcode": 0, "key": 0}})

    def h(c, text, state):
        now = time.time()
        if text is None:
            if now - state["last"] > 2:
                state["last"] = now
                ws_send(c, full())
                if int(now) % 10 == 0:
                    ws_send(c, json.dumps({"ModeCode": "heart_beat", "msg": time.strftime("%Y-%m-%d %H:%M:%S")}))
            return
        try:
            j = json.loads(text)
        except ValueError:
            return
        if "ModeCode" in j:
            return
        pr = j.get("params", {})
        if j.get("method") == "get":
            if "reqGcodeFile" in pr:
                ws_send(c, json.dumps({"retGcodeFileInfo": {"fileInfo": [
                    {"name": n.split("/")[-1], "path": "/usr/data/printer_data/gcodes", "file_size": s, "timeCost": e,
                     "create_time": 1761300000} for n, s, e in printer.files]}}))
            else:
                ws_send(c, full())
            return
        log("creality set", pr)
        if pr.get("pause") == 1:
            printer.pause()
        elif pr.get("pause") == 0:
            printer.resume()
        if pr.get("stop") == 1:
            printer.cancel()
        if "nozzleTempControl" in pr:
            printer.gcode("M104 S%s" % pr["nozzleTempControl"])
        if "bedTempControl" in pr:
            printer.gcode("M140 S%s" % pr["bedTempControl"].get("val", 0))
        if "gcodeCmd" in pr:
            printer.gcode(pr["gcodeCmd"])
        if "lightSw" in pr:
            printer.light = pr["lightSw"]
        if "setFeedratePct" in pr:
            printer.speed = pr["setFeedratePct"]
        if "opGcodeFile" in pr:
            op, _, path = pr["opGcodeFile"].partition(":")
            if op == "printprt":
                printer.start(path)
            elif op == "deleteprt":
                printer.delete(path.split("/")[-1])
        ws_send(c, full())
    return h


# --------------------------------------------------------------------------- Bambu: MQTT over TLS

def mqtt_read(c, buf):
    while True:
        if len(buf) >= 2:
            n, mul, i = 0, 1, 1
            while i < len(buf) and i < 5:
                n += (buf[i] & 0x7F) * mul
                mul *= 128
                if not buf[i] & 0x80:
                    break
                i += 1
            if i < len(buf) and len(buf) >= i + 1 + n:
                pkt = (buf[0], bytes(buf[i + 1:i + 1 + n]))
                del buf[:i + 1 + n]
                return pkt
        chunk = c.recv(65536)
        if not chunk:
            return None
        buf += chunk


def mqtt_pkt(t, body):
    n = len(body)
    out = bytearray([t])
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            break
    return bytes(out) + body


def mqtt_str(s):
    b = s.encode()
    return struct.pack("!H", len(b)) + b


def serve_bambu(port, printer, serial, code, ctx):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    log("bambu       mqtts://127.0.0.1:%d  serial %s" % (port, serial))

    def report(full):
        p = printer
        gs = {"printing": "RUNNING", "paused": "PAUSE", "done": "FINISH", "cancelled": "FAILED",
              "error": "FAILED"}.get(p.state, "IDLE")
        pr = {"nozzle_temper": round(p.tool[0], 1), "bed_temper": round(p.bed[0], 1),
              "mc_percent": int(p.progress() * 100), "mc_remaining_time": p.remaining() // 60,
              "layer_num": p.layer(), "command": "push_status", "msg": 1, "sequence_id": "0"}
        if full:
            pr.update({"nozzle_target_temper": p.tool[1], "bed_target_temper": p.bed[1], "chamber_temper": p.chamber[0],
                       "gcode_state": gs, "subtask_name": p.file.replace(".gcode", ""), "total_layer_num": p.layers,
                       "spd_lvl": getattr(p, "lvl", 2), "spd_mag": p.speed, "cooling_fan_speed": str(int(p.fan * 15 / 100)),
                       "print_error": 50348044 if p.state == "cancelled" else 0,
                       "lights_report": [{"node": "chamber_light", "mode": "on" if p.light else "off"}],
                       "ams": {"ams": [{"id": "0", "humidity": "4", "temp": "26.1", "tray": [
                           {"id": "0", "tray_type": "PLA", "tray_color": "FFFFFFFF", "remain": 80},
                           {"id": "1", "tray_type": "PETG", "tray_color": "0A84FFFF", "remain": 35},
                           {"id": "2", "tray_type": "PLA", "tray_color": "FF453AFF", "remain": 100},
                           {"id": "3"}]}], "tray_now": "1"},
                       "command": "push_status", "msg": 0})
        else:
            pr["gcode_state"] = gs
        return json.dumps({"print": pr})

    def client(raw):
        try:
            c = ctx.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError) as e:
            log("bambu: TLS failed:", e)
            raw.close()
            return
        buf = bytearray()
        sub = False
        topic_rep = "device/%s/report" % serial
        last = 0.0
        full_every = 0
        c.settimeout(0.5)
        try:
            while True:
                try:
                    pkt = mqtt_read(c, buf)
                    if pkt is None:
                        break
                except socket.timeout:
                    pkt = ("tick", b"")
                t, body = pkt
                if t == "tick":
                    pass
                elif t >> 4 == 1:                           # CONNECT
                    i = 2 + struct.unpack("!H", body[:2])[0] + 4
                    flags = body[i - 3]
                    def take(at):
                        n = struct.unpack("!H", body[at:at + 2])[0]
                        return body[at + 2:at + 2 + n].decode(), at + 2 + n
                    cid, i = take(i)
                    user = pw = ""
                    if flags & 0x80:
                        user, i = take(i)
                    if flags & 0x40:
                        pw, i = take(i)
                    ok = user == "bblp" and pw == code
                    c.sendall(mqtt_pkt(0x20, bytes([0, 0 if ok else 5])))
                    log("bambu: CONNECT", cid, "ok" if ok else "refused")
                    if not ok:
                        break
                elif t >> 4 == 8:                           # SUBSCRIBE
                    pid = body[:2]
                    c.sendall(mqtt_pkt(0x90, pid + b"\x00"))
                    sub = True
                elif t >> 4 == 3:                           # PUBLISH
                    tl = struct.unpack("!H", body[:2])[0]
                    payload = body[2 + tl:]
                    try:
                        j = json.loads(payload)
                    except ValueError:
                        j = {}
                    pr = j.get("print", {})
                    cmd = pr.get("command")
                    if "pushing" in j:
                        c.sendall(mqtt_pkt(0x30, mqtt_str(topic_rep) + report(True).encode()))
                    elif "info" in j:
                        c.sendall(mqtt_pkt(0x30, mqtt_str(topic_rep) + json.dumps({"info": {"command": "get_version", "module": [
                            {"name": "ota", "sw_ver": "01.08.02.00", "product_name": "Bambu Lab P1S"}]}}).encode()))
                    elif "system" in j:
                        printer.light = 1 if j["system"].get("led_mode") == "on" else 0
                        log("bambu: light", printer.light)
                    elif cmd == "pause":
                        printer.pause()
                    elif cmd == "resume":
                        printer.resume()
                    elif cmd == "stop":
                        printer.cancel()
                    elif cmd == "print_speed":
                        printer.lvl = int(pr.get("param", 2))
                        printer.speed = {1: 50, 2: 100, 3: 124, 4: 166}.get(printer.lvl, 100)
                    elif cmd == "gcode_line":
                        printer.gcode(pr.get("param", ""))
                    if cmd:
                        log("bambu:", cmd, pr.get("param", ""))
                        c.sendall(mqtt_pkt(0x30, mqtt_str(topic_rep) + report(True).encode()))
                elif t >> 4 == 12:                          # PINGREQ
                    c.sendall(mqtt_pkt(0xD0, b""))
                elif t >> 4 == 14:
                    break
                printer.step()
                now = time.time()
                if sub and now - last > 1.0:
                    last = now
                    full_every += 1
                    c.sendall(mqtt_pkt(0x30, mqtt_str(topic_rep) + report(full_every % 5 == 0).encode()))
        except (OSError, ssl.SSLError):
            pass
        finally:
            c.close()

    def loop():
        while True:
            c, _ = s.accept()
            threading.Thread(target=client, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()


def serve_bambu_cam(printer, code, ctx, port=6000):
    try:
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", port))
        s.listen(2)
    except OSError as e:
        log("bambu camera port %d not available: %s" % (port, e))
        return
    log("bambu       camera tls://127.0.0.1:%d" % port)

    def client(raw):
        try:
            c = ctx.wrap_socket(raw, server_side=True)
            auth = b""
            while len(auth) < 80:
                chunk = c.recv(80 - len(auth))
                if not chunk:
                    return
                auth += chunk
            if auth[16:20] != b"bblp" or auth[48:48 + len(code)] != code.encode():
                log("bambu camera: wrong login")
                return
            while True:
                jpg = picture(printer)
                c.sendall(struct.pack("<IIII", len(jpg), 0, 1, 0) + jpg)
                time.sleep(0.5)
        except (OSError, ssl.SSLError):
            pass
        finally:
            raw.close()

    def loop():
        while True:
            c, _ = s.accept()
            threading.Thread(target=client, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()


def make_certs(d):
    os.makedirs(d, exist_ok=True)
    ca, cak = os.path.join(d, "ca.pem"), os.path.join(d, "ca.key")
    crt, key = os.path.join(d, "printer.pem"), os.path.join(d, "printer.key")
    if not os.path.exists(crt):
        def run(*a):
            subprocess.run(a, check=True, capture_output=True)
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "30", "-keyout", cak, "-out", ca,
            "-subj", "/CN=P4OS fake printers CA")
        csr = os.path.join(d, "printer.csr")
        run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr, "-subj", "/CN=127.0.0.1")
        ext = os.path.join(d, "ext.cnf")
        # 127.0.0.1 twice, as an IP and as a name: mbedtls compares the name
        open(ext, "w").write("subjectAltName=IP:127.0.0.1,DNS:127.0.0.1,DNS:localhost\nbasicConstraints=CA:FALSE\n")
        run("openssl", "x509", "-req", "-in", csr, "-CA", ca, "-CAkey", cak, "-CAcreateserial", "-out", crt,
            "-days", "30", "-extfile", ext)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    return ctx, ca


CONFIG = """# fake printers (apps/impresoras/tools/fake_printers.py)

[OctoPrint (MK3S)]
type = octoprint
host = 127.0.0.1
port = {o}
key = testkey

[Prusa CORE One]
type = prusalink
host = 127.0.0.1
port = {p}
key = testpass

[Voron 2.4]
type = moonraker
host = 127.0.0.1
port = {m}

[Duet RRF]
type = duet
host = 127.0.0.1
port = {d}
key = testpass

[Adventurer 5M]
type = flashforge
host = 127.0.0.1
port = {f}
key = 1234
serial = SNFF0001

[Centauri Carbon]
type = elegoo
host = 127.0.0.1
port = {e}
serial = e1e2e3e4e5e6

[Creality K1C]
type = creality
host = 127.0.0.1
port = {c}

[Bambu P1S]
type = bambu
host = 127.0.0.1
port = {b}
key = 12345678
serial = 01P00A000000001
"""

BASE = 8300


def main():
    global SPEED, BASE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", type=int, default=8300)
    ap.add_argument("--speed", type=float, default=20.0, help="how much faster than real the jobs go")
    ap.add_argument("--certs", help="folder for the throwaway CA and certificate (Bambu)")
    ap.add_argument("--config", help="write an impresoras.txt with the eight")
    a = ap.parse_args()
    SPEED = a.speed
    BASE = b = a.base
    printers = {
        "octoprint": Printer("MK3S", total=5400, start=0.42),
        "prusalink": Printer("CORE One", total=3000, start=0.81, file="whistle.gcode"),
        "moonraker": Printer("Voron", total=7200, start=0.12, file="voron_cube_ABS.gcode"),
        "duet": Printer("Duet", total=2000, start=0.0),
        "flashforge": Printer("AD5M", total=4000, start=0.66, file="vase.gcode"),
        "elegoo": Printer("Centauri", total=3000, start=0.25, file="clip.gcode"),
        "creality": Printer("K1C", total=2600, start=0.5, file="hook.gcode"),
        "bambu": Printer("P1S", total=6000, start=0.3, file="plate_1.gcode"),
    }
    printers["duet"].state = "idle"
    printers["duet"].tool[1] = 0
    printers["duet"].bed[1] = 0
    printers["moonraker"].tool[1] = 245
    printers["moonraker"].bed[1] = 100
    serve_http(b + 1, "octoprint", printers["octoprint"])
    serve_http(b + 2, "prusalink", printers["prusalink"])
    serve_http(b + 3, "moonraker", printers["moonraker"])
    serve_http(b + 4, "duet", printers["duet"])
    serve_http(b + 5, "flashforge", printers["flashforge"])
    serve_ff_tcp(printers["flashforge"])
    serve_ws(b + 6, "elegoo", printers["elegoo"], elegoo_handler(printers["elegoo"], "e1e2e3e4e5e6"))
    serve_ws(b + 7, "creality", printers["creality"], creality_handler(printers["creality"]))
    if a.certs:
        ctx, ca = make_certs(a.certs)
        serve_bambu(b + 8, printers["bambu"], "01P00A000000001", "12345678", ctx)
        serve_bambu_cam(printers["bambu"], "12345678", ctx)
        log("for the simulator: P4_SIM_EXTRA_CA=%s" % ca)
    else:
        log("bambu: off (needs --certs DIR)")
    if a.config:
        os.makedirs(os.path.dirname(os.path.abspath(a.config)), exist_ok=True)
        open(a.config, "w").write(CONFIG.format(o=b + 1, p=b + 2, m=b + 3, d=b + 4, f=b + 5, e=b + 6, c=b + 7, b=b + 8))
        log("wrote", a.config)
    try:
        while True:
            time.sleep(1)
            for p in printers.values():
                p.step()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
