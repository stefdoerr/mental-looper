/*
 * Ouroboros — MOD modgui controller. Knobs are bound by MOD-UI from the HTML.
 * This script only drives the seven trigger buttons: a press writes 1 to the
 * port, the release writes 0. MOD reads control ports once per audio block,
 * so a fast tap could collapse to just "0" and miss the rising edge; hold the
 * 1 for at least 90 ms. A button also lights while its port is on, so presses
 * from a footswitch, MIDI or the HMI show up here too.
 */

function (event, funcs) {
    var KEYS = ['rec', 'stop_play', 'undo', 'redo', 'onoff', 'clear_track', 'clear_all'];
    var MIN_HOLD = 90;  // ms

    function paint(icon, sym) {
        var held = icon.data('ob-held') || {}, on = icon.data('ob-port') || {};
        icon.find('.ouroboros-key[data-port="' + sym + '"]').toggleClass('on', !!(held[sym] || on[sym]));
    }

    if (event.type === 'start') {
        var icon = event.icon;
        icon.data('ob-held', {});
        icon.data('ob-port', {});
        (event.ports || []).forEach(function (p) {
            if (KEYS.indexOf(p.symbol) >= 0) {
                icon.data('ob-port')[p.symbol] = p.value > 0.5;
                paint(icon, p.symbol);
            }
        });

        KEYS.forEach(function (sym) {
            var el = icon.find('.ouroboros-key[data-port="' + sym + '"]'), downAt = 0, down = false;
            // mod-ui fires no change event for values this script writes, so
            // record them as the port state too.
            function send(v) {
                icon.data('ob-port')[sym] = v > 0.5;
                paint(icon, sym);
                funcs.set_port_value(sym, v);
            }
            function release() {
                if (!down) return;
                down = false;
                icon.data('ob-held')[sym] = false;
                paint(icon, sym);
                var held = Date.now() - downAt;
                if (held < MIN_HOLD) setTimeout(function () { send(0); }, MIN_HOLD - held);
                else send(0);
            }
            el.on('mousedown touchstart', function (e) {
                e.preventDefault();
                downAt = Date.now();
                down = true;
                icon.data('ob-held')[sym] = true;
                send(1);
            });
            el.on('mouseup mouseleave touchend touchcancel', release);
        });
        return;
    }

    if (event.type === 'change' && event.symbol && KEYS.indexOf(event.symbol) >= 0) {
        var on = event.icon.data('ob-port') || {};
        on[event.symbol] = event.value > 0.5;
        event.icon.data('ob-port', on);
        paint(event.icon, event.symbol);
    }
}
