#!/usr/bin/env python3
"""
The face script (plugins/Ouroboros/modgui/script-ouroboros.js) in a real browser, against the real template and
stylesheet: start and change events shaped as mod-ui sends them, pointer and wheel input from Playwright, port writes
recorded from funcs.set_port_value.

Run: make test-modgui   (CHROME=google-chrome uv run --quiet --with playwright python3 tests/test_modgui_face.py)
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "plugins", "Ouroboros", "modgui"))
import render_screenshot as rs  # noqa: E402

# The plugin's defaults for every input the face sees at start (mod-ui sends them all, :bypass included).
DEFAULTS = {"rec": 0, "stop_clear": 0, "track": 1, "onoff": 0, "volume": 0, "undo": 0, "redo": 0, "clear_track": 0,
            "clear_all": 0, "on_1": 1, "on_2": 1, "on_3": 1, "on_4": 1, "stop_play": 0, "part": 0, "song": 0,
            ":bypass": 0}
for s in range(1, 17):
    DEFAULTS["step%d_part" % s] = 0 if s == 1 else 4
    DEFAULTS["step%d_repeats" % s] = 1

HARNESS = """
window.writes = [];
const host = document.querySelector('.pedal-host');
window.funcs = { set_port_value: (s, v) => writes.push([s, v, performance.now()]) };
window.ev = o => mlFace(Object.assign({ icon: [host] }, o), funcs);
window.chg = (symbol, value) => ev({ type: 'change', symbol, value });
window.start = over => ev({ type: 'start', ports: Object.entries(Object.assign({}, %s, over || {}))
    .map(([symbol, value]) => ({ symbol, value })) });
"""

BROWSER = None


def face(**over):
    """A fresh page with the face started; over: start-port values (`bypass` for :bypass)."""
    if "bypass" in over:
        over[":bypass"] = over.pop("bypass")
    import json
    tab = rs.load(BROWSER, HARNESS % json.dumps(DEFAULTS) + "start(%s);" % json.dumps(over))
    return tab


def chg(t, sym, v):
    t.evaluate("([s, v]) => chg(s, v)", [sym, v])


def writes(t):
    return t.evaluate("() => writes")


def classes(t, sel):
    return t.evaluate("s => [...document.querySelector(s).classList]", sel)


def has(t, sel, c):
    return c in classes(t, sel)


def centre(t, sel):
    b = t.locator(sel).first.bounding_box()
    return b["x"] + b["width"] / 2, b["y"] + b["height"] / 2


def click(t, sel):
    x, y = centre(t, sel)
    t.mouse.click(x, y)


def shown(t, sel):
    return t.evaluate("s => getComputedStyle(document.querySelector(s)).display !== 'none'", sel)


def spin(t, k):
    return t.evaluate("k => document.querySelectorAll('.ml-spin')[k].getAttribute('transform') || ''", k)


def angle(t, k):
    import re
    m = re.search(r"rotate\((-?[\d.]+)", spin(t, k))
    return float(m.group(1)) % 360 if m else 0.0


ROOT_SEL = ".ml-pedal"


# ---------------------------------------------------------------------------------------------------------------------

def test_looper_states():
    t = face()
    for n in range(6):
        chg(t, "looper", n)
        st = [c for c in classes(t, ROOT_SEL) if c.startswith("ml-st-")]
        assert st == ["ml-st-%d" % n], st


def test_bypass():
    t = face()
    chg(t, ":bypass", 1)
    assert has(t, ROOT_SEL, "ml-off")
    chg(t, ":bypass", 0)
    assert not has(t, ROOT_SEL, "ml-off")
    assert has(face(bypass=1), ROOT_SEL, "ml-off")


def test_bypass_wins_over_state():                      # Review Focus 1
    t = face()
    chg(t, "looper", 3)
    chg(t, ":bypass", 1)
    assert shown(t, ".ml-m-dead") and not shown(t, ".ml-m-scream")
    n = t.evaluate("() => [...document.querySelectorAll('.ml-hd, .ml-mouth, .ml-free')]"
                   ".reduce((n, e) => n + e.getAnimations().length, 0)")
    assert n == 0, n


def test_mouth_is_a_fixed_press():
    t = face()
    x, y = centre(t, ".ml-rec")
    t.mouse.move(x, y)
    t.mouse.down()
    assert has(t, ROOT_SEL, "ml-rec-on")
    t.wait_for_timeout(600)
    w = writes(t)
    assert [(s, v) for s, v, _ in w] == [("rec", 1), ("rec", 0)], w
    assert 80 <= w[1][2] - w[0][2] <= 200, w[1][2] - w[0][2]
    assert not has(t, ROOT_SEL, "ml-rec-on")
    t.mouse.up()
    t.wait_for_timeout(150)
    assert len(writes(t)) == 2, writes(t)


def test_keys_momentary_min_hold():
    t = face()
    x, y = centre(t, '.ml-key[data-port="undo"]')
    t.mouse.move(x, y)
    t.mouse.down()
    t.mouse.up()
    t.wait_for_timeout(250)
    w = writes(t)
    assert [(s, v) for s, v, _ in w] == [("undo", 1), ("undo", 0)], w
    assert w[1][2] - w[0][2] >= 89, w[1][2] - w[0][2]
    t.evaluate("() => writes.length = 0")
    t.mouse.down()
    t.wait_for_timeout(300)
    assert [(s, v) for s, v, _ in writes(t)] == [("undo", 1)]
    assert has(t, '.ml-key[data-port="undo"]', "on")
    t.mouse.up()
    t.wait_for_timeout(30)
    assert [(s, v) for s, v, _ in writes(t)] == [("undo", 1), ("undo", 0)]
    assert not has(t, '.ml-key[data-port="undo"]', "on")


def test_key_released_on_leave():                        # Review Focus 3
    t = face()
    x, y = centre(t, '.ml-key[data-port="clear_track"]')
    t.mouse.move(x, y)
    t.mouse.down()
    t.mouse.move(x, y + 200)                               # off the key (and the pedal)
    t.wait_for_timeout(200)
    assert [(s, v) for s, v, _ in writes(t)] == [("clear_track", 1), ("clear_track", 0)], writes(t)
    assert not has(t, '.ml-key[data-port="clear_track"]', "on")
    t.mouse.up()


def test_track_click_writes_track():
    t = face()
    click(t, '.ml-trk[data-track="3"]')
    assert [(s, v) for s, v, _ in writes(t)] == [("track", 3)]
    chg(t, "track", 2)
    assert has(t, '.ml-trk[data-track="2"]', "sel") and not has(t, '.ml-trk[data-track="3"]', "sel")
    assert t.inner_text(".ml-tn") == "2"
    chg(t, "on_2", 0)
    assert has(t, '.ml-trk[data-track="2"]', "off")


def test_part_and_song():
    t = face()
    click(t, '.ml-pt[data-part="2"]')
    click(t, ".ml-song")
    assert [(s, v) for s, v, _ in writes(t)] == [("part", 2), ("song", 1)]
    chg(t, "song", 1)
    assert has(t, ".ml-song", "lit")
    chg(t, "part", 1)
    assert has(t, '.ml-pt[data-part="1"]', "sel") and not has(t, '.ml-pt[data-part="0"]', "sel")


def test_step_click_cycles_part():
    t = face(step5_part=2)
    click(t, '.ml-step[data-step="5"]')
    t.wait_for_timeout(50)
    assert [(s, v) for s, v, _ in writes(t)] == [("step5_part", 3)], writes(t)
    t = face(step5_part=5)
    click(t, '.ml-step[data-step="5"]')
    t.wait_for_timeout(50)
    assert [(s, v) for s, v, _ in writes(t)] == [("step5_part", 0)], writes(t)


def test_step_wheel_one_per_notch():
    t = face(step1_repeats=4)
    x, y = centre(t, '.ml-step[data-step="1"]')
    t.mouse.move(x, y)
    for _ in range(3):
        t.mouse.wheel(0, -33)
    t.wait_for_timeout(300)
    assert [(s, v) for s, v, _ in writes(t)] == [("step1_repeats", 5)], writes(t)
    t.mouse.wheel(0, -120)
    t.wait_for_timeout(50)
    assert [(s, v) for s, v, _ in writes(t)] == [("step1_repeats", 5), ("step1_repeats", 6)], writes(t)
    assert "×6" in t.inner_text('.ml-step[data-step="1"]')


def test_step_repeats_clamp():                           # Review Focus 4
    t = face(step1_repeats=16, step2_part=1, step2_repeats=1)
    for sel, dy in (('.ml-step[data-step="1"]', -120), ('.ml-step[data-step="2"]', 120)):
        x, y = centre(t, sel)
        t.mouse.move(x, y)
        t.mouse.wheel(0, dy)
        t.wait_for_timeout(300)
    assert writes(t) == [], writes(t)
    x, y = centre(t, '.ml-step[data-step="3"]')            # a Loop step: no repeats
    t.mouse.move(x, y)
    t.mouse.wheel(0, -120)
    t.wait_for_timeout(300)
    t.mouse.down()
    t.mouse.move(x, y - 30)
    t.mouse.up()
    t.wait_for_timeout(50)
    assert writes(t) == [], writes(t)


# The user's mod-ui fork (pedalboard.js, wheelClaimedByWidget) zooms the board on a legacy `mousewheel` listener in
# the capture phase and stops the event, unless it is over a knob: without a claim from the face, a scroll on a step
# would zoom the board instead.
BOARD_ZOOM = """document.body.addEventListener('mousewheel', e => { window.zoomed = (window.zoomed || 0) + 1;
    e.stopPropagation(); e.preventDefault(); }, { capture: true, passive: false });"""


def test_step_wheel_beats_board_zoom():
    t = face(step1_repeats=4)
    t.evaluate("() => { %s }" % BOARD_ZOOM)
    x, y = centre(t, '.ml-step[data-step="1"]')
    t.mouse.move(x, y)
    t.mouse.wheel(0, -120)
    t.wait_for_timeout(100)
    assert [(s, v) for s, v, _ in writes(t)] == [("step1_repeats", 5)], writes(t)
    assert not t.evaluate("() => window.zoomed"), "the board zoomed"


def test_zoom_gesture_sliding_onto_a_step_keeps_zooming():
    t = face(step1_repeats=4)
    t.evaluate("() => { %s }" % BOARD_ZOOM)
    x, y = centre(t, ".ml-title")
    t.mouse.move(x, y)
    t.mouse.wheel(0, -120)                                   # a zoom starts off the steps...
    x, y = centre(t, '.ml-step[data-step="1"]')
    t.mouse.move(x, y)
    t.mouse.wheel(0, -120)                                   # ...and slides onto one within 250 ms
    t.wait_for_timeout(100)
    assert writes(t) == [] and t.evaluate("() => window.zoomed") == 2, (writes(t), t.evaluate("() => window.zoomed"))
    t.wait_for_timeout(300)
    t.mouse.wheel(0, -120)                                   # after a pause the step takes the wheel
    t.wait_for_timeout(100)
    assert [(s, v) for s, v, _ in writes(t)] == [("step1_repeats", 5)], writes(t)


def test_step_drag():
    t = face(step1_repeats=4)
    x, y = centre(t, '.ml-step[data-step="1"]')
    t.mouse.move(x, y)
    t.mouse.down()
    t.mouse.move(x, y - 9)
    t.mouse.move(x, y - 17)
    t.mouse.up()
    t.wait_for_timeout(50)
    w = [(s, v) for s, v, _ in writes(t)]
    assert w and w[-1] == ("step1_repeats", 6) and not [s for s, _ in w if s == "step1_part"], w


def test_steps_follow_ports():
    t = face(step2_part=1)
    chg(t, "step3_part", 4)
    for s in range(4, 17):
        assert has(t, '.ml-step[data-step="%d"]' % s, "dead"), s
    assert has(t, '.ml-step[data-step="3"]', "end") and not has(t, '.ml-step[data-step="3"]', "dead")
    assert "↺" in t.inner_text('.ml-step[data-step="3"]')
    assert not has(t, '.ml-step[data-step="2"]', "dead")
    chg(t, "song_step", 2)
    assert has(t, '.ml-step[data-step="2"]', "now")
    chg(t, "song_step", 0)
    assert not has(t, '.ml-step[data-step="2"]', "now")


def test_progress_bar():
    t = face()
    chg(t, "progress", .5)
    chg(t, "progress_loops", 4)
    assert t.evaluate("() => parseFloat(document.querySelector('.ml-prog').style.getPropertyValue('--p'))") == .5
    lefts = t.evaluate("() => [...document.querySelectorAll('.ml-prog-ticks i')].map(i => parseFloat(i.style.left))")
    assert lefts == [25, 50, 75], lefts
    chg(t, "looper", 4)
    chg(t, "take_secs", 67)
    assert has(t, ".ml-prog", "ml-first") and t.inner_text(".ml-prog-time") == "1:07"
    chg(t, "looper", 2)
    assert not has(t, ".ml-prog", "ml-first")


def test_ticks_follow_track_switch():                    # Review Focus 2
    t = face()
    chg(t, "progress_loops", 4)
    chg(t, "progress_loops", 1)
    assert t.evaluate("() => document.querySelectorAll('.ml-prog-ticks i').length") == 0


def test_eyes_follow_loop():
    t = face()
    chg(t, "looper", 2)
    chg(t, "progress_loops", 1)
    chg(t, "progress", .25)
    assert abs(angle(t, 0) - 90) < .2 and abs(angle(t, 1) - 180) < .2, (spin(t, 0), spin(t, 1))
    chg(t, "looper", 1)
    chg(t, "progress", 0)
    assert abs(angle(t, 0) - 90) < .2, spin(t, 0)
    chg(t, "looper", 2)
    chg(t, "progress_loops", 4)
    chg(t, "progress", .3)
    assert abs(angle(t, 0) - 72) < .2, spin(t, 0)


def test_eyes_hold_on_stop_whatever_the_order():
    # mod-host sends a batch of postponed events newest first: the stop's progress = 0 can come before looper = 1.
    # An Auto Rec arm while stopped (looper 5) holds them too: "as for the state it waits in".
    t = face()
    chg(t, "looper", 2)
    chg(t, "progress_loops", 1)
    chg(t, "progress", .25)
    chg(t, "progress", 0)
    chg(t, "looper", 1)
    assert abs(angle(t, 0) - 90) < .2, spin(t, 0)
    chg(t, "looper", 5)
    chg(t, "progress", 0)
    assert abs(angle(t, 0) - 90) < .2, spin(t, 0)


def test_chin_and_stop_light_follow_ports():
    t = face()
    chg(t, "rec", 1)
    chg(t, "stop_clear", 1)
    assert has(t, ROOT_SEL, "ml-rec-on") and has(t, '.ml-key[data-port="stop_play"]', "on")
    chg(t, "rec", 0)
    chg(t, "stop_clear", 0)
    assert not has(t, ROOT_SEL, "ml-rec-on") and not has(t, '.ml-key[data-port="stop_play"]', "on")


def test_volume_text_and_knob():
    t = face()
    chg(t, "volume", -6)
    assert t.inner_text(".ml-db") == "−6.0 dB"
    v = t.evaluate("() => parseFloat(document.querySelector('.ml-knob').style.getPropertyValue('--v'))")
    assert abs(v - 54 / 66) < 1e-3, v
    chg(t, "volume", 3)
    assert t.inner_text(".ml-db") == "+3.0 dB"


if __name__ == "__main__":
    from playwright.sync_api import sync_playwright
    failed = 0
    with sync_playwright() as p:
        BROWSER = rs.launch(p)
        for name, fn in list(globals().items()):
            if name.startswith("test_"):
                try:
                    fn()
                    print("ok  ", name)
                except Exception as e:  # an assertion, or the script throwing in the page
                    failed += 1
                    print("FAIL", name, type(e).__name__, str(e)[:300])
        BROWSER.close()
    sys.exit(1 if failed else 0)
