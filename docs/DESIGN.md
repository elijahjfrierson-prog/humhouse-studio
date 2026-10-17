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
