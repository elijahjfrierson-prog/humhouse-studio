# HumHouse Studio feature log + lock

Every feature the owner asked for is listed here. `scripts/feature_lock.py` (run in CI) fails the build
if one of the locked strings below disappears from `Source/`. Remove a line only when the owner says so.

## Removed on purpose (owner's call)
- Crash protection (out-of-process plug-in sandbox toggle): removed on all platforms (2026-11-18)
- Phone: Hum Verse, plug-in scanner, EXPORT ALL video button
- Subscription phone app: master plug-ins, spectrums/Scope, Glitch Gate, automation, Patch Bay, Drummer, stem split

## Locked (must stay in the code)
<!-- lock: one string per line, as it appears in Source/ -->
- lock: Set to master
- lock: HumHouse No Latency Driver
- lock: Chroma Verb
- lock: Amp House
- lock: VID POP
- lock: EXPORT ALL
- lock: Shrink
- lock: Warp Stabilizer
- lock: Separate audio
- lock: Find key
- lock: Hum Verse
- lock: Patch Bay
- lock: HumHouse Drummer
- lock: Split stems
- lock: Low Latency Mode
- lock: Recording offset
- lock: Retro Record
- lock: PUSH
- lock: Glitch Gate
- lock: Scope
- lock: Studio EQ
- lock: AutoPitch
- lock: Purify
- lock: Audio to MIDI
- lock: Freeze
- lock: Plugin House
- lock: HUMHOUSE VIEW
- lock: Stem Splitter
- lock: InSequence
- lock: Key Finder
- lock: Peak Controller
- lock: Patch Method
- lock: Templates
- lock: Auto-backup
- lock: Typing keyboard
- lock: Multithreaded mixer
- lock: Smart disable
- lock: RAM disk cache
- lock: Bezier
- lock: Metronome
- lock: Color wheel
- lock: Speed ramp
- lock: Optical
- lock: Low Latency
- lock: Flex-Tune
- lock: Harmony
- lock: Coat (vocal compression)
- lock: PAGE
- lock: Templates on this phone
- lock: Tune Finder
- lock: Recording offset slider
- lock: Iso Room
- lock: AUDIO CLIPS
- lock: BEAT PATTERNS
- lock: always closable, also while streaming
- lock: HumHouse Grand/HumHouse Grand.hhkeys
- lock: class KeysInstrument
- lock: Show in Finder (the clip's audio file)
- lock: Set to: HumHouse
- lock: dawKeyPresets
- lock: Tune Finder: key / BPM / tune of this clip
- lock: Tune Finder: key / BPM / tune of the selected clip
- lock: selection is UI only: no engine copy per mouse move
- lock: any HumHouse template zip
- lock: juce::String unreadableReason
- lock: void claimFileTypes()
- lock: name, knobs, boxes: the menu opens right away
- lock: HumHouse Drummer (turn this track into drums)
- lock: Drummer section the Drummer rows show and edit
- lock: first edit keeps what the drummer was playing
- lock: the same way Cmd+B stacks (each paste after the last)
- lock: follow: no page jumps
- lock: mouse-wheel targets the view glides to
- lock: half a screen above and below: up / down scrolling slides it too
- lock: Background picture (behind the UI, from Downloads)
- lock: phone: compact fixed tracks
- lock: phone slim list: name + R M S; swipe right for the rest
- lock: browser popped out full screen
