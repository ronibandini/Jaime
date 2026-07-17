# Jaime Arduino UNO Q Agentic robot
# Bridge between MCU and MPU
# Roni Bandini July 2026 - MIT License
# Run with $ nohup python3 bridge.py > bridge.log 2>&1 & 

import socket
import threading
import msgpack
import json
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer

SOCKET_PATH  = "/var/run/arduino-router.sock"
HTTP_PORT    = 8080
CALL_TIMEOUT = 20.0

def log(msg):
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"[{ts}] {msg}", flush=True)

class ArduinoBridge:
    def __init__(self):
        self.sock        = None
        self.counter     = 0
        self.running     = False
        self.cond        = threading.Condition()
        self.pending     = {}
        self.moveLock    = threading.Lock()

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.sock.connect(SOCKET_PATH)
            self.running = True
            t = threading.Thread(target=self._recvLoop, daemon=True)
            t.start()
            log(f"Connected to {SOCKET_PATH}")
            return True
        except Exception as e:
            log(f"connect error: {e}")
            return False

    def call(self, method, *args):
        startTime = time.time()
        log(f"→ CALL {method}({', '.join(str(a) for a in args)})")

        isMove = (method == "move" and args and args[0] != "stop")
        moveDuration = 0
        if isMove:
            log(f"  waiting for movement lock...")
            self.moveLock.acquire()
            try:
                moveDuration = float(args[1])
            except (IndexError, ValueError):
                moveDuration = 0

        with self.cond:
            self.counter += 1
            msgId = self.counter
            self.pending[msgId] = {"success": False, "result": None, "error": ""}

        payload = msgpack.packb([0, msgId, method, list(args)])
        try:
            self.sock.sendall(payload)
            log(f"  sent to MCU (msgId={msgId}, {len(payload)} bytes)")
        except Exception as e:
            log(f"  ✗ send failed: {e}")
            if isMove:
                self.moveLock.release()
            with self.cond:
                self.pending.pop(msgId, None)
            return {"success": False, "result": None, "error": str(e)}

        with self.cond:
            got = self.cond.wait_for(
                lambda: msgId not in self.pending or
                        self.pending[msgId]["success"] or
                        self.pending[msgId]["error"] != "",
                timeout=CALL_TIMEOUT
            )
            resp = self.pending.pop(msgId, {"success": False, "result": None,
                                            "error": "timeout"})

        elapsed = time.time() - startTime
        if resp.get("success"):
            log(f"  ✓ OK ({elapsed:.2f}s) result={resp.get('result')}")
        else:
            log(f"  ✗ FAILED ({elapsed:.2f}s) error={resp.get('error')}")

        if isMove:
            def _release(d, lock):
                time.sleep(d)
                lock.release()
                log(f"  movement lock released (after {d}s)")
            threading.Thread(target=_release, args=(moveDuration, self.moveLock),
                             daemon=True).start()

        return resp

    def disconnect(self):
        self.running = False
        try:
            self.sock.close()
        except Exception:
            pass

    def _recvLoop(self):
        unpacker = msgpack.Unpacker()
        while self.running:
            try:
                data = self.sock.recv(4096)
                if not data:
                    log("MCU socket closed by remote end")
                    break
                unpacker.feed(data)
                for msg in unpacker:
                    if not (isinstance(msg, list) and len(msg) >= 3):
                        continue
                    msgType = msg[0]

                    if msgType == 1 and len(msg) == 4:
                        msgId, error, result = msg[1], msg[2], msg[3]
                        with self.cond:
                            if msgId in self.pending:
                                if error is not None:
                                    self.pending[msgId]["error"] = str(error)
                                else:
                                    self.pending[msgId]["success"] = True
                                    self.pending[msgId]["result"]  = result
                                self.cond.notify_all()

                    elif msgType == 0 and len(msg) == 4:
                        msgId = msg[1]
                        method = msg[2]
                        log(f"← MCU pushed call: {method}({msg[3]})")
                        ack = msgpack.packb([1, msgId, None, "ok"])
                        try:
                            self.sock.sendall(ack)
                        except Exception:
                            pass

            except Exception as e:
                log(f"recvLoop error: {e}")
                break

bridge = ArduinoBridge()

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path == "/health":
            self._json(200, {"status": "ok"})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/command":
            self._json(404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            body   = json.loads(self.rfile.read(length))
            method = body["method"]
            args   = body.get("args", [])
            log(f"HTTP request from {self.client_address[0]}: {method} {args}")
            result = bridge.call(method, *args)

            if method == "readSensors" and result.get("success"):
                raw = result.get("result", "")
                if isinstance(raw, (bytes, bytearray)):
                    raw = raw.decode()
                if isinstance(raw, str) and "," in raw:
                    parts = raw.split(",")
                    result["result"] = {
                        "distance": int(parts[0]),
                        "line":     int(parts[1])
                    }

            self._json(200, result)
        except Exception as e:
            log(f"HTTP handler error: {e}")
            self._json(500, {"success": False, "error": str(e)})

    def _json(self, code, data):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

def run():
    log("Starting bridge...")
    if not bridge.connect():
        log(f"FATAL: cannot connect to {SOCKET_PATH}")
        return
    log(f"HTTP API listening on port {HTTP_PORT}")
    httpd = HTTPServer(("", HTTP_PORT), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("Shutting down (Ctrl+C)")
    finally:
        bridge.disconnect()
        httpd.server_close()

if __name__ == "__main__":
    run()