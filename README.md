# Mental Looper

![Mental Looper, a four-track looper](plugins/MentalLooper/modgui/screenshot-mental-looper.png)

**A four-track looper for free-time playing, for [MOD Dwarf](https://mod.audio/dwarf/),
[MOD Desktop](https://mod.audio/desktop/) and any LV2 / VST3 / CLAP host.** No click and no
bars: the first loop you record sets the length, and the other three tracks lock to it. Overdub
as much as you like and undo one layer at a time. Built with the
[DISTRHO Plugin Framework (DPF)](https://github.com/DISTRHO/DPF), mono in and mono out.

- Rec / Play / Dub on one footswitch; undo (hold) on the same one; on/off (tap), clear (hold) and stop (double-tap) on another, with redo on a knob.
- Optional auto-record: arm a track and it starts on your first note.
- Optional tempo sync: loops start on MOD's bar lines, are whole bars long, and follow MOD's play/stop. A click on its own output and a fixed loop length in bars come with it.
- Click-free loop point, punch in/out, undo, redo and clear.
- Tracks can be longer than the first loop: keep playing past the end.
- Four parts (A-D) and a 16-step song: switch parts at the end of a pass, or let the song play them in order with repeats, Loop and Stop.
- Save the whole piece (4 parts, mix, arrangement) to one of 16 slots and load another between songs, on the Dwarf. Pieces are plain WAV folders you can copy.
- Per-track volume and on/off. The Dwarf screen shows a progress bar, track states and layer counts
  (Starless 19 or later).
- The output is only the loops: wire your dry signal around it, or turn up **Dry**.

**[Install](#install) · [Manual (PDF)](docs/manual/mental-looper-manual.pdf) · [Building](DEVELOPERS.md)**

## Install

Build from source and copy `mental-looper.lv2` to your host.

### MOD Desktop (Linux)

```bash
git clone --recurse-submodules https://github.com/stefdoerr/mental-looper.git
cd mental-looper
make
./install.sh        # copies bin/mental-looper.lv2 into ~/Documents/MOD Desktop/lv2/
```

Restart MOD Desktop. The plugin appears under brand **Stefan** as **Mental Looper**.

### MOD Dwarf (hardware)

```bash
make dwarf          # cross-build (needs Docker, see DEVELOPERS.md) and deploy
```

or copy a prebuilt `mental-looper.lv2/` over by hand:

```bash
scp -O -r mental-looper.lv2 root@192.168.51.1:/root/.lv2/
ssh root@192.168.51.1 'systemctl restart jack2 mod-ui'
```

### Desktop DAWs

`make` also writes `bin/mental-looper.vst3` and `bin/mental-looper.clap`. Copy them into `~/.vst3/` and
`~/.clap/`. Rec/Undo and Stop/Clear are press-and-release switches: outside MOD, map them to a
momentary control such as a MIDI footswitch.

## Manual

The one-page **[PDF manual](docs/manual/mental-looper-manual.pdf)** covers the controls, the
footswitch gestures and the settings. On a MOD device it is the *documentation* button in the
plugin's info dialog.

## Building & contributing

Build targets, the Dwarf cross-build and the DPF patch are in **[DEVELOPERS.md](DEVELOPERS.md)**.
For LLM-assisted work, point your agent at [`INSTRUCTIONS.md`](INSTRUCTIONS.md).

## Acknowledgements

- [DISTRHO Plugin Framework (DPF)](https://github.com/DISTRHO/DPF), the LV2/VST/CLAP framework.
- [MOD Audio](https://mod.audio), for the Dwarf, MOD Desktop and the modgui design.

## License

Plugin code is ISC-licensed. DPF is ISC-licensed (see [`dpf/LICENSE`](dpf/LICENSE)).
