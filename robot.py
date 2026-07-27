# Arduino UNO Q AI Agentic Robot MPU commands
# Roni Bandini July 2026 - MIT License @ronibandini
# These commands will be used by OpenClaw and they will reach the MCU through the bridge
# bridge.py must be running on the UNO Q and reachable at localhost:8080.

# Compass calibration (hard/soft-iron) runs automatically at boot in
# sketch.ino - watch its Serial output and spin the robot in place when it
# says to, before using "setup" or any "heading ..." command below.

# Usage from CLI for testing, it will be used by OpenClaw in a real scenario:
#   python3 robot.py "setup"                # captures front, left, right
#   python3 robot.py "read sensors"         # distance, line, compass
#   python3 robot.py "move forward 2 seconds"
#   python3 robot.py "move back 2 seconds"
#   python3 robot.py "rotate right 2 seconds"
#   python3 robot.py "rotate left 2 seconds"
#   python3 robot.py "forward until 20"     # distance from an obstacle, cm
#   python3 robot.py "forward until line 2" # stops early + reports count if a wall is hit first
#   python3 robot.py "back until line 2"
#   python3 robot.py "heading front"        # small rotations + compass checks, based on setup
#   python3 robot.py "heading left"         # same
#   python3 robot.py "heading right"        # same


import json
import re
import time
import urllib.request

BRIDGE_URL = "http://127.0.0.1:8080"
DEBUG = True

def _log(msg):
    if DEBUG:
        print(f"[robot.py] {msg}")

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


def forward(seconds=1.0):    return _call("move", "forward", float(seconds))
def back(seconds=1.0):       return _call("move", "back",    float(seconds))
def turn_left(seconds=1.0):  return _call("move", "left",    float(seconds))
def turn_right(seconds=1.0): return _call("move", "right",   float(seconds))
def stop():                  return _call("move", "stop",    0.0)

def forward_until(target_cm):
    return _call("forwardUntil", int(target_cm))

def forward_until_line(line_number):
    return _call("forwardUntilLine", int(line_number))

def back_until_line(line_number):
    # Not part of the trimmed CLI command set below, but the MCU still
    # exposes backUntilLine, so this is here for direct/programmatic use.
    return _call("backUntilLine", int(line_number))

def read_sensors():
    """Returns {"distance": cm, "line": raw value, "heading": degrees}."""
    resp = _call("readSensors")
    if resp.get("success") and isinstance(resp.get("result"), dict):
        return resp["result"]
    raw = resp.get("result", "")
    if isinstance(raw, (bytes, bytearray)):
        raw = raw.decode()
    if isinstance(raw, str) and "," in raw:
        parts = raw.split(",")
        try:
            return {
                "distance": int(parts[0]),
                "line":     int(parts[1]),
                "heading":  float(parts[2])
            }
        except (ValueError, IndexError):
            pass
    return {"distance": -1, "line": -1, "heading": None, "error": resp.get("error", "unknown")}

def read_distance():
    return read_sensors().get("distance", -1)

def read_line():
    return read_sensors().get("line", -1)

def read_heading():
    """Current compass heading in degrees (as reported by readSensors, so
    it reflects sketch.ino's boot-time calibration)."""
    return read_sensors().get("heading")

def setup_calibration():
    steps = [
        ("front", "Point the robot FRONT (the direction you want as 0/home)"),
        ("left",  "Rotate the robot 90 degrees LEFT from front"),
        ("right", "Rotate the robot 90 degrees RIGHT from front"),
    ]
    readings = {}
    for key, prompt in steps:
        input(f"{prompt}, then press Enter...")
        heading = read_heading()
        if heading is None:
            return {"success": False, "error": f"could not read compass for '{key}'"}
        readings[key] = heading
        _log(f"  {key} = {heading}°")

    resp = _call("setCalibration", readings["front"], readings["left"], readings["right"])
    if not resp.get("success"):
        return resp
    return {"success": True, **readings}

def _log_turn_result(resp):
    result = resp.get("result")
    if not isinstance(result, dict):
        return
    if not result.get("calibrated", True):
        _log("  ! not set up yet - run setup_calibration() / \"setup\" first")
        return
    _log(f"  heading: start={result.get('start')}° -> final={result.get('final')}° "
         f"(target={result.get('target')}°, pulses={result.get('pulses')}, "
         f"reached={result.get('reached')})")

def heading_to(direction):
    direction = direction.lower().strip()
    method = {"front": "headingFront", "left": "turnLeft90", "right": "turnRight90"}.get(direction)
    if method is None:
        return {"success": False, "error": f"invalid direction: {direction}"}
    resp = _call(method)
    _log_turn_result(resp)
    return resp


def handle(command: str) -> str:
    cmd = command.lower().strip()
    _log(f"handle(\"{command}\")")


    if "stop" in cmd:
        stop()
        return "Robot stopped."


    if "setup" in cmd:
        result = setup_calibration()
        if not result.get("success"):
            return f"Error: {result.get('error')}"
        return (f"Setup complete. Front: {result['front']}°, "
                f"left: {result['left']}°, right: {result['right']}°.")


    if "heading" in cmd:
        if "front" in cmd:
            direction = "front"
        elif "left" in cmd:
            direction = "left"
        elif "right" in cmd:
            direction = "right"
        else:
            return "Specify a direction, e.g. 'heading front', 'heading left', 'heading right'."
        resp = heading_to(direction)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        result = resp.get("result")
        if isinstance(result, dict):
            if not result.get("calibrated", True):
                return "Not set up yet - run 'setup' first."
            if result.get("reached"):
                return f"Heading {direction} (heading {result.get('final')}°)."
            return (f"Timed out after {result.get('pulses')} pulses: "
                    f"heading went {result.get('start')}° -> {result.get('final')}° "
                    f"(target {result.get('target')}°).")
        return f"Heading {direction}." if result else "Timed out reaching target heading."

    nums = re.findall(r'\d+\.?\d*', cmd)

    # back until linea <N>
    if "back" in cmd and "line" in cmd:
        if not nums:
            return "Specify a line number, e.g. 'back until line 2'."
        lineNum = int(float(nums[0]))
        resp = back_until_line(lineNum)
        if not resp.get("success"):
            return f"Error: {resp.get('error')}"
        return f"Moved back until line {lineNum}."

    # forward until linea <N>
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
        heading = s.get('heading')
        heading_str = f"{heading}°" if heading is not None else "n/a"
        return f"Distance: {dist} | Line sensor: {line} | Compass: {heading_str}"

    # rotate left/right N seconds  
    if "rotat" in cmd:
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

    # move forward/back N seconds
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

    return (
        "Command not recognised. Try: 'setup', 'read sensors', "
        "'move forward 2 seconds', 'rotate right 2 seconds', "
        "'forward until 20', 'forward until line 2', "
        "'heading front', 'heading left', 'heading right', 'stop'."
    )

# entry point
skill_handler = handle


if __name__ == "__main__":
    import sys
    cmd = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "read sensors"
    print(handle(cmd))
