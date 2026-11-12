#!/usr/bin/env python3
# Closed-loop setter for the Parkside PHGA 1300 A1 pump clock/timer via HA.
# Reads the decoded screen, then presses (firmware author's rule).
#
# DETERMINISTIC STATE MACHINE (mapped live 2026-11-12):
#   Resting sub-screens cycle on SAVE:  Time Now -> Time On -> Time Off -> ...
#     labels Save/Set/Exit:  SET = enter edit,  SAVE = advance sub-screen,
#                            right/Exit = leave Time Mode
#   Edit (labels Next/+/-):  SET = + (increment active field),
#                            MODE = - (decrement),  SAVE = Next field.
#     Field order: HOURS first (blinks), then MINUTES, then a final SAVE that
#     CONFIRMS and lands on the NEXT sub-screen's resting view.
#   The active (editing) field blinks -> reads blank intermittently.
#   "Time Mode / NOT SET" (post power cut): SET activates and reveals Time Now.
#   The clock persists across Standby trips.
#
# Verify is a SEPARATE pass: re-enter Time Mode, walk to the sub-screen, read
# its resting value. This avoids all blink/transition races.
#
# Usage: pump_timer_set.py {now|on|off} HH:MM
import sys, os, time, json, urllib.request, re
DEBUG = os.environ.get("PUMP_DEBUG")
def dbg(*a):
    if DEBUG: print("  [dbg]", *a, file=sys.stderr, flush=True)

HA = os.environ.get("HA_URL", "").rstrip("/")
TOK = os.environ.get("HA_TOKEN", "")
if not HA or not TOK:
    # Running inside the HA core container (shell_command): use the supervisor
    # proxy and the injected SUPERVISOR_TOKEN.
    HA = "http://supervisor/core"
    TOK = os.environ.get("SUPERVISOR_TOKEN", TOK)
H = {"Authorization": f"Bearer {TOK}", "Content-Type": "application/json",
     "User-Agent": "Mozilla/5.0 pump-timer-set"}
ORDER = ["Time Now", "Time On", "Time Off"]

def api(path, data=None, tries=3):
    last=None
    for _ in range(tries):
        try:
            req = urllib.request.Request(f"{HA}/api/{path}",
                data=json.dumps(data).encode() if data is not None else None,
                headers=H, method="POST" if data is not None else "GET")
            with urllib.request.urlopen(req, timeout=8) as r:
                return json.loads(r.read().decode())
        except Exception as e:
            last=e; time.sleep(0.5)
    raise last if last else RuntimeError("api failed")

def l1(): return api("states/sensor.water_pump_display_line_1")["state"]
def l2(): return api("states/sensor.water_pump_display_line_2")["state"]
def labels():
    a = api("states/sensor.water_pump_key_labels")["attributes"]
    return ((a.get("left") or "").strip(), (a.get("middle") or "").strip(), (a.get("right") or "").strip())
def is_resting(): return labels()[2].lower()=="exit"
def is_edit():    return labels()[1]=="+"

def press(key, settle=1.3):
    api("services/button/press", {"entity_id": f"button.water_pump_press_{key}"})
    time.sleep(settle)

def set_mode(target):
    api("services/script/water_pump_set_mode", {"target": target}); time.sleep(5)

SCREEN_RE = re.compile(r"(Time Now|Time On|Time Off)\s+(\d{0,2}):(\d{0,2})")

def read_field(samples=7, gap=0.3):
    """Beat the blink: most-common non-blank hh/mm plus screen name."""
    names=[]; hhs=[]; mms=[]
    for _ in range(samples):
        m = SCREEN_RE.search(l1())
        if m:
            names.append(m.group(1))
            if m.group(2)!="": hhs.append(int(m.group(2)))
            if m.group(3)!="": mms.append(int(m.group(3)))
        time.sleep(gap)
    mode=lambda xs: max(set(xs),key=xs.count) if xs else None
    return mode(names), mode(hhs), mode(mms)

def enter_time_mode():
    set_mode("Time Mode")
    # NOT SET: press SET until a Time X HH:MM screen shows
    for _ in range(6):
        if SCREEN_RE.search(l1()): return True
        dbg("activating (NOT SET) via SET")
        press("set")
    return bool(SCREEN_RE.search(l1()))

def goto_resting(target):
    """Land on target sub-screen in RESTING state (Save/Set/Exit). After each
    SAVE the panel briefly shows the transitional 'Time Mode' header before the
    next sub-screen settles, so read patiently."""
    for _ in range(8):
        # wait for a settled Time X screen (not the transitional header)
        name=None
        for _ in range(12):
            m=SCREEN_RE.search(l1())
            if m: name=m.group(1); break
            time.sleep(0.4)
        dbg("goto", name, "want", target, "resting", is_resting())
        if name==target and is_resting(): return True
        press("save")
    return False

def nudge(cur, target, mod):
    up=(target-cur)%mod; down=(cur-target)%mod
    dbg(f"nudge {cur}->{target} (+{up}/-{down})")
    if up==0: return
    if up<=down:
        for _ in range(up): press("set", settle=0.8)
    else:
        for _ in range(down): press("mode", settle=0.8)

def do_set(target, hh, mm):
    if not enter_time_mode(): return "no-time-mode"
    if not goto_resting(target): return "no-subscreen"
    press("set")                        # enter edit (hours blink)
    if not is_edit(): return "no-edit"
    _, chh, _ = read_field()
    if chh is None: return "no-hours"
    nudge(chh, hh, 24)
    press("save")                       # -> minutes
    _, _, cmm = read_field()
    if cmm is None: return "no-minutes"
    nudge(cmm, mm, 60)
    press("save")                       # confirm (lands on next sub-screen)
    return "ok"

def verify_inline(target, hh, mm):
    """We are still inside Time Mode after do_set (on the next sub-screen,
    resting). Cycle sub-screens with SAVE (non-destructive) back to target and
    read its resting value. Never call set_mode here - that scrambles the menu."""
    if not goto_resting(target): return None
    _, vhh, vmm = read_field(samples=8)
    return (vhh, vmm)

def exit_time_mode():
    """From a resting Time sub-screen, the right key is Exit. Press it to leave
    the menu, then force a clean Standby."""
    for _ in range(3):
        if is_resting():
            press("mode")   # right == Exit on resting screens
            break
        press("save")
    time.sleep(1)
    set_mode("Standby")

def main():
    which=sys.argv[1]
    arg=sys.argv[2]
    if arg.upper()=="NOW":
        import datetime
        try:
            from zoneinfo import ZoneInfo
            n=datetime.datetime.now(ZoneInfo("Europe/Berlin"))
        except Exception:
            n=datetime.datetime.now()
        hh,mm=n.hour,n.minute
    else:
        hh,mm=map(int, arg.split(":"))
    target={"now":"Time Now","on":"Time On","off":"Time Off"}[which]
    st = do_set(target, hh, mm)
    dbg("set stage:", st)
    if st!="ok":
        print(f"FAIL {target}: {st}"); exit_time_mode(); sys.exit(1)
    got = verify_inline(target, hh, mm)
    ok = got==(hh,mm)
    print(f"RESULT {target}: wanted {hh:02d}:{mm:02d} got {got} -> {'OK' if ok else 'MISMATCH'}")
    exit_time_mode()
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
