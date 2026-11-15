# Zenith vs Logic Pro, FL Studio, Ableton Live, Pro Tools: heavy sessions

This compares the engines' public, documented behaviour (manuals, support notes, developer talks) with what HumHouse Zenith does today.
It is a study of architecture, not of their code: none of their code or binaries were used.

## How each DAW survives a heavy session

| Area | Logic Pro | FL Studio | Ableton Live | Pro Tools | Zenith (HumHouse) |
|---|---|---|---|---|---|
| Live vs playback tracks | Only the **selected / record-armed** track runs at the I/O buffer ("live mode"); all others render ahead in the large **Process Buffer Range** (S/M/L) | Everything runs at the device buffer; "Smart disable" turns off idle plug-ins | Everything at the device buffer; freeze for relief | **Native**: playback engine buffer for all; **HDX**: DSP chips; **Hybrid Engine** moves armed tracks to DSP mode | Armed / monitored tracks live; the rest is rendered ahead (anticipative FIFO, adaptive up to 250 ms) |
| Multicore | One thread per track/bus graph node ("Processing threads"), per-core meter | Multithreaded mixer; per-insert "Multithreaded processing", generators split by voice | Graph split by **track chain**; one chain = one core | Native: per-track across cores; Dynamic Plug-in Processing skips silent plug-ins | WorkerPool: independent tracks in parallel, buses after their sources |
| Silent plug-ins | Sleeps instruments with no notes after a tail | Smart disable (manual or auto) | Devices keep running | **Dynamic Plug-in Processing**: idle plug-ins cost 0 | Skips plug-ins that output pure silence; idle UI costs nothing |
| Freeze / bounce | Freeze (pre-fader or source only) | Consolidate / render to audio clip | Freeze + Flatten | Track Freeze, Commit, Bounce | Pre-render (anticipative); **gap: no one-click Freeze of a track** |
| Latency (PDC) | Full PDC; "Low Latency Mode" bypasses big-latency plug-ins on the live track | PDC per insert, manual offsets | Full PDC, "Reduced latency when monitoring" | Full ADC up to the engine limit; low-latency monitoring | Full PDC + 64-bit bus sum; **gap: no Low Latency Mode toggle** |
| Automation | Sample accurate, curve per segment, 0-127 or dB readouts | Event / envelope clips with tension curves | Breakpoints + curved segments (Alt-drag) | Sample accurate, trims, latch/touch/write | Sample accurate; **now** Bezier curve per segment + 0-127 readouts |
| Plug-in crash | Audio Unit hosting in separate processes (AUHostingService) | Bridged / sandboxed plug-ins optional | Plug-ins in process | AAX in process | Out-of-process isolation available + risky-call guard |
| Overload behaviour | "System overload" alert, stops | Underruns counted, keeps playing | CPU meter, keeps playing (clicks) | Error -6101 / -9128 stops | Keeps playing, counts dropouts, grows render-ahead |
| Disk | Disk I/O meter, streaming | Samples in RAM by default | Streaming, RAM mode per clip | Disk cache (RAM pre-load) | Disk writer thread for recording, RAM-streamed playback |

## Where Zenith already leads
- Render-ahead **grows by itself** after a dropout (Logic needs you to change the Process Buffer by hand).
- Never stops on overload (Pro Tools and Logic do).
- Out-of-process plug-in hosting like Logic, which FL/Ableton/Pro Tools don't do by default.
- Silent-plug-in skipping like Pro Tools' Dynamic Plug-in Processing.

## Where Zenith lacks (next builds)
1. **Track Freeze** (one click render track + FX to audio, unload the plug-ins): Logic/Ableton/Pro Tools all have it.
2. **Low Latency Mode**: bypass high-latency plug-ins on the live track only while recording (Logic).
3. **Per-core / per-track CPU meter** visible to the user (Logic's per-core meter, FL's per-insert CPU).
4. **Voice-level multithreading** for big instruments (FL splits a generator's voices across cores).
5. **Disk cache / RAM preload** setting for huge audio sessions (Pro Tools).
6. **Hybrid DSP-style split**: pin record-armed tracks to a dedicated high-priority worker (Pro Tools Hybrid Engine idea, done in software).

## Stress test plan (40+ VST3 + built-ins incl. Amplitude)
- Session: 40 audio tracks with 1 VST3 each + 10 instrument tracks + 6 buses, AmpliTube / Waves on 10 tracks, HumHouse Amplitude on 4.
- Log per run: callback load (`blockLoadFrac`), dropout count, render-ahead size, UI frame time per component, edit-lock time (`swapUsMax`).
- Pass = 0 dropouts at 128 samples with one armed track, UI frame under 16 ms on click.
- The UI-jump-on-click investigation (AmpliTube / Waves) uses the same log: a jump that matches a long `swapUsMax` is the edit lock; one that matches a long frame is painting.
