# HumHouse Studio design (v1)

Native JUCE 8 standalone app (macOS universal + Windows x64, Linux for CI tests).
Hosts the user's VST3 (and AU on macOS) plugins via AudioPluginFormatManager + KnownPluginList.

## Layout (BandLab-simple, FL-deep)
Top bar: logo, transport (play/stop/rec, PAT/SONG, BPM, metronome, count-in), key + scale picker,
view tabs BEAT (Channel Rack + Piano Roll) | ARRANGE (Playlist) | MIX (Mixer), browser toggle.
Left browser: Kits, Samples, Instruments, Effects, Plugins. "+ Track" quick menu: Record Vocals,
Drum Kit, Instrument, Import Audio. Black UI with grids, orange/teal accents.

## Model (Source/Model)
Note{key,start,length(beats),velocity,muted,selected}; Pattern{name,colour,map channelId->notes,lengthBars};
Channel{id,name,type Sampler|Synth|Plugin, instrument, mixerTrack, vol, pan, mute, solo, glide};
Playlist lanes of Clip{lane,start,length,patternIndex|audio, offset, gain, muted};
MixerTrack{name, vol, pan, mute, solo, fx[8], sends{target,level}} index 0 = Master.
Key root + Scale for highlighting; snap grid.

## Engine (Source/Engine)
AudioIODeviceCallback. Renders pattern loop (PAT) or song (SONG). Per block: gather note on/off
per channel (sample accurate), render instruments into channel buffers, sum to mixer inserts,
audio clips + live input (armed lane) into lane insert, inserts processed in topological order
through fx + sends -> master. Recording: input -> ThreadedWriter wav in project folder; clip added on stop.
Metronome + count-in. Offline render to WAV. Audio thread uses a try-lock on the project lock.

## Instruments: Drum Sampler (sample one-shot / pitched, glide), Synth (saw/square/tri/sine, ADSR,
filter, glide, presets incl 808), Plugin instruments. Built-in synthesized drum kit so it works without samples.
Drum kits: any folder of samples -> one channel per sample; kit folders remembered in settings.

## Built-in FX (renamed): Para EQ, Compressor, Limiter, Room Verb, Echo Tape, Chorus, Phase Swirl,
Grit (dist), Sweep Filter, Glitch Gate (Gross-Beat-style 16-step stutter: repeat 1/4..1/32, reverse,
half speed, tape stop, mute), Stereo Wide, Soft Clip, Bit Crush, Noise Gate, Pump (sidechain-style duck), De-Esser.

## Note tools: see SHORTCUTS.md (Alt+A arp/roll, Alt+S strum, chop, flam, quantize, randomize,
limit, articulate, scale levels, flip, claw, LFO, glue, legato, quick chop, transpose, duplicate).

## Big-session performance (Pro Tools-size sessions)
- Mixer graph processed by a worker pool: tracks grouped into dependency levels; tracks in a level render in parallel.
- Audio clips stream from disk (BufferingAudioReader per clip) instead of full RAM loads.
- Freeze track (render to WAV, bypass its chain), auto-idle plugins/fx after silence, CPU meter, buffer/sample-rate settings.
- No allocation or locks in the audio callback (try-lock project; preallocated buffers).

## AutoPitch (built-in vocal tuner, BandLab-style)
Follows project key/scale. Retune speed, humanize, formant-free PSOLA/granular shift, styles:
Classic, Modern, Hard (robot), Natural. Pitch detect = YIN on downsampled mono.

## User requirements added mid-build (must all ship)
- Studio-grade Pro Tools-style stock FX (own DSP): Studio EQ 7 (HPF/LPF 6-24dB, 2 shelves, 4 para, analyzer),
  Studio Dyn (Dyn3-style comp/limiter: knee, peak/RMS, SC HPF, GR meter), FET 76 (1176-style 4/8/12/20/all),
  Studio Gate/Expander, De-Esser, Channel Strip (gate->comp->EQ), Studio Limiter, oversampled saturation.
- FL-style recording: per-lane input picker + arm, multiple armed lanes record simultaneously, R+Space,
  count-in, take lands at exact position, playhead returns to start on stop, monitoring through insert FX.
- Punch: drag on playlist ruler = time selection = punch range; 1 bar pre-roll; records only inside range;
  existing clip split at punch points w/ short fades; loop-record within range = takes (switch via clip menu).
- Takes/punches stay on the SAME lane and SAME mixer insert (routing follows recording, Clip::mixerTrack).
- Fades: fade-in/out handles on every audio clip corner, Alt-drag curve, right-click fade selected, auto crossfade.
- Ctrl+S saves + timestamped backup in Backup/ folder; auto-backup every 5 min (keep 50); 999-level undo
  (Ctrl+Z, Ctrl+Shift+Z/Ctrl+Y redo, Ctrl+Alt+Z step), History panel.
- Hover tooltips on every knob/button/fader + FL-style hint bar showing name/value/shortcut.
- "Plugin House" browser: built-ins (incl. AutoPitch) + scanned 3rd-party VST3/AU (HumHouse Autotune if installed).
- Big sessions: parallel mixer worker pool, disk-streamed clips, freeze, auto-idle FX, CPU meter.

## Next (requested 2026-10-18, add to PR #2 branch devin/1792315880-crash-fixes-logo-theme)
- FL tool toolbar in Piano Roll + Playlist: Draw (P), Paint (B: drag paints repeated notes/clips at snap), Delete (D),
  Mute (T), Slice (C: click/drag cuts notes/clips), Select (E), Zoom (Z), Group (Ctrl+G in playlist / group id on notes+clips: grouped items select+move together).
- Mixer routing wires: draw cables from each strip to its targets (sends + master), FL routing view; channel->insert wires too.
- Sampler/808 channel playback modes: One-shot (ignore note-off), Cut itself (new hit chokes previous voice), Cut group id
  (e.g. hats choke each other), amp envelope A/H/D/R per channel, ~3 ms declick fade on choke. Expose in Channel settings + rack menu.
- DONE (uncommitted->commit): sampler oneShot / cutItself / cutGroup / amp A-H-R (Model, Instruments, Engine renderChannels choke events, ProjectIO, channel editor, rack menu).
- PARTLY DONE (shuffle committed; drag-replace, used-samples list DONE (View menu); drag-replace + browser highlight, FL tool toolbar, mixer wires still TODO). Kit browsing like FL (requested 2026-10-18): on a sampler channel, Up/Down arrows or mouse wheel over the channel name
  shuffle to prev/next audio file in the same folder (live while playing); drag a browser sample onto a rack row replaces
  its sample, onto empty rack space adds a channel; browser highlights samples used in the project; menu "Samples used"
  lists every sample file in the project.
- Alt+A arp/rolls already implemented (ToolDialogs Tool::Arp, rack right-click roll x2..x8) - user re-asked, told them.
- UI sizing (requested 2026-10-18): Options > UI scale 75-200% (Desktop::setGlobalScaleFactor, saved "uiScale");
  playlist lane height per track: drag lane bottom edge / Alt+wheel on header (Lane::height already in model+IO, Playlist uses const laneH=60 -> make per-lane);
  "All tracks: small/medium/large"; detach Mixer / Piano Roll / Channel Rack / Browser into own resizable DocumentWindow (FL detached).
- Clean rack (requested 2026-10-18): Project::createDefault() should start with an EMPTY channel rack (no starter drums/808/keys,
  empty pattern) - rack only shows samples the user chose; keep lanes + mixer. Update tests that rely on default channels
  (engine renders default beat test -> add a sampler/drum channel in the test instead).
- Sleek pass: flatter thinner controls, softer rounded panels w/ subtle gradients, quieter grid lines, cleaner spacing/type (HHLookAndFeel + view paints).
- Tracks (requested 2026-10-18): Playlist + Mixer: "+ Track", right-click Insert above/below, Delete track (key Del when
  headers selected), click header = select/highlight, Shift = range, Ctrl/Cmd = toggle; selected headers highlighted;
  mute/arm/height/delete apply to all selected. Needs Lane::selected / MixerTrack::selected (UI-only, not saved).
- DONE: createDefault() is now an empty rack; createDemo() keeps the starter beat (tests use it).
- FL continuous punch: setRecording(true/false) while playing already starts/finishes writers without stopping transport (R key / rec button). Verify keyboard R toggles during playback; each toggle = new take on same lane/insert.
- DONE: default 10 tracks. TODO: Playlist lane height = max(60, visible height / lanes) so 10 tracks fill view evenly (replace const laneH).
- DONE: default lanes/inserts named Track 1..10 (+Master). TODO: '+ Track' in Playlist/Mixer should name 'Track N' not 'Vocal N'/'Insert N'.
- Templates + presets (requested 2026-10-18): File > Save as template (.hhs in ~/Music/HumHouse Studio/Templates), New from template, set default template; preset menu in EffectEditors ParamGrid + plugin windows (Save/Load preset: builtin params XML, plugin getStateInformation), stored in ~/Music/HumHouse Studio/Presets/<type>/.
- Instrument Pool (requested 2026-10-18): built-in synth instruments listed in Plugin House > Instruments, addable as channels: Prism (wavetable psychedelic pad, morph + phaser shimmer), Drift (detuned supersaw lead, wobble LFO), Lava (analog mono bass, squelch filter) + 808, Glass (FM bell/pluck keys), Haze (lo-fi tape keys wow/flutter/noise), Nebula (evolving drone). Each with presets. Implement as SynthParams presets + new wave/engine options in SynthInstrument.
- Live instruments (requested 2026-10-18): pool adds piano/EP/Rhodes, guitar pluck, bass gtr, strings, brass, flute, choir (synth-modelled, no copied sample libs); MIDI keyboard input (deviceManager MIDI callback -> previewNote on selected channel) + computer-keyboard piano (FL typing layout) + record into piano roll; live guitar/bass via armed track + amp FX.
- FL channel settings (requested 2026-10-18): wrench icon on each rack row + click name opens 3-tab panel: SMP (normalize, reverse, declick, fade in/out, trim silence, boost, pitch/fine, root, loop, stretch, invert), INS (one-shot, cut, AHDSR, filter cutoff/res, pan, glide, mono, vel->vol), FUNC (portamento, arp, time offset/swing, note length, crossfade). Non-destructive (Channel fields + sampler render), undo, saved in ProjectIO.
- Stretch modes (requested 2026-10-18): SMP tab + audio clips: Resample (pitch+speed), Stretch (tempo-synced time, pitch kept), Stretch X (time multiplier + pitch shift w/o length change), Fit to tempo (beats length). Use granular/PSOLA stretcher (reuse PitchShifter in Effects).
- DONE (Browser.cpp SampleBrowser rewrite). Sample browser (requested 2026-10-18): default root = ~/Downloads (userHomeDirectory/Downloads) when no saved folders; FL-like look: folder tree with icons, HumHouse logo (drawLogo) in browser header, folder chooser buttons, search bar filtering files (WildcardFileFilter on '*query*'), click = preview.
- SHIP POLICY (user 2026-10-18 'no ship as all'): do NOT send partial installers; build every item above, then one CI-green .pkg + Setup.exe.
- DONE: Playlist laneH even-fill (min 60), + Track / Add insert name 'Track N', mixer title shows track name.
- DONE: Options > UI scale 75-200% (saved uiScale).
- DONE: Templates (File > Save as template / New from template / default template in ~/Music/HumHouse Studio/Templates).
- DONE: PRESETS button on every built-in effect and synth/sampler editor (~/Music/HumHouse Studio/Presets/<kind>/*.hhpreset). Third-party plugin state presets still TODO.
- DONE: Track select (click/Shift/Ctrl-Cmd, highlighted), insert above/below, delete selected (menu or Delete key), arm/mute apply to selection.
- DONE: Mixer routing wires (WireOverlay cables from each insert to send targets/master; selected insert highlighted).
- DONE: Right-click track header / mixer insert > Presets (add effect from saved preset, FX chain presets save/load incl. plugin state).
- DONE: Piano roll FL toolbar DRAW/PAINT/DEL/MUTE/SLICE/SEL/ZOOM + keys P B D T C E Z.
- DONE: Playlist tools SEL/PAINT/DEL/MUTE/SLICE (+E B D T C), clip GROUP (Ctrl/Cmd+G, Shift ungroup, Clip.group saved), Split by channel, notes/steps light up as they play.
- DONE: 3-page channel settings (SMP/INS/FUNC): normalize, boost, reverse, polarity, declick, trim, fades, Resample/Stretch/Stretch X (OLA) baked non-destructively.
