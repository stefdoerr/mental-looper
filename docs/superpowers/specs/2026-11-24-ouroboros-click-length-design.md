# Click and fixed length (short design)

Approved in conversation on 2026-11-24 ("ok do it"). A small change on top of tempo sync, done before song parts.

## Why

Building song parts by ear needs a click that lines up with the bars Ouroboros snaps to. The only metronome on the
user's Dwarf (Caps Click) has a BPM control only: no accent, no link to MOD's bar position or play/stop. A fixed
length lets the first loop close by itself after a set number of bars instead of hitting B exactly.

## Decisions

- **Click (Off / On, default Off) and Click Level (-40 to 0 dB, default -10 dB):** new settings.
  - It sounds only with Sync = Bars, a usable tempo, and MOD playing. It ticks on MOD's beats, taken from the
    same time position the bar grid comes from, so it cannot drift from the bars recordings snap to.
  - Beat 1 is accented: a higher, louder tick (1500 Hz against 1000 Hz at -6 dB, 25 ms each). *My call.*
  - It goes to its own new audio output, "Click", so the pedalboard can route it to headphones or output 2 and keep
    it out of the PA. The loop output is unchanged.
  - Off, or not sounding: the Click output is silence, one fill per block. The engine's per-sample path is
    untouched (the CPU rule from tempo sync).
- **Length (Free, 1 to 16 bars, default Free):** new setting.
  - With Sync = Bars and a grid, the first recording closes by itself on its Nth bar line, exactly as a B press
    waiting for that line would (After Rec applies). B before then closes it as usual.
  - First loop only. A recording started with no grid ignores it, and so does Sync Off.
  - Sync Off: the per-sample path gains only flag tests.
- **Ports:** the three settings go before Bypass, as Sync did. The click output is the second audio output; pedalboards
  connect by symbol, so existing ones keep working.

## Not doing

- A click while MOD is stopped (the click follows MOD's play/stop like the loops do).
- A count-in setting: with MOD playing, B already waits for the next bar line, so the click is the count-in.
- Length for recordings onto the loop: they lock to the first loop already.

## Checks

- Engine: Length closes the first loop on bar N with MOD playing and with MOD stopped (free grid); B before then
  wins; Sync Off ignores it.
- Plugin: click ticks every beat at 120 BPM 4/4, accented on the bar; silent with Click off, with MOD stopped and
  with Sync Off.
- Manual (Tempo sync section and settings table) and README; the PDF stays 2 pages.
