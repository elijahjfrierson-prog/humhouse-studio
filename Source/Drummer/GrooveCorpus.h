#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hhx
{
    /** Canonical 30-piece articulation map.

        The corpus, the performance engine, the sampler and the MIDI exporter
        all speak these ids; only the exporter knows about GM note numbers.
        Keep in sync with tools/corpusx/lanes.py.
    */
    enum Lane : std::uint8_t
    {
        LaneKick = 0,
        LaneSnare,
        LaneSnareRim,
        LaneSideStick,
        LaneSnareGhost,
        LaneSnareFlam,
        LaneSnareRoll,
        LaneHatClosed,
        LaneHatTight,
        LaneHatOpen1,
        LaneHatOpen2,
        LaneHatOpen3,
        LaneHatOpen4,
        LaneHatPedal,
        LaneHatSplash,
        LaneHatBell,
        LaneRideBow,
        LaneRideBell,
        LaneRideEdge,
        LaneRideCrash,
        LaneCrashL,
        LaneCrashR,
        LaneCrash3,
        LaneChina,
        LaneSplash,
        LaneTom1,
        LaneTom2,
        LaneTom3,
        LaneTom4,
        LanePerc,
        NumLanes
    };

    /** The hi-hat openness ladder, closed to widest. */
    inline constexpr int kHatLadder[] = { LaneHatClosed, LaneHatTight, LaneHatOpen1,
                                          LaneHatOpen2,  LaneHatOpen3, LaneHatOpen4 };
    inline constexpr int kNumHatSteps = 6;

    enum Section : std::uint8_t
    {
        SectionIntro = 0,
        SectionVerse,
        SectionChorus,
        SectionBridge,
        SectionOutro,
        SectionFill,
        NumSections
    };

    enum FillStyle : std::uint8_t
    {
        FillStraight   = 1,
        FillTriplet    = 2,
        FillRoll       = 4,
        FillSyncopated = 8,
        FillTomLed     = 16,
        FillCymbalLed  = 32
    };

    const char* laneName (int lane);
    const char* sectionName (int section);

    bool isSnareLane (int lane);
    bool isHatLane (int lane);
    bool isRideLane (int lane);
    bool isTomLane (int lane);
    bool isCymbalLane (int lane);

    /** General MIDI note for a lane. Several articulations share a GM note, so
        this is used for reading corpus MIDI, not for writing it. */
    int laneToGmNote (int lane);
    int gmNoteToLane (int note);

    /** The HumHouse drum map: one distinct note per articulation, used when the
        plugin emits or exports MIDI. */
    int laneToNote (int lane);
    /** The reverse of laneToNote, falling back to the GM map. -1 when the note
        addresses no piece on the kit. */
    int noteToLane (int note);

    /** A drum hit ready to play: absolute beat inside the rendered timeline.

        `variant` is the round-robin slot the sampler must use. It is decided
        by the performance engine, because avoiding two identical strokes in a
        row is a performance decision, not a playback one.
    */
    struct Hit
    {
        float         beat     = 0.0f;
        std::uint8_t  lane     = LaneKick;
        std::uint8_t  velocity = 100;
        std::uint8_t  variant  = 0;
    };

    /** A hit as stored in the corpus: the musical grid position the drummer
        was aiming at, plus how far off it they actually landed. Keeping the
        two apart is what lets the Feel control scale real human timing
        instead of adding noise to a quantised grid.
    */
    struct SourceHit
    {
        std::uint16_t grid     = 0;   // 1/48 beat units
        std::int8_t   dev      = 0;   // 1/512 beat units, signed
        std::uint8_t  lane     = LaneKick;
        std::uint8_t  velocity = 100;

        float gridBeat() const { return (float) grid / 48.0f; }
        float devBeats() const { return (float) dev / 512.0f; }
    };

    /** One bar-aligned slice of a real performance. */
    struct Phrase
    {
        std::vector<SourceHit> hits;
        float         complexity = 0.0f;   // 0..1
        float         intensity  = 0.0f;   // 0..1
        float         swing      = 0.0f;   // measured, 0 straight .. 1 triplet
        int           bars       = 2;
        int           bpm        = 120;
        std::uint16_t charMask   = 0;
        std::uint8_t  section    = SectionVerse;
        std::uint8_t  fillStyles = 0;
        std::uint8_t  sigNum     = 4;
        std::uint8_t  sigDen     = 4;
        bool          isFill     = false;

        /** True when the bar was re-voiced from a real take to fill a corner of
            the pad rather than being played by a person. Selection prefers a
            recorded take wherever one covers the position. */
        bool          derived    = false;

        /** The metre the take was actually played in, in quarter notes. */
        float sourceBeatsPerBar() const
        {
            return (float) sigNum * 4.0f / (float) sigDen;
        }
    };

    /** Human-performance groove library.

        Compiled offline by tools/corpusx/build_corpus.py from the Magenta
        Groove MIDI Dataset (CC-BY 4.0). Selection is nearest-neighbour on the
        (complexity, intensity) plane within a character - the plugin never
        invents a pattern, it picks takes a person actually played and varies
        and blends them.
    */
    class GrooveCorpus
    {
    public:
        static constexpr int kVelocityBuckets = 8;

        /** Binary layout the runtime understands. Installed content carries the
            same number in content_manifest.json so a stale install is rejected
            instead of half-loaded. */
        static constexpr std::uint32_t kFormatVersion = 4;

        bool loadFromMemory (const void* data, std::size_t numBytes);

        bool isLoaded()      const { return loaded; }
        int  numBeats()      const { return (int) beatPhrases.size(); }
        int  numFills()      const { return (int) fillPhrases.size(); }
        int  numCharacters() const { return (int) characterNames.size(); }

        const std::string& characterName (int i) const { return characterNames[(std::size_t) i]; }

        const Phrase& beat (int index) const { return beatPhrases[(std::size_t) index]; }
        const Phrase& fill (int index) const { return fillPhrases[(std::size_t) index]; }

        /** The k nearest real takes to an XY position, closest first.

            Used both for selection (element 0..n) and for morphing, where the
            ornamentation of a neighbour is blended over the primary take's
            kick/snare skeleton.
        */
        std::vector<int> neighbours (float complexity,
                                     float intensity,
                                     std::uint16_t charMask,
                                     int   bars,
                                     int   maxResults,
                                     int   sigNum = 0,
                                     int   sigDen = 0) const;

        /** Index of a beat phrase near (complexity, intensity).

            `variation` walks the ranked neighbour list, so the UI's numbered
            variation buttons map straight onto "the Nth closest real take".
            `section` is a preference, not a filter: matching phrases are
            pulled forward but a near miss still beats an empty bar.
        */
        int pickBeat (float complexity,
                      float intensity,
                      int   variation,
                      std::uint16_t charMask,
                      int   bars,
                      int   section,
                      int   sigNum = 0,
                      int   sigDen = 0) const;

        /** Fill selection, filtered by bar length, by the lanes the user
            allows fills to use, and by requested playing style. `avoid` holds
            recently played fills so the same one never lands twice in a row.
        */
        int pickFill (float complexity,
                      float intensity,
                      int   bars,
                      int   variation,
                      const std::vector<int>& avoid,
                      std::uint32_t laneMask,
                      std::uint8_t  styleMask,
                      int   sigNum = 0,
                      int   sigDen = 0,
                      std::uint16_t charMask = 0) const;

        /** Learned velocity transition row for a lane: given the bucket of the
            previous stroke, the probability (0..255) of each next bucket.
        */
        const std::uint8_t* velocityRow (int lane, int fromBucket) const;

    private:
        std::vector<int> rank (const std::vector<Phrase>& pool,
                               float complexity,
                               float intensity,
                               std::uint16_t charMask,
                               int   bars,
                               int   section,
                               int   maxResults,
                               int   sigNum,
                               int   sigDen) const;

        std::vector<Phrase>      beatPhrases;
        std::vector<Phrase>      fillPhrases;
        std::vector<std::string> characterNames;
        std::vector<std::uint8_t> velocityModel;   // NumLanes * buckets * buckets
        bool                     loaded = false;
    };
}
