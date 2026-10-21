#pragma once

#include "GrooveCorpus.h"

#include <cstdint>
#include <vector>

namespace hhx
{
    /** One arrangement span read from the host's markers. */
    struct SectionSpan
    {
        int startBar = 0;
        int numBars  = 0;
        int section  = SectionVerse;
    };

    /** One block of the song the user placed on the arrangement strip.

        Each block carries its own performance settings, so turning a chorus up
        cannot touch the verse either side of it, and the strip can grow for as
        long as the song does.
    */
    struct ArrangementSection
    {
        int   id       = 1;        // stable identity: seeds this block's takes
        int   numBars  = 8;
        int   section  = SectionVerse;

        float complexity = 0.45f;
        float intensity  = 0.55f;

        /** The block's own Intensity knob: how hard this section is played.
            It scales dynamics only and is deliberately not part of the XY
            pad, which chooses which take is played rather than how loud. */
        float velocity   = 0.55f;

        float fillAmount = 0.35f;
        float swing      = 0.0f;
        bool  halfTime   = false;
        int   variationRhythm = 0;
        int   variationCymbal = 0;

        /** Which kit pieces this block plays, and which of them are pushed
            down to ghost level. A toms-only intro is a mask on that block:
            the pieces it drops are still played by every other block. */
        std::uint32_t laneMask  = (1u << NumLanes) - 1u;
        std::uint32_t ghostMask = 0;
    };

    /** How many bars the whole arrangement covers. */
    int arrangementBars (const std::vector<ArrangementSection>& sections);

    /** Everything the performance depends on. Two identical settings structs
        always produce identical output - that is what makes golden-render
        tests possible.
    */
    struct PerformanceSettings
    {
        // --- performance ------------------------------------------------
        float complexity   = 0.45f;   // pad X: which take is picked
        float intensity    = 0.55f;   // pad Y: which take is picked
        float sectionVelocity = 0.55f; // Intensity knob: how hard it is played

        /** The song's own pad position. The part is chosen from this, so every
            block of the arrangement plays the same part and the block's own
            knobs only decide how hard and how busy it is played. Negative
            means "there is no arrangement": use complexity/intensity. */
        float songComplexity = -1.0f;
        float songIntensity  = -1.0f;
        int   character    = 1;        // index into the corpus character table

        // --- fills ------------------------------------------------------
        float fillAmount     = 0.35f;
        float fillComplexity = 0.5f;
        float fillVelVar     = 0.3f;
        float fillLengthBars = 1.0f;   // 0.5, 1 or 2
        std::uint8_t  fillStyleMask = 0;               // 0 = any style
        std::uint32_t fillLaneMask  = 0xFFFFFFFFu;

        /** What the drummer is actually hearing. Fills are vetted against it:
            the same written figure that reads as a roll at 150 falls apart as
            slow flams at 70, so the tempo decides which fills are playable. */
        float tempoBpm = 120.0f;

        // --- feel -------------------------------------------------------
        float swing          = 0.0f;
        bool  swingSixteenth = false;
        float humanize       = 0.5f;   // how much of the drummer's deviation to keep
        float feel           = 0.5f;   // 0 = laid back, 1 = pushed
        float ghostAmount    = 0.5f;
        float hatOpenness    = 0.0f;
        float kickVariation  = 0.3f;
        bool  rideInsteadOfHat = false;
        bool  halfTime         = false;

        // --- kit / metre ------------------------------------------------
        std::uint32_t laneMask  = 0xFFFFFFFFu;
        std::uint32_t ghostMask = 0;   // lanes forced to ghost level
        float beatsPerBar = 4.0f;      // in quarter notes
        int   timeSigNum  = 4;
        int   timeSigDen  = 4;
        int   phraseBars  = 2;

        int   variationRhythm = 0;
        int   variationCymbal = 0;
        bool  followSections  = false;
        std::vector<SectionSpan> sections;

        /** The user's blocks. When it is empty the settings above play the
            whole song; otherwise each block overrides them for its own bars. */
        std::vector<ArrangementSection> arrangement;

        /** Set while rendering inside a block: the block's own song section and
            an identity salt, so two identical blocks still play different
            takes. Both are derived, never edited directly. */
        int           sectionHint = -1;
        std::uint64_t sectionSalt = 0;

        std::uint64_t seed = 1;
    };

    /** Selection, variation and humanization. Knows nothing about samples.

        Rendering is pure and deterministic: (settings, bar) always yields the
        same hits, so the host can ask for any window at any time and the
        result is stitch-free.
    */
    class PerformanceEngine
    {
    public:
        void setCorpus (const GrooveCorpus* c) { corpus = c; }

        std::vector<Hit> renderBars (const PerformanceSettings& s,
                                     int startBar,
                                     int numBars) const;

        std::vector<Hit> renderPhrasePreview (const PerformanceSettings& s,
                                              int phraseIndex) const;

        /** Where the pad's (complexity, loudness) position lands in the
            corpus. The pad is curved onto the part of the library that plays
            like a song rather than like a practice session, so the top corner
            is still a real take and not the busiest bar in the dataset. */
        static void corpusTarget (const PerformanceSettings& s,
                                  float& complexity,
                                  float& intensity);

        /** The most hits per bar the pad position is allowed to produce. */
        static float densityCap (const PerformanceSettings& s);

        bool phraseEndsWithFill (const PerformanceSettings& s, int phraseIndex) const;

        /** The fill this phrase hands over with, as beats from the start of the
            fill. Manual mode keeps the player's own pattern as the groove and
            asks for this, so a written pattern is still played with fills the
            way a drummer plays it rather than looped verbatim. */
        std::vector<Hit> fillForPhrase (const PerformanceSettings& s,
                                        int   phraseIndex,
                                        float fillBeats) const;

        /** Where the current XY position lands: the nearest real takes, for the
            landing-zone display on the performance page.
        */
        std::vector<int> landingZone (const PerformanceSettings& s, int maxResults) const;

        int sectionAtBar (const PerformanceSettings& s, int bar) const;

        /** Which arrangement block a bar belongs to, 0 when there is none. */
        int blockIndexForBar (const PerformanceSettings& s, int bar) const;

        /** The settings in force at a bar: the base settings with the
            arrangement block covering that bar folded in. Idempotent, so a
            resolved struct can safely be resolved again. */
        PerformanceSettings settingsForBar (const PerformanceSettings& s, int bar) const;

    private:
        /** A hit mid-render: the grid position it has been folded onto plus the
            drummer's own deviation, still separate so Humanize can scale it.
        */
        struct Raw
        {
            float        beat     = 0.0f;
            float        dev      = 0.0f;
            std::uint8_t lane     = LaneKick;
            std::uint8_t velocity = 100;
            /** A fill is played straight into the next section, so it takes no
                swing, feel offset or drift: its timing is settled where it was
                placed. */
            bool         fill     = false;
        };

        struct Sources
        {
            const Phrase* skeleton = nullptr;   // kick / snare / toms
            const Phrase* colour   = nullptr;   // hats / ride / cymbals
        };

        /** Drops the weakest ornamentation until the bar is no busier than the
            pad asked for, so a high pad position stays a groove. */
        void thinOrnaments (const PerformanceSettings& s,
                            float dstBar,
                            int   bars,
                            std::vector<Raw>& raw) const;

        /** Crashes on the landmarks a drummer would hit: the top of a section,
            the downbeat after a fill, and - the harder the section is played -
            the halfway point of the phrase. */
        void addCrashes (const PerformanceSettings& base,
                         const PerformanceSettings& sec,
                         int   phraseIndex,
                         int   bars,
                         float dstBar,
                         std::uint64_t seed,
                         std::vector<Hit>& out) const;

        /** Collapses two cymbal strokes that land within a few milliseconds of
            each other - a closed and an open hat on the same eighth, or a
            phrase boundary landing on top of the phrase before it - into the
            louder of the two. One hand cannot play both, and hearing them
            together is what reads as a doubled, flammed sample. Hits must be
            sorted by beat. */
        static void collapseDoubledCymbals (std::vector<Hit>& out);

        /** Gives a switched-on kit piece a part of its own when the take does
            not play it, so the lane buttons add and subtract whole patterns
            the way Logic's kit-piece selector does. */
        void supplyLanes (const PerformanceSettings& s,
                          float dstBar,
                          int   bars,
                          std::uint64_t seed,
                          std::vector<Raw>& raw) const;

        /** Adds the ghost strokes a drummer leans on when the Ghost knob is
            asking for more of them than the take itself plays. */
        void addGhostNotes (const PerformanceSettings& s,
                            float dstBar,
                            int   bars,
                            std::uint64_t seed,
                            std::vector<Raw>& raw) const;

        Sources pickSources (const PerformanceSettings& s,
                             int phraseIndex,
                             std::uint64_t seed) const;

        std::vector<Hit> renderPhrase (const PerformanceSettings& s,
                                       int phraseIndex,
                                       bool includeFill) const;

        void appendFill (const PerformanceSettings& s,
                         int   phraseIndex,
                         float fillStartBeat,
                         float phraseBeats,
                         std::uint64_t seed,
                         std::vector<Raw>& raw) const;

        /** A plain figure into the downbeat, played dead on the grid: snare and
            toms down the kit with the kick under the last stroke. Used when the
            take chosen for the phrase survives the tempo, the feel and the
            block's piece switches as one or two lonely strokes, which reads as
            a mistake rather than as a simple fill. */
        void appendSimpleFill (const PerformanceSettings& s,
                               float fillStartBeat,
                               float phraseBeats,
                               std::vector<Raw>& raw) const;

        int fillIndexForPhrase (const PerformanceSettings& s, int phraseIndex) const;

        /** Whether a take is a fill a drummer would actually play here, at this
            tempo and in this feel. */
        static bool fillIsPlayable (const PerformanceSettings& s,
                                    const Phrase& f,
                                    float scale,
                                    bool  strict = true);

        std::uint16_t characterMask (const PerformanceSettings& s) const;

        const GrooveCorpus* corpus = nullptr;
    };
}
