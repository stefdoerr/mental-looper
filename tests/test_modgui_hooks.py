#!/usr/bin/env python3
"""
Regression guard for the MOD pedal face (plugins/Ouroboros/modgui/), in the style of ~/Fun/boreas's.

mod-ui and script-ouroboros.js find every control by its mod-role / mod-port-symbol / data-* attributes, and mod-ui's
own CSS has traps a restyle walks into (forced fonts, stylesheets that outlive their plugin version, a z-index-20
footswitch, a shadow on the first child div). This pins all of that, plus the baked head and the static defaults the
face shows before its script runs.

Run: python3 tests/test_modgui_hooks.py   (part of `make test-modgui`)
"""

import os
import re
import sys

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGIN = os.path.join(ROOT, "plugins", "Ouroboros")
MODGUI = os.path.join(PLUGIN, "modgui")
read   = lambda f: open(os.path.join(MODGUI, f), encoding="utf-8").read()
html   = read("icon-ouroboros.html")
js     = read("script-ouroboros.js")
ttl    = open(os.path.join(PLUGIN, "modgui.ttl"), encoding="utf-8").read()
# The stylesheet is a Mustache template: swap {{{cns}}}/{{{ns}}} out so their braces don't confuse the rule parser.
css    = re.sub(r"/\*.*?\*/", "", read("stylesheet-ouroboros.css"), flags=re.DOTALL) \
           .replace("{{{cns}}}", "__CNS__").replace("{{{ns}}}", "")
SCOPE  = ".ml-pedal__CNS__"

KEY_PORTS = ["onoff", "stop_play", "undo", "redo", "clear_track", "clear_all"]
ROLES     = {"drag-handle", "bypass", "bypass-light", "input-audio-port", "output-audio-port"}
OUTPUTS   = {"progress", "progress_loops", "looper", "take_secs", "song_step"}


def tags(attr_re):
    return [t for t in re.findall(r"<[a-zA-Z][^>]*>", html) if re.search(attr_re, t)]


def attr(tag, name):
    m = re.search(r"\b" + name + r'="([^"]*)"', tag)
    return m.group(1) if m else None


def classes(tag):
    return (attr(tag, "class") or "").split()


def css_rules():
    """(selector, body) of every plain rule (inside @media too; @font-face/@keyframes skipped)."""
    flat = re.sub(r"@keyframes[^{]*\{(?:[^{}]*\{[^{}]*\})*[^{}]*\}", "", css)
    return [(sel.strip(), body) for sel, body in re.findall(r"([^{}]+)\{([^{}]*)\}", flat)
            if not sel.strip().startswith("@")]


def button(attr_name, value):
    m = re.search(r'(<button[^>]*\b' + attr_name + '="' + value + r'"[^>]*>)(.*?)</button>', html, re.DOTALL)
    assert m, f"no button with {attr_name}={value}"
    return m.group(1), m.group(2)


def test_knob_ports():
    ports = {attr(t, "mod-port-symbol") for t in tags(r'mod-role="input-control-port"')}
    assert ports == {"volume"}, ports


def test_roles():
    roles = {attr(t, "mod-role") for t in tags(r"mod-role=")}
    assert ROLES <= roles, f"missing roles {ROLES - roles}"


def test_keys_carry_their_ports():
    keys = [attr(t, "data-port") for t in tags(r'class="[^"]*\bml-key\b') if attr(t, "data-port")]
    assert keys == KEY_PORTS, keys


def test_root_scoped_and_no_shadow():
    # pedals.css: #pedalboard-dashboard > .mod-pedal > div:first-child:not(.mod-no-shadow); mod-ui's own wrapper is
    # the .mod-pedal there, so its first child is OUR root.
    root = re.search(r'<div class="([^"]*)"', html).group(1).split()
    assert "ml-pedal{{{cns}}}" in root and "ml-pedal" not in root, root
    assert "mod-no-shadow" in root, f"root div needs mod-no-shadow: {root}"


def test_no_old_class_names():
    used = set(" ".join(re.findall(r'class="([^"]*)"', html)).split())
    assert not [c for c in used if c.startswith("ouroboros")], used


def test_no_ids():
    assert not re.search(r"\sid=", re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)), "two pedals on a page would share an id"


def test_styles_scoped_to_this_plugin_version():
    for sel, _ in css_rules():
        for part in sel.split(","):
            assert part.strip().startswith(SCOPE), f"unscoped selector: {part.strip()!r}"
    for name in re.findall(r"@(?:keyframes|property)\s+([\w-]+)", css):
        assert name.startswith("ml") and "__CNS__" in name, f"global name must be ml-prefixed with {{{{{{cns}}}}}}: {name}"


def test_fonts_beat_modui_important():
    decls = [(sel, d) for sel, body in css_rules() for d in re.findall(r"font-family:[^;]*", body)]
    assert decls, "no font-family declarations"
    for sel, d in decls:
        assert d.rstrip().endswith("!important"), f"{sel}: {d}"
        assert "MentalLooper" in d, f"{sel}: bundled family names only: {d}"


def test_footswitch_z_index_pinned():
    zs = [body for sel, body in css_rules() if ".mod-footswitch" in sel and "z-index" in body]
    assert zs and "!important" in zs[-1], "set .mod-footswitch z-index with !important"


def test_animations_compositor_only():
    blocks = re.findall(r"@keyframes\s+[\w-]+\s*\{((?:[^{}]*\{[^{}]*\})*[^{}]*)\}", css)
    assert blocks, "no keyframes found"
    for b in blocks:
        props = set(re.findall(r"([\w-]+)\s*:", re.sub(r"[\d.,%\s]+\{", "{", b)))
        assert props <= {"transform", "opacity"}, f"animates {props - {'transform', 'opacity'}}"


def test_knob_image_has_strip():
    # Without a background-image strip mod-ui's film widget never initialises: the knob can't be dragged.
    bodies = [body for sel, body in css_rules() if sel.endswith(".mod-knob-image")]
    assert any(re.search(r"background-image:\s*url\(/resources/knobs/clear\.png", b) for b in bodies)


def test_head_baked():
    m = re.search(r"<!-- ml-head:begin -->(.*?)<!-- ml-head:end -->", html, re.DOTALL)
    assert m, "head markers missing"
    head = m.group(1)
    assert '<svg class="ml-head"' in head
    for g in ("ml-m-grit", "ml-m-wait", "ml-m-scream", "ml-m-dead"):
        assert re.search(r'class="ml-mouth ' + g + '"', head), g
    assert len(re.findall(r'class="ml-spin"', head)) == 2
    assert "clipPath" not in head and "<text" in head


def test_monitored_outputs():
    m = re.search(r"modgui:monitoredOutputs((?:\s*\[\s*lv2:symbol\s*\"[^\"]+\"\s*\]\s*,?)+)", ttl)
    assert m, "no modgui:monitoredOutputs"
    assert set(re.findall(r'"([^"]+)"', m.group(1))) == OUTPUTS


def test_static_defaults():
    # What the face shows before its script runs (a fresh pedal, mod-ui's drag-in preview): nothing recorded, an empty
    # bar, track 1, part A, 0 dB, and the default song (step 1 A x1, then Loop).
    root = re.search(r'<div class="([^"]*)"', html).group(1).split()
    assert "ml-st-0" in root, root
    assert "sel" in classes(button("data-track", "1")[0]) and "sel" not in classes(button("data-track", "2")[0])
    assert "sel" in classes(button("data-part", "0")[0])
    tag, body = button("data-step", "1")
    assert re.sub(r"\s", "", body) == "<b>A</b><small>×1</small>" and "end" not in classes(tag), body
    tag, body = button("data-step", "2")
    assert "<b>↺</b>" in body and "end" in classes(tag) and "dead" not in classes(tag)
    for s in range(3, 17):
        assert "dead" in classes(button("data-step", str(s))[0]), s
    knob = tags(r'class="[^"]*\bml-knob\b')[0]
    assert re.search(r"--v:\s*0\.9091", attr(knob, "style") or ""), knob
    assert re.search(r'class="ml-db">0\.0 dB<', html)
    prog = tags(r'class="[^"]*\bml-prog\b')[0]
    assert re.search(r"--p:\s*0\b", attr(prog, "style") or ""), prog



if __name__ == "__main__":
    failed = 0
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
                print("ok  ", name)
            except AssertionError as e:
                failed += 1
                print("FAIL", name, e)
    sys.exit(1 if failed else 0)
