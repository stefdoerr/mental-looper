#!/usr/bin/env python3
"""
Renders screenshot-mental-looper.png (and thumbnail-mental-looper.png) via headless Chrome, so it matches mod-ui's rendering
of the pedal: bundled fonts, the baked title and head, real CSS. Also the page tests/test_modgui_face.py drives.

Pipeline: resolve the Mustache tokens mod-ui fills at runtime, point every /resources/ URL at the local files, drop
the audio port loops (the jacks hang outside the frame), mirror mod-ui's forced `*` rules, inline the face script as
window.mlFace, then run a pose (start and change events, as mod-ui sends them) and screenshot.

Run (make screenshot):
    CHROME=google-chrome uv run --quiet --with playwright python3 plugins/MentalLooper/modgui/render_screenshot.py
"""

import os
import re
import shutil
import tempfile

HERE   = os.path.dirname(os.path.abspath(__file__))
ICON   = os.path.join(HERE, "icon-mental-looper.html")
STYLE  = os.path.join(HERE, "stylesheet-mental-looper.css")
SCRIPT = os.path.join(HERE, "script-mental-looper.js")
OUT    = os.path.join(HERE, "screenshot-mental-looper.png")
THUMB  = os.path.join(HERE, "thumbnail-mental-looper.png")

WIDTH, HEIGHT = 520, 420
SCALE = 2          # 2x: crisper on HiDPI screens and in the printed manual

SUBSTITUTIONS = {
    "{{{cns}}}": "",   # cache-bust class suffix; harmless to drop
    "{{{ns}}}":  "",   # cache-bust query string; not needed for local files
}


def local_resources(text: str) -> str:
    """mod-ui serves this folder as /resources/; point the URLs at the local files."""
    return (text.replace("url(/resources/", "url(file://" + HERE + "/")
                .replace('src="/resources/', 'src="file://' + HERE + "/"))


def render_template(html: str) -> str:
    # Drop HTML comments first: a comment may mention Mustache tokens.
    html = re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)
    for k, v in SUBSTITUTIONS.items():
        html = html.replace(k, v)
    for kind in ("audio", "midi", "cv"):
        for direction in ("input", "output"):
            html = re.sub(r"\{\{#effect\.ports\." + kind + r"\." + direction + r"\}\}.*?\{\{/effect\.ports\." + kind
                          + r"\." + direction + r"\}\}", "", html, flags=re.DOTALL)
    return local_resources(html)


def render_stylesheet(css: str) -> str:
    for k, v in SUBSTITUTIONS.items():
        css = css.replace(k, v)
    return local_resources(css)


def page_html(pose_js: str = "") -> str:
    """The face on its own page. window.mlFace is the face script; pose_js runs once the fonts are in."""
    icon = render_template(open(ICON, encoding="utf-8").read())
    css = render_stylesheet(open(STYLE, encoding="utf-8").read())
    script = open(SCRIPT, encoding="utf-8").read()
    return f"""<!doctype html>
<html><head><meta charset="utf-8">
<style>
  html, body {{ margin: 0; padding: 0; width: {WIDTH}px; height: {HEIGHT}px; overflow: hidden; background: transparent; }}
  /* mod-ui's main.css forces these on everything */
  * {{ box-sizing: border-box; font-family: "cooper hewitt", sans-serif !important; }}
  .pedal-host {{ position: relative; width: {WIDTH}px; height: {HEIGHT}px; }}
{css}
</style></head>
<body><div class="pedal-host">{icon}</div>
<script>
window.mlFace = ({script});
window.posed = document.fonts.ready.then(() => {{ {pose_js} }});
</script></body></html>"""


def launch(p):
    """Chromium from $CHROME (default google-chrome): Playwright's own builds may not match its version."""
    return p.chromium.launch(executable_path=shutil.which(os.environ.get("CHROME", "google-chrome")))


def load(browser, pose_js: str = "", scale: float = 1):
    """A page with the face loaded, posed and its fonts in."""
    tab = browser.new_context(viewport={"width": WIDTH, "height": HEIGHT}, device_scale_factor=scale).new_page()
    tmp = tempfile.NamedTemporaryFile("w", suffix=".html", delete=False, encoding="utf-8")
    tmp.write(page_html(pose_js))
    tmp.close()
    tab.goto("file://" + tmp.name)
    tab.wait_for_load_state("networkidle")
    tab.evaluate("() => window.posed")
    os.unlink(tmp.name)
    return tab


# The screenshot's pose, through the face script as mod-ui would drive it: screaming (looper 3), track 1, the bar at
# 40 %, the mockup's song playing step 1. Transitions jump to their end, animations hold their first frame.
POSE = """
const host = document.querySelector('.pedal-host');
const ev = o => mlFace(Object.assign({ icon: [host] }, o), { set_port_value() {} });
const ports = { track: 1, volume: 0, song: 1, part: 0, ':bypass': 0, on_1: 1, on_2: 1, on_3: 1, on_4: 1 };
const song = [[0, 4], [1, 2], [0, 4], [1, 4], [2, 2], [5, 1]];
for (let s = 1; s <= 16; s++) { const [p, r] = song[s - 1] || [4, 1]; ports['step' + s + '_part'] = p; ports['step' + s + '_repeats'] = r; }
ev({ type: 'start', ports: Object.entries(ports).map(([symbol, value]) => ({ symbol, value })) });
for (const [symbol, value] of [['looper', 3], ['progress_loops', 1], ['progress', .4], ['song_step', 1]])
    ev({ type: 'change', symbol, value });
document.body.offsetWidth;
document.getAnimations().forEach(a => a instanceof CSSTransition ? a.finish() : (a.pause(), a.currentTime = 0));
"""


def main():
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        browser = launch(p)
        load(browser, POSE, SCALE).screenshot(path=OUT, omit_background=True)
        load(browser, POSE, 0.5).screenshot(path=THUMB, omit_background=True)   # 260 px wide
        browser.close()
    print(f"Wrote {OUT} ({WIDTH * SCALE}x{HEIGHT * SCALE}) and {THUMB} ({WIDTH // 2}x{HEIGHT // 2})")


if __name__ == "__main__":
    main()
