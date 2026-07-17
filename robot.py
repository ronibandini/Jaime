# robot.py — OpenClaw skill file
# Roni Bandini July 2026 - MIT License
#
# Place this file in your OpenClaw skill folder alongside PARKING.md.
# bridge.py must be running on the UNO Q and reachable at localhost:8080.
#
# Usage from CLI (test):
#   python3 robot.py "read sensors"
#   python3 robot.py "move forward 2 seconds"
#   python3 robot.py "rotate right 2 seconds"
#   python3 robot.py "forward until 20"
#   python3 robot.py "forward until line 2"
#
# Diagnostics: every call prints a console line with the HTTP request sent,
# elapsed time, and the parsed response. Set DEBUG = False to silence.

import json
import re
import time
import urllib.request

BRIDGE_URL = "http://127.0.0.1:8080"
DEBUG = True

def _log(msg):
    if DEBUG:
        print(f"[robot.py] {msg}")

# ─── HTTP helper ──────────────────────────────────────────────────────────────
def _call(method, *args):
    payload = json.dumps({"method": method, "args": list(args)}).encode()
    req = urllib.request.Request(
        BRIDGE_URL + "/command",
        data=payload,
        headers={"Content-Type": "application/json"}
    )
    _log(f"→ {method}({', '.join(str(a) for a in args)})")
    start = time.time()
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            result = json.loads(r.read().decode())
            elapsed = time.time() - start
            if result.get("success"):
                _log(f"  ✓ ({elapsed:.2f}s) → {result.get('result')}")
            else:
                _log(f"  ✗ ({elapsed:.2f}s) error: {result.get('error')}")
            return result
    except Exception as e:
        elapsed = time.time() - start
        _log(f"  ✗ ({elapsed:.2f}s) request failed: {e}")
        return {"success": False, "error": str(e)}

# ─── Robot primitive functions (all routed through bridge.py → sketch) ───────
# Cambiado int(seconds) por float(seconds) para soportar decimales
def forward(seconds=1.0):    return _call("move", "forward", float(seconds))
def back(seconds=1.0):       return _call("move", "back",    float(seconds))
def turn_left(seconds=1.0):  return _call("move", "left",    float(seconds))
def turn_right(seconds=1.0): return _call("move", "right",   float(seconds))
def stop():                return _call("move", "stop",    0.0)

def forward_until(target_cm):
    """Drive forward until ultrasonic reads <= target_cm (15s MCU timeout)."""
    return _call("forwardUntil", int(target_cm))

def forward_until_line(line_number):
    """Drive forward until the Nth line is crossed (20s MCU timeout).
    Aborts early if the ultrasonic sensor detects a wall under 30cm.
    result["result"] is the number of lines actually crossed: equal to
    line_number on a normal finish, or lower if it stopped early for a wall."""
    return _call("forwardUntilLine", int(line_number))

def back_until_line(line_number):
    """Drive backward until the Nth line is crossed (20s MCU timeout)."""
    return _call("backUntilLine", int(line_number))

def read_sensors():
    """Returns {"distance": cm, "line": raw_adc_value}."""
    resp = _call("readSensors")
    if resp.get("success") and isinstance(resp.get("result"), dict):
        return resp["result"]
    raw = resp.get("result", "")
    if isinstance(raw, (bytes, bytearray)):
        raw = raw.decode()
    if isinstance(raw, str) and "," in raw:
        parts = raw.split(",")
        try:
            return {"distance": int(parts[0]), "line": int(parts[1])}
        except ValueError:
            pass
    return {"distance": -1, "line": -1, "error": resp.get("error", "unknown")}

def read_distance():
    """Convenience: returns distance in cm only, or -1 on error."""
    return read_sensors().get("distance", -1)

def read_line():
    """Convenience: returns raw line sensor value only."""
    return read_sensors().get("line", -1)

# ─── OpenClaw / Telegram command handler ─────────────────────────────────────
def handle(command: str) -> str:
    cmd = command.lower().strip()
    _log(f"handle(\"{command}\")")

    # stop
    if "stop" in cmd:
        stop()
        return "Robot stopped."

    nums = re.findall(r'\d+\.?\d*', cmd)

    # back until line <N>
    if "back" in cmd and "line" in cmd:
        if not nums:
            return "Specify a line number, e.g. 'back until line 2'."
        lineNum = int(float(nums[0]))
        resp = back_until_line(lineNum)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        return f"Moved back until line {lineNum}."

    # forward until line <N>
    if "forward" in cmd and "line" in cmd:
        if not nums:
            return "Specify a line number, e.g. 'forward until line 2'."
        lineNum = int(float(nums[0]))
        resp = forward_until_line(lineNum)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        found = resp.get("result")
        if isinstance(found, int) and found < lineNum:
            return f"Stopped early: wall detected, found {found} of {lineNum} line(s)."
        return f"Moved forward until line {lineNum}."

    # forward until <cm>
    if "forward" in cmd and "until" in cmd:
        if not nums:
            return "Specify a target distance, e.g. 'forward until 20'."
        target = int(float(nums[0]))
        resp = forward_until(target)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        return f"Moved forward until distance <= {target} cm."

    # read sensors
    if any(w in cmd for w in ["sensor", "distance", "line", "read"]):
        s = read_sensors()
        dist = f"{s['distance']} cm" if s.get('distance', -1) >= 0 else "no signal"
        line = s.get('line', -1)
        return f"Distance: {dist} | Line sensor: {line}"

    # rotate/turn right|left N seconds (time-based)
    if "rotat" in cmd or "turn" in cmd:
        # Cambiado a float para preservar decimales
        seconds = float(nums[0]) if nums else 1.0
        if "right" in cmd:
            resp = turn_right(seconds)
            if not resp.get("success"):
                return f"Error: {resp.get('error')}"
            return f"Rotated right {seconds}s."
        if "left" in cmd:
            resp = turn_left(seconds)
            if not resp.get("success"):
                return f"Error: {resp.get('error')}"
            return f"Rotated left {seconds}s."
        return "Specify direction, e.g. 'rotate right 2 seconds'."

    # plain movement
    # Cambiado a float para preservar decimales
    seconds = float(nums[0]) if nums else 1.0
    if any(w in cmd for w in ["forward", "ahead"]):
        resp = forward(seconds)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        return f"Moved forward {seconds}s."
    if any(w in cmd for w in ["back", "reverse"]):
        resp = back(seconds)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        return f"Moved back {seconds}s."
    if "right" in cmd:
        resp = turn_right(seconds)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        return f"Turned right {seconds}s."
    if "left" in cmd:
        resp = turn_left(seconds)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        return f"Turned left {seconds}s."

    return (
        "Command not recognised. Try: 'move forward 2 seconds', "
        "'forward until 20', 'forward until line 2', 'back until line 1', "
        "'read sensors', 'stop'."
    )

# OpenClaw skill entry point
skill_handler = handle

# ─── CLI self-test ────────────────────────────────────────────────────────────
if __name__ == "__main__":
    import sys
    cmd = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "read sensors"
    print(handle(cmd))