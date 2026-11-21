# SCENEDRIFT

A polyrhythmic saw+FM synth sequencer for the M-VAVE FM-1, ported from the Scenedrift HTML toy. Four lanes
(lead, bass, pluck, air) share one detuned-saw engine with light FM, each looping its own length against one clock.
Per-step locks, trig conditions, a twin delay, and an XY pad that morphs between four stored sounds. Made with
[foma](https://github.com/konsumer/foma).

**Try it in the browser, and install it on the FM-1 from Chrome or Edge:** <https://sunjaculley.github.io/scenedrift-fm1/>

## Playing it

| Control | What it does |
| --- | --- |
| White keys 1-16 | Steps of the selected lane. Tap: on/off |
| Hold a step | KNOB1-4 lock PITCH, LEVEL, SEND, HOLD to it. SELECT sets its trig condition (all, 10-90 %, 1:2 ... 4:4). EDIT turns its locks on/off. D#5 clears it |
| F#3 G#3 A#3 C#4 | Lane select: LEAD, BASS, PLUCK, AIR. Hold one and turn KNOB1: that lane's length (1-16) |
| D#4 | RANDOM pattern for the lane, weighted toward downbeats |
| F#4 G#4 A#4 C#5 | Scenes A-D. Tap: jump the XY puck to that corner. Hold SAVE + tap: store the current sound there |
| D#5 | CLEAR the lane (with steps held: clear those steps) |
| F#5 | Hold 1 s: INIT, back to the sparse start state |
| ALGORITHM / PRESETS | XY morph: X and Y. Blends 20 sound settings between the four scenes |
| EDIT · SEL · ENV · LFO · FX · GLO · ARP | Pages: OSC · CHARACTER · ENVELOPE (+ width) · LFO (+ gravity) · DELAY · GLOBAL (tempo, swing, root, trail) · SYSTEM (MIDI sync, unison, octave). SELECT cycles them, HOME goes to OSC |
| OCT- / OCT+ | Everything an octave down / up |
| PLAY / SAVE | Start, stop / tap SAVE while stopped to store everything in flash |
| MASTER | Volume (hardware) |

Lights: steps of the selected lane, the playhead blinking against them, the selected lane's key, and the scene
corner the puck is nearest.

## What changed for the FM-1

- **Two notes per lane.** A new note replaces the older of the two. The HTML version started an unlimited number
  of overlapping notes; the chip needs a fixed budget.
- **CPU guard.** The header shows the chip's audio load. Above 85 % the engine thins the unison from 7 saws to 5,
  then 3, and gives them back when there is room. UNISON on the SYSTEM page sets the maximum.
- **Delays** are mono lines panned apart (A left, B right) instead of two stereo lines, to halve their memory.
- **Gravity** drives the particle field inside the XY square; Trail still lengthens the delay feedback.
- **Master volume** is the FM-1's MASTER knob. The output has a limiter with its ceiling 2 dB under full scale.
- **New:** scene keys jump straight to a corner; OCT shifts every lane; MIDI clock sync (SYSTEM page).

## Develop

Only Docker is needed:

```sh
./fm1 dev          # http://localhost:8000: the emulator, the installer, the rescue; rebuilds when you save
./fm1 build        # build/scenedrift-VERSION.fwsc
```

The firmware is in `src/main.c`; what it can call is in [platform/fm1.h](platform/fm1.h). Every push to `main`
publishes the emulator and the installer to GitHub Pages; a tag (`v0.2`) also makes a release with the `.fwsc`.

Installing firmware is at your own risk; back up your FM-1 first. If it does not start, hold OCT- while switching it
on (the USB rescue), then install from the page.

## License

GPL-3.0-only. Credits and third-party parts: [LICENSING.md](LICENSING.md).
