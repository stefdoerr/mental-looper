/*
 * Mental Looper — the MOD face's script. Design: docs/superpowers/specs/2026-11-25-mental-looper-face-design.md.
 *
 * It animates nothing itself: every movement is CSS (stylesheet-ouroboros.css). It only
 *   - follows ports and outputs: state classes on the root (ml-st-0..5 from `looper`, ml-off when bypassed, ml-rec-on
 *     while `rec` is on), the eyes' angle (one turn per loop, from progress x progress_loops), the bar (--p, a tick
 *     per loop, the first take's time), the selected track, part, song and steps, the knob's --v and dB;
 *   - writes ports through funcs.set_port_value: the keys (1 while held, at least 90 ms: MOD reads control ports once
 *     per audio block, so a shorter 1 can be missed), the mouth (a fixed 90 ms press: holding it must not undo, as
 *     holding footswitch B does), tracks, parts, Song, and the hidden song steps (click: part; drag or scroll:
 *     repeats).
 * mod-ui sends no change event for a value this script writes, so each write is applied here too.
 * Native DOM only (event.icon[0]), with the state kept on the root: tests/test_modgui_face.py drives this script.
 */
function (event, funcs) {
    var MIN_HOLD = 90;  // ms
    var KEYS = ['onoff', 'stop_play', 'undo', 'redo', 'clear_track', 'clear_all'];
    var NAMES = ['A', 'B', 'C', 'D', '↺', '■'], LOOP = 4;

    var host = event.icon && event.icon[0];
    if (!host) return;
    var root = host.matches('[class*="ml-pedal"]') ? host : host.querySelector('[class*="ml-pedal"]');
    if (!root) return;
    function q(s) { return root.querySelector(s); }
    function qa(s) { return Array.prototype.slice.call(root.querySelectorAll(s)); }
    function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }

    if (event.type === 'start') {
        var st = root._ml = { funcs: funcs, port: {}, held: {}, steps: [], looper: 0, progress: 0, loops: 1, songStep: 0,
                              wheel: { acc: 0, at: 0, stepAt: 0, el: null } };
        for (var i = 0; i < 16; i++) st.steps.push([i ? LOOP : 0, 1]);
        wire(st);
        if (q('.mod-light.off')) root.classList.add('ml-off');
        (event.ports || []).forEach(function (p) { apply(st, p.symbol, p.value); });
        return;
    }
    if (event.type === 'change' && event.symbol && root._ml) apply(root._ml, event.symbol, event.value);

    // ----------------------------------------------------------------------------------------------------- follow
    function apply(st, sym, v) {
        var m;
        if (sym === ':bypass') root.classList.toggle('ml-off', v > 0.5);
        else if (sym === 'looper') { st.looper = Math.round(v); state(st); }
        else if (sym === 'progress') { st.progress = v; q('.ml-prog').style.setProperty('--p', clamp(v, 0, 1).toFixed(4)); eyes(st); }
        else if (sym === 'progress_loops') { st.loops = Math.max(1, Math.round(v)); ticks(st); eyes(st); }
        else if (sym === 'take_secs') {
            var s = Math.max(0, Math.floor(v));
            q('.ml-prog-time').textContent = (Math.floor(s / 60) % 10) + ':' + (s % 60 < 10 ? '0' : '') + (s % 60);
        }
        else if (sym === 'song_step') { st.songStep = Math.round(v); drawSteps(st); }
        else if (sym === 'track') {
            var t = clamp(Math.round(v), 1, 4);
            qa('.ml-trk').forEach(function (b) { b.classList.toggle('sel', +b.getAttribute('data-track') === t); });
            q('.ml-tn').textContent = t;
        }
        else if ((m = /^on_([1-4])$/.exec(sym))) q('.ml-trk[data-track="' + m[1] + '"]').classList.toggle('off', v < 0.5);
        else if (sym === 'part') {
            var p = Math.round(v);
            qa('.ml-pt').forEach(function (b) { b.classList.toggle('sel', +b.getAttribute('data-part') === p); });
        }
        else if (sym === 'song') { st.port.song = v > 0.5; q('.ml-song').classList.toggle('lit', st.port.song); }
        else if (sym === 'volume') {
            var db = Math.round(v * 10) / 10;
            q('.ml-knob').style.setProperty('--v', clamp((db + 60) / 66, 0, 1).toFixed(4));
            q('.ml-db').textContent = (db > 0 ? '+' : db < 0 ? '−' : '') + Math.abs(db).toFixed(1) + ' dB';
        }
        else if (sym === 'rec') { st.port.rec = v > 0.5; root.classList.toggle('ml-rec-on', st.port.rec); }
        else if (sym === 'stop_clear') { st.port.stop_clear = v > 0.5; paintKey(st, 'stop_play'); }
        else if (KEYS.indexOf(sym) >= 0) { st.port[sym] = v > 0.5; paintKey(st, sym); }
        else if ((m = /^step(\d+)_(part|repeats)$/.exec(sym))) {
            var j = +m[1] - 1;
            if (j >= 0 && j < 16) { st.steps[j][m[2] === 'part' ? 0 : 1] = Math.round(v); drawSteps(st); }
        }
    }

    function state(st) {
        for (var k = 0; k <= 5; k++) root.classList.toggle('ml-st-' + k, st.looper === k);
        q('.ml-prog').classList.toggle('ml-first', st.looper === 4);
        eyes(st);
    }

    // One turn per loop while the loop runs (playing, writing, waiting); otherwise the eyes keep their angle: the
    // engine jumps back to 0 when it stops. The first take spins in CSS.
    function eyes(st) {
        if (st.looper !== 2 && st.looper !== 3 && st.looper !== 5) return;
        var x = st.progress * st.loops, a = (x - Math.floor(x)) * 360;
        qa('.ml-spin').forEach(function (g) {
            g.setAttribute('transform', 'rotate(' + ((a * +g.getAttribute('data-spd')) % 360).toFixed(1) + ')');
        });
    }

    function ticks(st) {
        var box = q('.ml-prog-ticks'), n = st.loops, html = '';
        if (n >= 2 && n <= 64) for (var j = 1; j < n; j++) html += '<i style="left:' + (100 * j / n).toFixed(2) + '%"></i>';
        box.innerHTML = html;
    }

    function drawSteps(st) {
        var end = -1;
        for (var j = 0; j < 16 && end < 0; j++) if (st.steps[j][0] >= LOOP) end = j;
        qa('.ml-step').forEach(function (b) {
            var j = +b.getAttribute('data-step') - 1, p = clamp(st.steps[j][0], 0, 5), r = st.steps[j][1];
            b.innerHTML = '<b>' + NAMES[p] + '</b><small>' + (p < LOOP ? '×' + r : '') + '</small>';
            b.classList.toggle('end', p >= LOOP);
            b.classList.toggle('dead', end >= 0 && j > end);
            b.classList.toggle('now', j === st.songStep - 1);
            b.setAttribute('aria-label', 'Step ' + (j + 1) + ': ' + (p === LOOP ? 'Loop' : p > LOOP ? 'Stop'
                           : 'part ' + NAMES[p] + ' times ' + r));
        });
    }

    function paintKey(st, sym) {
        var on = !!(st.held[sym] || st.port[sym] || (sym === 'stop_play' && st.port.stop_clear));
        var el = q('.ml-key[data-port="' + sym + '"]');
        if (el) el.classList.toggle('on', on);
    }

    // ------------------------------------------------------------------------------------------------------ write
    function wire(st) {
        function send(sym, v) {
            apply(st, sym, v);
            st.funcs.set_port_value(sym, v);
        }

        KEYS.forEach(function (sym) {
            var el = q('.ml-key[data-port="' + sym + '"]'), downAt = 0, down = false;
            function release() {
                if (!down) return;
                down = false;
                st.held[sym] = false;
                paintKey(st, sym);
                var held = Date.now() - downAt;
                if (held < MIN_HOLD) setTimeout(function () { send(sym, 0); }, MIN_HOLD - held);
                else send(sym, 0);
            }
            el.addEventListener('pointerdown', function (e) {
                e.preventDefault();
                downAt = Date.now();
                down = true;
                st.held[sym] = true;
                send(sym, 1);
            });
            ['pointerup', 'pointerleave', 'pointercancel'].forEach(function (t) { el.addEventListener(t, release); });
        });

        // The mouth records only: a fixed press, whatever the pointer does (a hold on B undoes; this must not).
        var mouthBusy = false;
        q('.ml-rec').addEventListener('pointerdown', function (e) {
            e.preventDefault();
            if (mouthBusy) return;
            mouthBusy = true;
            send('rec', 1);
            setTimeout(function () { send('rec', 0); mouthBusy = false; }, MIN_HOLD);
        });

        qa('.ml-trk').forEach(function (b) {
            b.addEventListener('click', function () { send('track', +b.getAttribute('data-track')); });
        });
        qa('.ml-pt').forEach(function (b) {
            b.addEventListener('click', function () { send('part', +b.getAttribute('data-part')); });
        });
        q('.ml-song').addEventListener('click', function () { send('song', st.port.song ? 0 : 1); });

        // Steps: click cycles the part; drag (8 px a repeat) or scroll (one per notch) changes the repeats.
        function setRepeats(j, r) {
            if (st.steps[j][0] >= LOOP) return;
            r = clamp(r, 1, 16);
            if (r !== st.steps[j][1]) send('step' + (j + 1) + '_repeats', r);
        }
        var drag = null, suppress = false;
        qa('.ml-step').forEach(function (b) {
            var j = +b.getAttribute('data-step') - 1;
            b.addEventListener('pointerdown', function (e) {
                e.preventDefault();
                drag = { j: j, y0: e.clientY, r0: st.steps[j][1], moved: false };
                suppress = false;
                if (b.setPointerCapture) b.setPointerCapture(e.pointerId);
            });
            b.addEventListener('pointermove', function (e) {
                if (!drag || drag.j !== j) return;
                var d = Math.round((drag.y0 - e.clientY) / 8);
                if (d) drag.moved = true;               // a drag is never a click, on a Loop or Stop step too
                setRepeats(j, drag.r0 + d);             // which has no repeats: nothing written
            });
            b.addEventListener('pointerup', function () { if (drag) suppress = drag.moved; drag = null; });
            b.addEventListener('click', function () {
                if (suppress) { suppress = false; return; }
                send('step' + (j + 1) + '_part', (st.steps[j][0] + 1) % 6);
            });
        });
        // High-resolution and smooth-scrolling wheels send several small events per notch: add up the distance
        // (lines x 40, pages x 800), take a step at 50 px, at most one per 80 ms, and start over after 250 ms of quiet.
        q('.ml-steps').addEventListener('wheel', function (e) {
            var b = e.target.closest ? e.target.closest('.ml-step') : null;
            if (!b) return;
            e.preventDefault();
            var w = st.wheel, now = performance.now();
            if (b !== w.el || now - w.at > 250) w.acc = 0;
            w.el = b; w.at = now;
            w.acc += e.deltaY * (e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? 800 : 1);
            if (Math.abs(w.acc) < 50 || now - w.stepAt < 80) return;
            var n = w.acc > 0 ? 1 : -1, j = +b.getAttribute('data-step') - 1;
            w.acc = 0; w.stepAt = now;
            setRepeats(j, st.steps[j][1] - n);
        }, { passive: false });
    }
}
