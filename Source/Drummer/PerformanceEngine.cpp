#include "PerformanceEngine.h"

#include <algorithm>
#include <cmath>

namespace hhx
{
    namespace
    {
        std::uint64_t mix (std::uint64_t x)
        {
            x += 0x9E3779B97F4A7C15ull;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            return x ^ (x >> 31);
        }

        /** Deterministic 0..1 draw from a seed and a couple of salts. */
        float rand01 (std::uint64_t seed, std::uint64_t a, std::uint64_t b = 0)
        {
            return (float) ((mix (seed ^ mix (a * 0x2545F4914F6CDD1Dull + b)) >> 11)
                            * (1.0 / 9007199254740992.0));
        }

        /** Sine of `turns` full cycles, built only from multiply/add/abs so the
            result is bit-identical on every platform. libm's sin is not, and the
            golden-render fixtures depend on it being. */
        float cycleSin (float turns)
        {
            const float frac = turns - std::floor (turns);
            const float x = 2.0f * frac - 1.0f;              // sin(2*pi*t) = -sin(pi*x)
            float s = 4.0f * x * (1.0f - std::abs (x));      // parabola approximation
            s = 0.775f * s + 0.225f * s * std::abs (s);      // shape correction
            return -s;
        }

        /** Skeleton lanes come from the primary take, colour from a neighbour. */
        bool isSkeletonLane (int lane)
        {
            return lane == LaneKick || isSnareLane (lane) || isTomLane (lane);
        }

        int velocityBucket (int velocity)
        {
            return std::clamp (velocity * GrooveCorpus::kVelocityBuckets / 128, 0,
                               GrooveCorpus::kVelocityBuckets - 1);
        }

        constexpr int kRoundRobins = 4;

        /** How strong a beat position is: a quarter note is a landmark, a
            sixteenth off-beat is decoration. Used to decide what to drop when
            a take is busier than the pad asked for, and to accent. */
        float metricWeight (float beatInBar)
        {
            const float sixteenths = beatInBar * 4.0f;
            const float nearest    = std::round (sixteenths);
            if (std::abs (sixteenths - nearest) > 0.35f)
                return 0.25f;                       // between the sixteenths
            const int step = ((int) nearest) & 3;
            if (step == 0) return 1.0f;             // quarter
            if (step == 2) return 0.6f;             // eighth off-beat
            return 0.3f;                            // sixteenth
        }

        /** Lanes a drummer decorates with. Everything else - kick, snare,
            toms, crashes - is structure and is never thinned out. */
        bool isOrnamentLane (int lane)
        {
            return (isHatLane (lane) && lane != LaneHatPedal)
                || (isRideLane (lane) && lane != LaneRideCrash);
        }

        /** The quiet colour a drummer adds under the groove: ghost strokes on
            the snare, the stick on the rim and any hand percussion. The Ghost
            knob owns these and nothing else. */
        bool isGhostLane (int lane)
        {
            return lane == LaneSnareGhost || lane == LaneSideStick
                || lane == LaneSnareRim   || lane == LanePerc;
        }

        /** An accent cymbal: it marks a landmark in the form, so it is always
            heard exactly on the beat it marks. */
        bool isAccentCymbal (int lane)
        {
            return lane == LaneCrashL || lane == LaneCrashR || lane == LaneCrash3
                || lane == LaneChina  || lane == LaneSplash || lane == LaneRideCrash;
        }

        /** Which subdivision a position belongs to, coarsest first. Thinning a
            take walks these in order so what is left still keeps time: a
            drummer playing eighths instead of sixteenths, not sixteenths with
            holes in them. */
        int subdivisionClass (float beatInBar)
        {
            const float sixteenths = beatInBar * 4.0f;
            const float nearest    = std::round (sixteenths);
            if (std::abs (sixteenths - nearest) > 0.35f)
                return 0;                           // between the sixteenths
            const int step = ((int) nearest) & 3;
            if (step == 1 || step == 3) return 1;   // sixteenths
            if (step == 2)              return 2;   // eighth off-beats
            return 3;                               // quarters
        }

        /** Lanes struck by hand and so played with alternating sticking: the
            snare family and the toms. The feet and the time-keeping cymbal
            stay on their own limb. */
        bool isHandLane (int lane)
        {
            return isSnareLane (lane) || isTomLane (lane);
        }

        /** Where each limb sits against the click. Real players are not
            uniformly early or late: the kick leans back under the backbeat and
            the hat leads it slightly. Correlated, not noise. */
        float laneTimingBias (int lane)
        {
            if (lane == LaneKick)               return  0.006f;
            if (isSnareLane (lane))             return  0.004f;
            if (isHatLane (lane))               return -0.005f;
            if (isRideLane (lane))              return -0.003f;
            return 0.0f;
        }

        int wrap (int value, int size)
        {
            return size <= 0 ? 0 : ((value % size) + size) % size;
        }

        /** The seed a block renders from: the song seed folded with the block's
            identity, so two blocks at the same XY position still pick their own
            takes and neither changes when the other is edited. */
        std::uint64_t blockSeed (const PerformanceSettings& s)
        {
            return s.seed ^ (s.sectionSalt * 0x9E3779B97F4A7C15ull);
        }

        /** The seed the groove itself is chosen from. It deliberately does not
            depend on the bar: a drummer holds one groove for a whole section
            and lets the fills carry the song, instead of trying a different
            take every couple of bars. Only the XY position, the variation
            knobs, the seed or the block change what is played. */
        std::uint64_t grooveSeed (const PerformanceSettings& s)
        {
            return mix (blockSeed (s) ^ 0x6E9C4F1Dull);
        }

        /** The seed the song's own part is chosen from: the song, and nothing
            else. Every section of the arrangement starts from this one take, so
            an intro, a verse and a chorus are the same drummer playing the same
            song rather than three records spliced together. */
        std::uint64_t songSeed (const PerformanceSettings& s)
        {
            return mix (s.seed ^ 0x51A6D0B5F2C3ull);
        }
    }

    int arrangementBars (const std::vector<ArrangementSection>& sections)
    {
        int total = 0;
        for (const auto& sec : sections)
            total += std::max (1, sec.numBars);
        return total;
    }

    PerformanceSettings PerformanceEngine::settingsForBar (const PerformanceSettings& base,
                                                           int bar) const
    {
        if (base.arrangement.empty())
            return base;

        const int total = arrangementBars (base.arrangement);
        int pos = wrap (bar, total);

        const ArrangementSection* found = &base.arrangement.back();
        const ArrangementSection* next  = &base.arrangement.front();
        int barsInBlock = std::max (1, found->numBars);
        for (std::size_t i = 0; i < base.arrangement.size(); ++i)
        {
            const auto& sec = base.arrangement[i];
            const int n = std::max (1, sec.numBars);
            if (pos < n)
            {
                found       = &sec;
                barsInBlock = n;
                next        = &base.arrangement[(i + 1) % base.arrangement.size()];
                break;
            }
            pos -= n;
        }

        PerformanceSettings out = base;
        out.complexity      = found->complexity;
        out.intensity       = found->intensity;
        out.sectionVelocity = found->velocity;
        out.fillAmount      = found->fillAmount;
        out.swing           = found->swing;
        out.halfTime        = found->halfTime;
        out.variationRhythm = found->variationRhythm;
        out.variationCymbal = found->variationCymbal;
        out.sectionHint     = found->section;

        // Each block carries its own kit: an intro can be toms only while the
        // chorus behind it still plays everything, because the switches mask
        // this block instead of the song.
        out.laneMask     = base.laneMask     & found->laneMask;
        out.fillLaneMask = base.fillLaneMask & found->laneMask;
        out.ghostMask    = base.ghostMask    | found->ghostMask;

        // Blocks of the same kind share a groove, so the second chorus is the
        // same chorus played again rather than a different song: what makes the
        // repeat live is the fills, the round robins and the humanisation,
        // which all move with the bar.
        out.sectionSalt     = 0x9E3779B1ull * (std::uint64_t) (found->section + 1);

        // Which part is played belongs to the song, not to the block: the pad
        // decides it once and every block plays that same part, so an intro is
        // the verse held back rather than a different groove.
        out.songComplexity = base.complexity;
        out.songIntensity  = base.intensity;

        // A drummer arrives at the next section instead of stepping into it, so
        // the run-in to a block leans towards how hard that block is played: a
        // chorus is grown into over the bars before it, and the verse after one
        // is come down to rather than dropped into.
        if (next != found)
        {
            const int lead = std::min (4, barsInBlock);
            const int into = pos - (barsInBlock - lead);   // 0-based within the run-in
            if (into >= 0)
            {
                const float t = ((float) into + 1.0f) / (float) lead;
                out.sectionVelocity += 0.7f * t * (next->velocity - found->velocity);
            }
        }
        out.sectionVelocity = std::clamp (out.sectionVelocity, 0.0f, 1.0f);
        return out;
    }

    void PerformanceEngine::corpusTarget (const PerformanceSettings& s,
                                          float& complexity,
                                          float& intensity)
    {
        // The library is a practice-room recording session: its busiest bars
        // run past thirty hits, which is nobody's idea of a rock groove. The
        // pad is curved so its whole travel stays inside the range a song is
        // actually played in, and the top of the travel is a hard, busy take
        // rather than a drum solo.
        // Loudness is played, not just turned up: a section hit hard is busier
        // than the same section played quietly, so the height of the pad lifts
        // the complexity it asks the library for as well.
        const float x = std::clamp (s.complexity, 0.0f, 1.0f);
        const float y = std::clamp (s.intensity,  0.0f, 1.0f);
        complexity = std::clamp (0.05f + 0.62f * x * x + 0.15f * x + 0.16f * y * y,
                                 0.0f, 0.84f);
        intensity  = std::clamp (0.06f + 0.78f * y, 0.0f, 1.0f);
    }

    float PerformanceEngine::densityCap (const PerformanceSettings& s)
    {
        const float x = std::clamp (s.complexity, 0.0f, 1.0f);
        const float y = std::clamp (s.intensity,  0.0f, 1.0f);
        return (8.0f + 10.0f * x + 3.0f * y) * (s.halfTime ? 0.7f : 1.0f);
    }

    std::uint16_t PerformanceEngine::characterMask (const PerformanceSettings& s) const
    {
        if (corpus == nullptr || s.character < 0 || s.character >= corpus->numCharacters())
            return 0;
        return (std::uint16_t) (1u << s.character);
    }

    int PerformanceEngine::blockIndexForBar (const PerformanceSettings& s, int bar) const
    {
        if (s.arrangement.empty())
            return 0;

        int pos = wrap (bar, arrangementBars (s.arrangement));
        for (int i = 0; i < (int) s.arrangement.size(); ++i)
        {
            const int n = std::max (1, s.arrangement[(std::size_t) i].numBars);
            if (pos < n)
                return i;
            pos -= n;
        }
        return (int) s.arrangement.size() - 1;
    }

    int PerformanceEngine::sectionAtBar (const PerformanceSettings& s, int bar) const
    {
        // The host's song form wins while Follow Arrangement is on; the strip's
        // own blocks only decide the form when it is off.
        if (s.followSections && ! s.sections.empty())
        {
            for (const auto& span : s.sections)
                if (bar >= span.startBar && bar < span.startBar + span.numBars)
                    return span.section;
            return SectionVerse;
        }

        if (s.sectionHint >= 0)
            return s.sectionHint;
        if (! s.arrangement.empty())
            return settingsForBar (s, bar).sectionHint;
        return s.followSections ? SectionVerse : -1;
    }

    std::vector<int> PerformanceEngine::landingZone (const PerformanceSettings& s,
                                                     int maxResults) const
    {
        if (corpus == nullptr || ! corpus->isLoaded())
            return {};

        float cx = 0.0f, in = 0.0f;
        corpusTarget (s, cx, in);
        return corpus->neighbours (cx, in, characterMask (s),
                                   std::max (1, s.phraseBars), maxResults,
                                   s.timeSigNum, s.timeSigDen);
    }

    PerformanceEngine::Sources PerformanceEngine::pickSources (const PerformanceSettings& s,
                                                               int phraseIndex,
                                                               std::uint64_t seed) const
    {
        Sources out;
        (void) phraseIndex;   // the part is the song's, not the phrase's

        // The part is looked up at the song's pad position, never the block's.
        // A block that is quieter or busier than the song plays the same take
        // harder or softer; it does not fetch a different one, which is what
        // used to make an intro and the verse after it sound like two songs.
        PerformanceSettings song = s;
        if (s.songComplexity >= 0.0f) song.complexity = s.songComplexity;
        if (s.songIntensity  >= 0.0f) song.intensity  = s.songIntensity;

        float cx = 0.0f, in = 0.0f;
        corpusTarget (song, cx, in);
        // A wide shortlist so the heavy corner of the pad has real dense takes
        // to draw from, not only whatever happens to sit nearest to it.
        const auto ranked = corpus->neighbours (cx, in, characterMask (song),
                                                std::max (1, s.phraseBars), 24,
                                                s.timeSigNum, s.timeSigDen);
        if (ranked.empty())
            return out;

        // Sparse takes are not what the pad was asked for once it is up in the
        // heavy corner, so they are passed over while enough real candidates
        // remain to still have a choice.
        const auto perBar = [this] (int i)
        {
            const auto& p = corpus->beat (i);
            return (float) p.hits.size() / (float) std::max<int> (1, p.bars);
        };

        std::vector<int> dense;
        {
            const float floorPerBar = 0.55f * densityCap (song);
            for (const int i : ranked)
                if (perBar (i) >= floorPerBar)
                    dense.push_back (i);

            // Nothing nearby is busy enough for where the pad is: rather than
            // fall back on a thin take, take the busiest real performances the
            // shortlist has.
            if (dense.size() < 4)
            {
                dense = ranked;
                std::stable_sort (dense.begin(), dense.end(),
                                  [&] (int a, int b)
                                  {
                                      const float pa = perBar (a), pb = perBar (b);
                                      return pa > pb || (! (pb > pa) && a < b);
                                  });
                dense.resize (std::min<std::size_t> (6, dense.size()));
            }
        }
        const auto& pool = dense.empty() ? ranked : dense;

        // Seeded weighted choice over the k nearest takes: the closest are the
        // most likely, but a held XY position still breathes between real
        // performances instead of repeating one.
        const int n = std::min (12, (int) pool.size());
        float weights[12] {};
        float total = 0.0f;
        // Deliberately blind to which section is playing: a take tagged
        // "chorus" is not a different song from the one tagged "verse", and
        // preferring it per block is what used to swap the part underneath the
        // listener at every section boundary.
        for (int i = 0; i < n; ++i)
        {
            const float w = 1.0f / (1.0f + (float) i * 0.55f);
            weights[i] = w;
            total += w;
        }

        const auto drawFrom = [&] (std::uint64_t from, std::uint64_t salt)
        {
            float r = rand01 (from, salt) * total;
            for (int i = 0; i < n; ++i)
            {
                r -= weights[i];
                if (r <= 0.0f)
                    return i;
            }
            return n - 1;
        };

        const auto draw = [&] (std::uint64_t salt) { return drawFrom (seed, salt); };

        // The song's part comes from the song, not from the section: that is
        // what stops an intro, a verse and a chorus each setting off on their
        // own. A section may then step to a neighbouring take - the same
        // drummer playing the same part again - but only to one close enough
        // to still be the same groove.
        // One take now plays the whole song, so it has to be the take that
        // matches the pad rather than a lucky draw near it: the shortlist is
        // ordered by how close its density is to what the pad asked for, and
        // the song picks between the best couple of those.
        const int shortlist = std::min (n, 4);
        int order[4] { 0, 1, 2, 3 };
        {
            const float wanted = 0.9f * densityCap (song);
            std::stable_sort (order, order + shortlist,
                              [&] (int a, int b)
                              {
                                  const float da = std::abs (perBar (pool[(std::size_t) a]) - wanted);
                                  const float db = std::abs (perBar (pool[(std::size_t) b]) - wanted);
                                  return da < db;
                              });
        }
        const int choices = std::min (shortlist, 2);
        int primary = order[wrap ((int) (rand01 (songSeed (s), 0x5Bu) * (float) choices)
                                  + s.variationRhythm, choices)];
        {
            const auto& part  = corpus->beat (pool[(std::size_t) primary]);
            // The step to a neighbouring take is drawn from the song too, so it
            // is the song that has a slightly different favourite - not each
            // block picking its own.
            const int   start = wrap (primary + 1
                                      + (int) (rand01 (songSeed (s), 0x9Du) * 3.0f), n);
            for (int step = 0; step < n; ++step)
            {
                const int i = wrap (start + step, n);
                if (i == primary)
                    break;

                const auto& other = corpus->beat (pool[(std::size_t) i]);
                if (other.bars != part.bars || other.sigNum != part.sigNum
                    || other.sigDen != part.sigDen
                    || (other.charMask & part.charMask) == 0)
                    continue;
                if (std::abs (other.complexity - part.complexity) > 0.10f
                    || std::abs (other.swing - part.swing) > 0.10f)
                    continue;
                // And it has to be as good a match for where the pad is sitting
                // as the song's own take, or the section would drift away from
                // what was asked for.
                if (weights[i] < weights[primary] * 0.9f)
                    continue;

                primary = i;
                break;
            }
        }

        const auto& take = corpus->beat (pool[(std::size_t) primary]);

        // One human take plays the whole bar. Cymbals only come from a
        // different take when the user asks for a cymbal variation, and even
        // then only from a take close enough that the two were playing the
        // same kind of music - otherwise the bar stops sounding like one
        // person and starts sounding like two records at once.
        out.skeleton = &take;
        out.colour   = &take;

        if (s.variationCymbal != 0 && n > 1)
        {
            const int start = wrap (draw (0xC010u) + s.variationCymbal, n);
            for (int step = 0; step < n; ++step)
            {
                const int i = wrap (start + step, n);
                if (i == primary)
                    continue;

                const auto& other = corpus->beat (pool[(std::size_t) i]);
                if (other.bars != take.bars || other.sigNum != take.sigNum
                    || other.sigDen != take.sigDen)
                    continue;
                if (std::abs (other.complexity - take.complexity) > 0.15f
                    || std::abs (other.intensity - take.intensity) > 0.20f
                    || std::abs (other.swing - take.swing) > 0.15f)
                    continue;

                out.colour = &other;
                break;
            }
        }

        return out;
    }

    bool PerformanceEngine::fillIsPlayable (const PerformanceSettings& s,
                                            const Phrase& f,
                                            float scale,
                                            bool  strict)
    {
        // Every stroke, in destination beats and then in seconds, because a
        // fill is only good or bad at a tempo: the same written figure is a
        // roll at 150 and a bar of slow flams at 70.
        std::vector<float> beats;
        beats.reserve (f.hits.size());
        for (const auto& h : f.hits)
            beats.push_back (h.gridBeat() * scale);
        if (beats.size() < (strict ? 4u : 3u))
            return false;

        std::sort (beats.begin(), beats.end());

        const float secPerBeat = 60.0f / std::max (20.0f, s.tempoBpm);
        std::vector<float> gaps;
        gaps.reserve (beats.size());
        for (std::size_t i = 1; i < beats.size(); ++i)
        {
            const float d = beats[i] - beats[i - 1];
            if (d > 0.01f)                     // simultaneous strokes are one stroke
                gaps.push_back (d);
        }
        if (gaps.size() < 2)
            return false;

        const float fastest = *std::min_element (gaps.begin(), gaps.end());
        std::vector<float> sorted = gaps;
        std::sort (sorted.begin(), sorted.end());
        const float typical = sorted[sorted.size() / 2];

        // Two hands cannot play faster than about twenty strokes a second, and
        // a figure whose strokes are further apart than a slow eighth is not a
        // fill - it is scattered hits that never gather into one gesture.
        if (fastest * secPerBeat < 0.048f)
            return false;
        if (strict && typical * secPerBeat > 0.46f)
            return false;

        // Half time is played half as busy: at this feel the groove is moving
        // at half rate, so a fill of thirty-second notes over it reads as a
        // different drummer barging in.
        if (strict && s.halfTime && fastest < 0.22f)
            return false;

        // And it has to be continuous. A modern rock fill is one unbroken run
        // into the downbeat, so a figure with a hole in the middle of it - two
        // strokes, a rest of half a bar, two more - is not played, however well
        // it reads on paper.
        const float widest = *std::max_element (gaps.begin(), gaps.end());
        if (widest > (s.halfTime ? 1.05f : 0.55f) * (strict ? 1.0f : 1.6f))
            return false;

        // And it has to resolve: the figure must run right up to the downbeat
        // it is handing over to, rather than stopping short and leaving a hole.
        const float span = f.sourceBeatsPerBar() * (float) std::max (1, f.bars) * scale;
        return beats.back() >= span - (strict ? 0.55f : 1.05f);
    }

    int PerformanceEngine::fillIndexForPhrase (const PerformanceSettings& s,
                                               int phraseIndex) const
    {
        const int  bars    = (s.fillLengthBars >= 1.5f) ? 2 : 1;
        const auto pick    = [&] (int idx, const std::vector<int>& avoid)
        {
            const std::uint64_t seed = mix (blockSeed (s) ^ mix ((std::uint64_t) idx + 1));
            return corpus->pickFill (s.fillComplexity, s.intensity, bars,
                                     (int) (mix (seed ^ 0xF111ull) % 16u), avoid,
                                     s.fillLaneMask & s.laneMask, s.fillStyleMask,
                                     s.timeSigNum, s.timeSigDen,
                                     characterMask (s));
        };

        // Recently-used ring buffer: the last few fills are excluded so the
        // same one never lands twice in a row.
        // Only fills inside the same arrangement block count, so what a block
        // plays never depends on the block before it.
        const int bars_ = std::max (1, s.phraseBars);
        const int block = blockIndexForBar (s, phraseIndex * bars_);

        std::vector<int> avoid;
        for (int q = std::max (0, phraseIndex - 4); q < phraseIndex; ++q)
            if (blockIndexForBar (s, q * bars_) == block && phraseEndsWithFill (s, q))
                if (const int i = pick (q, {}); i >= 0)
                    avoid.push_back (i);

        // The nearest fill to the pad position is only played if it is playable
        // here; otherwise it is rejected and the search moves on, so a bad take
        // is never heard rather than being smoothed over afterwards.
        const float dstBar = std::max (1.0f, s.beatsPerBar);
        std::vector<int> seen;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const int i = pick (phraseIndex, avoid);
            if (i < 0)
                break;

            const auto& f = corpus->fill (i);
            if (fillIsPlayable (s, f, dstBar / f.sourceBeatsPerBar(), true))
                return i;

            seen.push_back (i);
            avoid.push_back (i);
        }

        // Nothing in the corpus reads well at this tempo and feel. The second
        // pass only asks that the figure be physically playable and land near
        // the downbeat; if even that fails the phrase keeps its groove through,
        // which is what a drummer does rather than play a fill that fights the
        // song.
        for (const int i : seen)
        {
            const auto& f = corpus->fill (i);
            if (fillIsPlayable (s, f, dstBar / f.sourceBeatsPerBar(), false))
                return i;
        }
        return -1;
    }

    void PerformanceEngine::appendFill (const PerformanceSettings& s,
                                        int   phraseIndex,
                                        float fillStartBeat,
                                        float phraseBeats,
                                        std::uint64_t seed,
                                        std::vector<Raw>& raw) const
    {
        const int fillIdx = fillIndexForPhrase (s, phraseIndex);
        if (fillIdx < 0)
            return;

        const auto& f         = corpus->fill (fillIdx);
        const float dstBar    = std::max (1.0f, s.beatsPerBar);
        const float kSourceBar = f.sourceBeatsPerBar();
        const bool  halfFill = s.fillLengthBars < 0.75f;
        // A half-bar fill is the tail of a one-bar fill, which is how it is
        // actually played, rather than a squashed whole fill.
        const float srcSkip  = halfFill ? kSourceBar * 0.5f : 0.0f;
        const float scale    = dstBar / kSourceBar;

        // Where every stroke of the take lands once it has been stretched, so a
        // stroke can be told whether it is part of a run or standing on its own.
        std::vector<float> placed;
        placed.reserve (f.hits.size());
        for (const auto& h : f.hits)
            if (h.gridBeat() >= srcSkip)
                placed.push_back ((h.gridBeat() - srcSkip) * scale);
        std::sort (placed.begin(), placed.end());

        const auto inARun = [&placed] (float rel)
        {
            // A neighbour within a sixteenth means the stroke is one of a run,
            // where the drummer's own push and drag is the point of the take.
            for (const float other : placed)
                if (std::abs (other - rel) > 0.01f && std::abs (other - rel) < 0.26f)
                    return true;
            return false;
        };

        for (const auto& h : f.hits)
        {
            const float src = h.gridBeat();
            if (src < srcSkip)
                continue;

            // A fill has to turn the corner with the bar, so it is played to
            // the grid: stretching a source phrase into another bar length
            // lands its hits between subdivisions, and those are pulled back
            // onto the nearest subdivision the take was played at, a thirty
            // second or a triplet sixteenth, so a roll keeps all its strokes.
            // The take's own micro-timing then rides on top, but only as far
            // as Humanize asks.
            const float rel  = (src - srcSkip) * scale;
            const float q32  = std::round (rel * 8.0f) / 8.0f;
            const float q24  = std::round (rel * 6.0f) / 6.0f;
            const float snap = std::abs (rel - q32) <= std::abs (rel - q24) ? q32 : q24;

            // An isolated stroke is played dead on the grid. Micro-timing is
            // heard as feel inside a run, but a lone hit nudged off the beat is
            // heard as a mistake - which is what a one-note fill with the
            // drummer's deviation on it sounds like.
            // Micro-timing inside a fill is a fraction of what it is under a
            // groove: a run of sixteenths is where a shifted stroke is heard
            // most plainly, so the deviation is scaled right down and capped at
            // well under a thirty-second either way.
            const float dev = inARun (rel)
                            ? std::clamp (h.devBeats() * scale * 0.08f
                                              * std::clamp (s.humanize, 0.0f, 1.0f),
                                          -0.03f, 0.03f)
                            : 0.0f;
            const float beat = fillStartBeat + snap + dev;
            if (beat < fillStartBeat - 0.02f || beat >= phraseBeats - 0.005f)
                continue;

            float vel = (float) h.velocity;
            vel *= 1.0f + (rand01 (seed, (std::uint64_t) (src * 64.0f), 0x1Eu) - 0.5f)
                          * 2.0f * s.fillVelVar * 0.4f;

            // A fill is one gesture that hands the song over, so it is played
            // into the downbeat: it starts under the groove and arrives above
            // it. Played flat, a fill reads as a burst of notes rather than as
            // something a drummer meant.
            const float through = std::clamp ((beat - fillStartBeat)
                                                  / std::max (0.25f, phraseBeats - fillStartBeat),
                                              0.0f, 1.0f);
            vel *= 0.86f + 0.26f * through;
            const Raw hit { beat, 0.0f, h.lane,
                            (std::uint8_t) std::clamp ((int) std::lround (vel), 1, 127),
                            true };

            // A stretched fill can land two strokes of the same drum on one
            // subdivision, which reads as one hit struck twice as hard rather
            // than as two strokes, so the louder of the pair is kept.
            const auto same = std::find_if (raw.begin(), raw.end(), [&] (const Raw& r)
            {
                return r.lane == hit.lane && std::abs (r.beat - hit.beat) < 0.004f;
            });
            if (same != raw.end())
            {
                same->velocity = std::max (same->velocity, hit.velocity);
                continue;
            }
            raw.push_back (hit);
        }

        // At half time the groove moves at half rate, so the fill is played at
        // half rate too: strokes closer together than a sixteenth of the
        // half-time pulse are thinned to the louder one, which is the figure a
        // drummer plays over a half-time groove rather than a roll barging in.
        if (s.halfTime)
        {
            std::stable_sort (raw.begin(), raw.end(),
                              [] (const Raw& a, const Raw& b) { return a.beat < b.beat; });
            std::vector<Raw> kept;
            kept.reserve (raw.size());
            float lastBeat = -10.0f;
            for (const auto& r : raw)
            {
                if (! r.fill)
                {
                    kept.push_back (r);
                    continue;
                }
                if (r.beat - lastBeat < 0.2f && std::abs (r.beat - lastBeat) > 0.01f)
                    continue;
                lastBeat = r.beat;
                kept.push_back (r);
            }
            raw.swap (kept);
        }

        // What is left of the figure once the tempo, the feel and the block's
        // piece switches have had their say. One or two strokes is not a simple
        // fill, it is a hole with a hit in it, so a plain figure is played
        // instead of the take.
        {
            std::vector<float> beats;
            for (const auto& r : raw)
                if (r.fill && (s.laneMask & (1u << r.lane)) != 0)
                    beats.push_back (r.beat);
            std::sort (beats.begin(), beats.end());

            int   strokes = 0;
            float last    = -10.0f;
            float widest  = 0.0f;
            for (const float b : beats)
                if (b - last > 0.01f)
                {
                    if (strokes > 0)
                        widest = std::max (widest, b - last);
                    ++strokes;
                    last = b;
                }

            // A hole inside the figure, or a figure that stops short of the
            // downbeat, is the gap that reads as the drummer losing the thread,
            // so the plain continuous figure is played instead.
            const bool broken = widest > (s.halfTime ? 1.05f : 0.55f)
                                || (strokes > 0
                                    && last < phraseBeats - (s.halfTime ? 1.05f : 0.55f));

            if (strokes < 4 || broken)
            {
                raw.erase (std::remove_if (raw.begin(), raw.end(),
                                           [] (const Raw& r) { return r.fill; }),
                           raw.end());
                appendSimpleFill (s, fillStartBeat, phraseBeats, raw);
            }
        }

        // A drummer riding the hats does not take the foot off them to play a
        // fill: the hat keeps the quarter under the figure. Only the quarters
        // no cymbal already covers are added, so the fill keeps its shape and
        // the openness knob is heard through it as well as under the groove.
        if (s.hatOpenness > 0.15f)
        {
            for (float q = std::ceil (fillStartBeat); q < phraseBeats - 0.005f; q += 1.0f)
            {
                const bool covered = std::any_of (raw.begin(), raw.end(), [&] (const Raw& r)
                {
                    return std::abs (r.beat - q) < 0.12f
                           && (isHatLane (r.lane) || isCymbalLane (r.lane));
                });
                if (covered)
                    continue;

                raw.push_back (Raw { q, 0.0f, (std::uint8_t) LaneHatClosed,
                                     (std::uint8_t) 78, true });
            }
        }
    }

    void PerformanceEngine::appendSimpleFill (const PerformanceSettings& s,
                                              float fillStartBeat,
                                              float phraseBeats,
                                              std::vector<Raw>& raw) const
    {
        const float window = phraseBeats - fillStartBeat;
        if (window < 0.4f)
            return;

        const auto on = [&s] (int lane) { return (s.laneMask & (1u << lane)) != 0; };
        const auto firstOn = [&on] (std::initializer_list<int> preference)
        {
            for (const int lane : preference)
                if (on (lane))
                    return lane;
            return -1;
        };

        // What the figure is played on: the snare and the toms down the kit if
        // they are in front of him, otherwise whatever the block left switched
        // in, so a toms-only intro still turns the corner.
        const int hand = firstOn ({ LaneSnare, LaneTom1, LaneTom2, LaneSnareRim,
                                    LaneTom3, LaneTom4, LaneKick });
        if (hand < 0)
            return;

        std::vector<int> down;
        for (const int lane : { LaneTom1, LaneTom2, LaneTom3, LaneTom4 })
            if (on (lane))
                down.push_back (lane);

        // A sixteenth-note figure, unless the tempo makes sixteenths a roll: at
        // that point the same figure is played in eighths, which is how a
        // drummer plays it rather than doubling his hands to fit the grid.
        const float secPerBeat = 60.0f / std::max (20.0f, s.tempoBpm);
        const float step = (0.25f * secPerBeat >= 0.105f && ! s.halfTime) ? 0.25f : 0.5f;
        const int   n    = (int) std::floor (window / step + 0.001f);
        if (n < 3)
            return;

        // With no snare in front of him the figure is played as a tom-and-kick
        // conversation rather than as a row of toms, which is both what a
        // drummer does and what keeps a switched-off snare from being quietly
        // re-voiced onto the toms.
        const bool tomLed = ! on (LaneSnare) && ! on (LaneSnareRim)
                            && ! down.empty() && on (LaneKick);

        for (int i = 0; i < n; ++i)
        {
            // Snare on the front of the figure, then down the toms into the
            // downbeat, and the kick under the last stroke so the handover has
            // weight. Plain, on the grid, and it resolves - which is all a
            // simple fill has to do.
            const float where = (float) i / (float) n;
            int lane = hand;
            if (! down.empty() && where >= 0.5f)
            {
                const int rung = (int) ((where - 0.5f) * 2.0f * (float) down.size());
                lane = down[(std::size_t) std::min ((int) down.size() - 1, rung)];
            }
            if (tomLed && (i % 2) == 0 && i < n - 1)
                lane = LaneKick;

            const int vel = (int) std::lround (86.0f + 36.0f * where);
            raw.push_back (Raw { fillStartBeat + step * (float) i, 0.0f,
                                 (std::uint8_t) lane,
                                 (std::uint8_t) std::clamp (vel, 1, 127), true });
        }

        if (on (LaneKick) && n >= 3)
            raw.push_back (Raw { fillStartBeat + step * (float) (n - 1), 0.0f,
                                 (std::uint8_t) LaneKick, (std::uint8_t) 116, true });
    }

    void PerformanceEngine::thinOrnaments (const PerformanceSettings& s,
                                           float dstBar,
                                           int   bars,
                                           std::vector<Raw>& raw) const
    {
        const int limit = (int) std::lround (densityCap (s) * (float) bars);
        if ((int) raw.size() <= limit)
            return;

        // Thin the decoration a whole subdivision at a time, and evenly within
        // one, so what is left still keeps time. Dropping the quietest strokes
        // wherever they happen to be - which is what this used to do - leaves
        // sixteenths in clusters with half-bar holes between them, and that is
        // what reads as boxy rather than as a drummer easing off.
        std::vector<bool> remove (raw.size(), false);
        int needed = (int) raw.size() - limit;

        for (int layer = 0; layer < 3 && needed > 0; ++layer)
        {
            std::vector<std::size_t> inLayer;
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (! isOrnamentLane (raw[i].lane))
                    continue;
                const float inBar = raw[i].beat - std::floor (raw[i].beat / dstBar) * dstBar;
                if (subdivisionClass (inBar) == layer)
                    inLayer.push_back (i);
            }
            if (inLayer.empty())
                continue;

            std::sort (inLayer.begin(), inLayer.end(),
                       [&] (std::size_t a, std::size_t b) { return raw[a].beat < raw[b].beat; });

            if ((int) inLayer.size() <= needed)
            {
                for (const std::size_t i : inLayer)
                    remove[i] = true;
                needed -= (int) inLayer.size();
                continue;
            }

            // Part of a layer goes: take them at an even stride so the strokes
            // that stay are still periodic.
            const double stride = (double) inLayer.size() / (double) needed;
            for (int k = 0; k < needed; ++k)
                remove[inLayer[(std::size_t) ((double) k * stride)]] = true;
            needed = 0;
        }

        std::size_t write = 0;
        for (std::size_t i = 0; i < raw.size(); ++i)
            if (! remove[i])
                raw[write++] = raw[i];
        raw.resize (write);
    }

    void PerformanceEngine::supplyLanes (const PerformanceSettings& s,
                                         float dstBar,
                                         int   bars,
                                         std::uint64_t seed,
                                         std::vector<Raw>& raw) const
    {
        const auto enabled = [&s] (int lane) { return (s.laneMask & (1u << lane)) != 0; };
        const auto playing = [&] (auto&& predicate)
        {
            // A lane the player has switched out is not keeping time either.
            return std::any_of (raw.begin(), raw.end(),
                                [&] (const Raw& h)
                                {
                                    return enabled ((int) h.lane)
                                        && predicate ((int) h.lane);
                                });
        };

        // A time keeper the whole phrase can lean on: eighths, quarters when
        // the section is half-time, accented on the beat the way a drummer
        // plays them rather than flat.
        const auto layTimeKeeper = [&] (int lane, float step, float level)
        {
            const float total = dstBar * (float) bars;
            for (float beat = 0.0f; beat < total - 0.001f; beat += step)
            {
                const float inBar = beat - std::floor (beat / dstBar) * dstBar;
                const bool  onBeat = std::abs (inBar - std::round (inBar)) < 0.01f;
                const float vel = level * (onBeat ? 1.0f : 0.72f)
                                * (1.0f + 0.05f * (rand01 (seed, (std::uint64_t) (beat * 16.0f), 0x5Du) - 0.5f));
                raw.push_back ({ beat, 0.0f, (std::uint8_t) lane,
                                 (std::uint8_t) std::clamp ((int) std::lround (vel * 127.0f), 1, 127) });
            }
        };

        const float step  = (s.halfTime ? 1.0f : 0.5f);
        const float level = 0.45f + 0.28f * std::clamp (s.sectionVelocity, 0.0f, 1.0f);

        const bool hatsOn = enabled (LaneHatClosed);
        const bool rideOn = enabled (LaneRideBow);

        // A take whose only cymbal work is a foot splash or a crashed ride is
        // not keeping time on anything, so time is laid for it.
        if (hatsOn && ! playing (isOrnamentLane))
            layTimeKeeper (LaneHatClosed, step, level);
        else if (rideOn && ! hatsOn && ! playing (isOrnamentLane))
            layTimeKeeper (LaneRideBow, step, level);

        // The time keeper has to be continuous. A take that plays sixteenths
        // for a beat and a half and then leaves half a bar empty is not a
        // drummer easing off, it is a hole, and holes are the single biggest
        // reason a groove reads as one-shots stitched together. Whatever the
        // take is keeping time on, the grid it started is carried through.
        {
            int counts[NumLanes] = {};
            for (const Raw& h : raw)
                if (isOrnamentLane ((int) h.lane))
                    ++counts[h.lane];

            int timeLane = -1;
            for (int lane = 0; lane < NumLanes; ++lane)
                if (counts[lane] > 0 && (timeLane < 0 || counts[lane] > counts[timeLane]))
                    timeLane = lane;

            if (timeLane >= 0 && enabled (timeLane))
            {
                std::vector<float> beats;
                for (const Raw& h : raw)
                    if ((int) h.lane == timeLane)
                        beats.push_back (h.beat);
                std::sort (beats.begin(), beats.end());

                std::vector<float> gaps;
                for (std::size_t i = 1; i < beats.size(); ++i)
                    if (const float g = beats[i] - beats[i - 1]; g > 0.02f)
                        gaps.push_back (g);
                std::sort (gaps.begin(), gaps.end());

                {
                    // The subdivision the take is actually playing, read off its
                    // own strokes rather than assumed - then coarsened until the
                    // whole bar fits inside what the pad asked for. A hat part
                    // is either sixteenths or eighths; it is never sixteenths
                    // with a beat and a half missing out of the middle.
                    const float median = gaps.empty() ? step : gaps[gaps.size() / 2];
                    float grid = 0.5f;
                    for (const float candidate : { 0.25f, 0.5f, 1.0f })
                        if (std::abs (median - candidate) < std::abs (median - grid))
                            grid = candidate;
                    // Time is sixteenths or eighths. A stroke a beat is not time
                    // being kept, it is the holes the take had left in.
                    grid = std::clamp (grid, 0.25f, 0.5f);
                    if (s.halfTime)
                        grid = 0.5f;

                    // How busy the time may be is the pad's complexity axis
                    // only: Intensity decides how hard the kit is hit, never
                    // which strokes are played.
                    const float total = dstBar * (float) bars;
                    const float perBar = (8.0f + 10.0f * std::clamp (s.complexity, 0.0f, 1.0f))
                                       * (s.halfTime ? 0.7f : 1.0f);
                    const int   limit = (int) std::lround (perBar * (float) bars);
                    int structure = 0;
                    for (const Raw& h : raw)
                        if ((int) h.lane != timeLane)
                            ++structure;
                    // Eighths are the floor: a drummer thins sixteenths down to
                    // eighths, never down to a stroke a beat, which is where a
                    // groove stops keeping time at all.
                    while (grid < 0.5f
                           && (float) structure + total / grid > (float) limit)
                        grid *= 2.0f;

                    // The take's own dynamic on this lane, so the strokes that
                    // are filled in belong to the same performance.
                    std::vector<int> vels;
                    for (const Raw& h : raw)
                        if ((int) h.lane == timeLane)
                            vels.push_back ((int) h.velocity);
                    std::sort (vels.begin(), vels.end());
                    const float own = vels.empty() ? level
                                                   : (float) vels[vels.size() / 2] / 127.0f;
                    const float base = 0.5f * own + 0.5f * level;

                    // Anything this lane plays away from the grid it is keeping
                    // is clutter, not time: it is pulled onto the grid instead
                    // of sounding like a stray stick.
                    for (Raw& h : raw)
                        if ((int) h.lane == timeLane)
                        {
                            h.beat = std::round (h.beat / grid) * grid;
                            h.dev  = 0.0f;
                        }

                    std::vector<Raw> added;
                    for (float beat = 0.0f; beat < total - 0.001f; beat += grid)
                    {
                        // A crash covers the time it lands on: the hand is over
                        // there, so no hat is added under it.
                        const bool covered = std::any_of (raw.begin(), raw.end(),
                            [&] (const Raw& h)
                            {
                                return (isOrnamentLane ((int) h.lane) || isAccentCymbal ((int) h.lane))
                                    && std::abs (h.beat - beat) < grid * 0.45f;
                            });
                        if (covered)
                            continue;

                        const float inBar  = beat - std::floor (beat / dstBar) * dstBar;
                        const bool  onBeat = std::abs (inBar - std::round (inBar)) < 0.01f;
                        added.push_back ({ beat, 0.0f, (std::uint8_t) timeLane,
                                           (std::uint8_t) std::clamp (
                                               (int) std::lround (base * (onBeat ? 1.0f : 0.78f) * 127.0f),
                                               1, 127) });
                    }
                    raw.insert (raw.end(), added.begin(), added.end());

                    // Two strokes on one subdivision are one stroke.
                    std::stable_sort (raw.begin(), raw.end(),
                                      [] (const Raw& a, const Raw& b)
                                      {
                                          if (a.lane != b.lane) return a.lane < b.lane;
                                          return a.beat < b.beat;
                                      });
                    std::vector<Raw> merged;
                    merged.reserve (raw.size());
                    for (const Raw& h : raw)
                    {
                        if (! merged.empty() && merged.back().lane == h.lane
                            && std::abs (merged.back().beat - h.beat) < 0.01f)
                        {
                            merged.back().velocity = std::max (merged.back().velocity, h.velocity);
                            continue;
                        }
                        merged.push_back (h);
                    }
                    raw.swap (merged);
                }
            }
        }

        // The backbeat is the groove. A take that has drifted out of the corpus
        // without a snare on two and four does not read as a rock beat at all,
        // so the missing stroke is played rather than left as a hole.
        if (enabled (LaneSnare) && std::abs (s.beatsPerBar - 4.0f) < 0.01f && s.timeSigDen == 4)
        {
            for (int bar = 0; bar < bars; ++bar)
            {
                const float top = (float) bar * dstBar;
                const std::vector<float> spots = s.halfTime
                                               ? std::vector<float> { top + 2.0f }
                                               : std::vector<float> { top + 1.0f, top + 3.0f };
                for (const float beat : spots)
                {
                    const bool taken = std::any_of (raw.begin(), raw.end(),
                        [&] (const Raw& h)
                        {
                            // Only a snare counts as the backbeat: a tom or a
                            // ghost stroke on two does not hold a groove up.
                            return (h.lane == LaneSnare || h.lane == LaneSnareRim
                                    || h.lane == LaneSnareFlam || h.lane == LaneSnareRoll)
                                && std::abs (h.beat - beat) < 0.13f;
                        });
                    if (taken)
                        continue;
                    raw.push_back ({ beat, 0.0f, (std::uint8_t) LaneSnare,
                                     (std::uint8_t) std::clamp (
                                         (int) std::lround ((0.62f + 0.30f * std::clamp (s.sectionVelocity, 0.0f, 1.0f))
                                                            * 127.0f), 1, 127) });
                }
            }
        }

        // Up in the heavy corner of the pad a drummer drives the time instead
        // of counting eighths. If the take is playing less than the pad asked
        // for, the sixteenths between its existing strokes are filled in on
        // whichever cymbal is already keeping time - the take's own part, just
        // played harder - rather than reaching for a busier, unrelated take.
        if (! s.halfTime)
        {
            const float wanted = densityCap (s) * (float) bars * 0.9f;
            if ((float) raw.size() < wanted)
            {
                int timeLane = -1;
                for (const Raw& h : raw)
                    if (isHatLane ((int) h.lane) || isRideLane ((int) h.lane))
                    {
                        timeLane = (int) h.lane;
                        break;
                    }

                if (timeLane >= 0 && enabled (timeLane))
                {
                    std::vector<float> beats;
                    for (const Raw& h : raw)
                        if ((int) h.lane == timeLane)
                            beats.push_back (h.beat);
                    std::sort (beats.begin(), beats.end());

                    const float total = dstBar * (float) bars;
                    for (std::size_t i = 0; i + 1 < beats.size(); ++i)
                    {
                        const float gap = beats[i + 1] - beats[i];
                        if (gap < 0.49f || gap > 0.51f)   // only eighth-note time
                            continue;
                        const float mid = beats[i] + gap * 0.5f;
                        if (mid >= total - 0.001f || (float) raw.size() >= wanted)
                            continue;
                        raw.push_back ({ mid, 0.0f, (std::uint8_t) timeLane,
                                         (std::uint8_t) std::clamp (
                                             (int) std::lround (level * 0.6f * 127.0f), 1, 127) });
                    }
                }
            }
        }
    }

    void PerformanceEngine::addGhostNotes (const PerformanceSettings& s,
                                           float dstBar,
                                           int   bars,
                                           std::uint64_t seed,
                                           std::vector<Raw>& raw) const
    {
        if ((s.laneMask & (1u << LaneSnareGhost)) == 0)
            return;

        // Below the middle of the knob the take's own ghost strokes are the
        // performance; above it the player is asked for more of them, so the
        // idiomatic placements are filled in - the sixteenth leading into the
        // backbeat first, then the one leading into the beat before it.
        const float amount = std::clamp (s.ghostAmount, 0.0f, 1.0f);
        if (amount <= 0.55f)
            return;

        const float chance = (amount - 0.55f) / 0.45f;
        const float lead   = 0.25f;

        std::vector<float> spots;
        for (int bar = 0; bar < bars; ++bar)
        {
            const float top = (float) bar * dstBar;
            spots.push_back (top + dstBar * 0.5f - lead);
            spots.push_back (top + dstBar - lead);
            if (amount > 0.8f)
            {
                spots.push_back (top + dstBar * 0.25f - lead);
                spots.push_back (top + dstBar * 0.75f - lead);
            }
        }

        for (const float beat : spots)
        {
            if (beat < lead)
                continue;
            if (rand01 (seed, (std::uint64_t) (beat * 32.0f), 0x6Bu) > chance)
                continue;

            // Never in the way of a stroke the take already plays: a ghost is
            // what fits between the hands, not a second snare hit.
            const bool taken = std::any_of (raw.begin(), raw.end(),
                                            [beat] (const Raw& h)
                                            {
                                                return (isSnareLane (h.lane) || h.lane == LaneKick)
                                                    && std::abs (h.beat - beat) < 0.12f;
                                            });
            if (taken)
                continue;

            const float level = 0.24f + 0.10f * amount;
            raw.push_back ({ beat, 0.0f, (std::uint8_t) LaneSnareGhost,
                             (std::uint8_t) std::clamp ((int) std::lround (level * 127.0f), 1, 127) });
        }
    }

    void PerformanceEngine::addCrashes (const PerformanceSettings& base,
                                        const PerformanceSettings& sec,
                                        int   phraseIndex,
                                        int   bars,
                                        float dstBar,
                                        std::uint64_t seed,
                                        std::vector<Hit>& out) const
    {
        const bool haveRight = (sec.laneMask & (1u << LaneCrashR)) != 0;
        const bool haveLeft  = (sec.laneMask & (1u << LaneCrashL)) != 0;
        if (! haveRight && ! haveLeft)
            return;

        const float energy  = std::clamp (sec.sectionVelocity, 0.0f, 1.0f);
        const int   section = sectionAtBar (base, phraseIndex * bars);
        const bool  blockTop = phraseIndex == 0
                             || blockIndexForBar (base, phraseIndex * bars)
                                    != blockIndexForBar (base, (phraseIndex - 1) * bars);
        const bool  afterFill = phraseIndex > 0 && phraseEndsWithFill (base, phraseIndex - 1);

        std::vector<float> beats;

        // The top of the phrase: always after a fill, always at the top of a
        // section, and from moderate energy up, every phrase. A drummer marks
        // the downbeat he has just filled into whatever the genre is, so this
        // is deliberately easy to reach rather than a metal-only behaviour.
        if (afterFill || blockTop || energy > 0.42f || section == SectionChorus)
            beats.push_back (0.0f);

        // Sections played up mark the half-way bar as well, which is what makes
        // a chorus sound like a chorus instead of a louder verse.
        if (energy > 0.55f && bars >= 2)
            beats.push_back ((float) (bars / 2) * dstBar);

        // A big chorus: every bar gets marked.
        if (energy > 0.7f)
            for (int bar = 0; bar < bars; ++bar)
                beats.push_back ((float) bar * dstBar);

        // Flat out, the halves of every bar are marked too - the wall of
        // crashes a heavy chorus is actually played with, alternating left and
        // right so it stays playable.
        if (energy > 0.84f)
            for (int bar = 0; bar < bars; ++bar)
                beats.push_back (((float) bar + 0.5f) * dstBar);

        std::sort (beats.begin(), beats.end());
        beats.erase (std::unique (beats.begin(), beats.end()), beats.end());

        int index = 0;
        std::vector<float> struck;
        for (const float beat : beats)
        {
            const bool taken = std::any_of (out.begin(), out.end(),
                                            [beat] (const Hit& h)
                                            {
                                                return isCymbalLane (h.lane)
                                                    && ! isHatLane (h.lane)
                                                    && std::abs (h.beat - beat) < 0.12f;
                                            });
            if (taken)
            {
                ++index;
                continue;
            }

            const bool useRight = haveRight && (! haveLeft || (index % 2) == 0);
            const int  lane     = useRight ? LaneCrashR : LaneCrashL;
            const float level   = 0.62f + 0.30f * energy
                                + 0.05f * (rand01 (seed, (std::uint64_t) index, 0xC7u) - 0.5f);

            out.push_back ({ std::max (0.0f, beat), (std::uint8_t) lane,
                             (std::uint8_t) std::clamp ((int) std::lround (level * 127.0f), 1, 127),
                             (std::uint8_t) (index % kRoundRobins) });
            struck.push_back (std::max (0.0f, beat));
            ++index;
        }

        // The crash is played with the hand that was keeping time, so the hat
        // stroke underneath it drops right back: two cymbals sounding equally
        // on one beat is the give-away that a machine placed them.
        for (Hit& h : out)
            if (isOrnamentLane (h.lane)
                && std::any_of (struck.begin(), struck.end(),
                                [&] (const float beat) { return std::abs (h.beat - beat) < 0.06f; }))
                h.velocity = (std::uint8_t) std::max (1, (int) h.velocity / 3);
    }

    void PerformanceEngine::collapseDoubledCymbals (std::vector<Hit>& out)
    {
        // A hand plays one cymbal at a time. Two time-keeping strokes a few
        // milliseconds apart are not a groove, they are the same stroke twice,
        // and the louder one is the one that was played.
        constexpr float window = 0.06f;

        std::vector<bool> drop (out.size(), false);
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            if (drop[i] || ! isOrnamentLane ((int) out[i].lane))
                continue;

            for (std::size_t j = i + 1;
                 j < out.size() && out[j].beat - out[i].beat < window;
                 ++j)
            {
                if (drop[j] || ! isOrnamentLane ((int) out[j].lane))
                    continue;

                const bool keepFirst = out[i].velocity != out[j].velocity
                                     ? out[i].velocity > out[j].velocity
                                     : out[i].lane <= out[j].lane;
                drop[keepFirst ? j : i] = true;

                if (! keepFirst)
                    break;
            }
        }

        std::size_t write = 0;
        for (std::size_t i = 0; i < out.size(); ++i)
            if (! drop[i])
                out[write++] = out[i];
        out.resize (write);
    }

    std::vector<Hit> PerformanceEngine::renderPhrase (const PerformanceSettings& base,
                                                      int  phraseIndex,
                                                      bool includeFill) const
    {
        std::vector<Hit> out;
        if (corpus == nullptr || ! corpus->isLoaded())
            return out;

        const int   bars        = std::max (1, base.phraseBars);
        // Whichever arrangement block owns this phrase supplies its settings.
        PerformanceSettings s   = settingsForBar (base, phraseIndex * bars);
        const float dstBar      = std::max (1.0f, s.beatsPerBar);
        const float phraseBeats = dstBar * (float) bars;
        const std::uint64_t seed = mix (blockSeed (s) ^ mix ((std::uint64_t) phraseIndex + 1));

        // An intro comes in on the kick and the hats: the same groove with the
        // backbeat and the toms left out, which is the vibe without having to
        // build it by hand. Touching that block's kit switches - or the pad -
        // hands the decision straight back to the user, and the fill that runs
        // into the verse still plays the whole kit.
        if (sectionAtBar (s, phraseIndex * bars) == SectionIntro
            && (s.laneMask == 0xFFFFFFFFu || s.laneMask == (1u << NumLanes) - 1u))
        {
            std::uint32_t intro = 1u << LaneKick;
            for (int lane = 0; lane < NumLanes; ++lane)
                if (isHatLane (lane) || lane == LaneCrashL || lane == LaneCrashR
                    || (s.rideInsteadOfHat && isRideLane (lane)))
                    intro |= 1u << lane;
            s.laneMask = intro;
        }

        // --- section colouring -------------------------------------------
        PerformanceSettings sec = s;
        switch (sectionAtBar (s, phraseIndex * bars))
        {
            case SectionIntro:
                // Barely moved: an intro is the same part held back, and a big
                // step across the pad would fetch a different song entirely.
                sec.complexity *= 0.92f;
                sec.sectionVelocity *= 0.82f;
                break;
            case SectionChorus:
                sec.sectionVelocity = std::min (1.0f, sec.sectionVelocity + 0.10f);
                sec.hatOpenness = std::min (1.0f, sec.hatOpenness + 0.3f);
                break;
            case SectionBridge:
                sec.sectionVelocity *= 0.85f; sec.halfTime = true;
                break;
            case SectionOutro:
                sec.sectionVelocity *= 0.9f;
                break;
            default:
                break;
        }

        const auto src = pickSources (sec, phraseIndex, grooveSeed (sec));
        if (src.skeleton == nullptr || src.colour == nullptr)
            return out;

        // --- gather, folded onto the target metre -------------------------
        // Takes played in the requested metre are preferred, so most bars need
        // no folding at all. When only 4/4 material is available, bars shorter
        // than the source drop the tail and longer bars wrap round to the top
        // of the source bar, keeping the backbeat where a drummer would put it.
        const float stretch  = s.halfTime || sec.halfTime ? 2.0f : 1.0f;

        std::vector<Raw> raw;
        raw.reserve (src.skeleton->hits.size() + src.colour->hits.size() + 16);

        const auto gather = [&] (const Phrase& phrase, bool wantSkeleton)
        {
            const int   srcBars   = std::max (1, phrase.bars);
            const float kSourceBar = phrase.sourceBeatsPerBar();
            // Only a 4/4 take folded into a compound metre needs its grid
            // re-read as triplets; a take actually played in 6/8 already is.
            const bool  compound  = s.timeSigDen == 8 && s.timeSigNum % 3 == 0
                                    && std::abs (kSourceBar - dstBar) > 0.01f;
            for (int bar = 0; bar < bars; ++bar)
            {
                const int srcBarIndex = bar % srcBars;
                for (const auto& h : phrase.hits)
                {
                    if (isSkeletonLane (h.lane) != wantSkeleton)
                        continue;

                    float grid = h.gridBeat();
                    if (compound)
                        grid = std::round (grid * 3.0f) / 3.0f;   // triplet subdivision

                    const int   hitBar = (int) std::floor (grid / kSourceBar);
                    if (hitBar != srcBarIndex)
                        continue;
                    float pos = (grid - (float) hitBar * kSourceBar) * stretch;
                    if (stretch > 1.0f && pos >= kSourceBar)
                        continue;

                    for (float p = pos; p < dstBar - 0.001f; p += kSourceBar * stretch)
                        raw.push_back ({ (float) bar * dstBar + p, h.devBeats(),
                                         h.lane, h.velocity });
                }
            }
        };

        gather (*src.skeleton, true);
        gather (*src.colour,   false);

        // The louder a section is played, the less loose cymbal chatter belongs
        // in it: a heavy chorus keeps its time on the grid and lets the crashes
        // carry the size, instead of sprinkling stray hat and ride hits.
        {
            const float loud = std::clamp (s.intensity, 0.0f, 1.0f);
            if (loud > 0.7f)
            {
                // The stray chatter is pulled onto the sixteenth rather than
                // thrown away: a chorus played hard should be bigger than the
                // verse, not thinner than it, so the hits stay and only their
                // placement is tidied. Two strokes landing on one subdivision
                // become the louder of the two.
                const float unit = 0.25f;
                for (Raw& h : raw)
                {
                    if (! isOrnamentLane (h.lane))
                        continue;
                    const float bar   = std::floor (h.beat / dstBar) * dstBar;
                    const float inBar = h.beat - bar;
                    h.beat = bar + std::round (inBar / unit) * unit;
                    h.dev  = 0.0f;
                }

                std::stable_sort (raw.begin(), raw.end(),
                                  [] (const Raw& a, const Raw& b)
                                  {
                                      if (a.lane != b.lane)
                                          return a.lane < b.lane;
                                      return a.beat < b.beat;
                                  });
                std::vector<Raw> merged;
                merged.reserve (raw.size());
                for (const Raw& h : raw)
                {
                    if (! merged.empty() && merged.back().lane == h.lane
                        && std::abs (merged.back().beat - h.beat) <= 0.004f)
                    {
                        merged.back().velocity = std::max (merged.back().velocity,
                                                           h.velocity);
                        continue;
                    }
                    merged.push_back (h);
                }
                raw.swap (merged);
            }
        }

        thinOrnaments (sec, dstBar, bars, raw);
        supplyLanes (sec, dstBar, bars, seed, raw);
        addGhostNotes (sec, dstBar, bars, seed, raw);

        // --- fill ---------------------------------------------------------
        if (includeFill)
        {
            const float fillBeats = std::min (phraseBeats,
                                              dstBar * std::max (0.5f, s.fillLengthBars));
            const float fillStart = phraseBeats - fillBeats;
            raw.erase (std::remove_if (raw.begin(), raw.end(),
                                       [&] (const Raw& h) { return h.beat >= fillStart - 0.02f; }),
                       raw.end());
            appendFill (sec, phraseIndex, fillStart, phraseBeats, seed, raw);
        }

        // --- articulation routing ------------------------------------------
        for (auto& h : raw)
        {
            if (sec.rideInsteadOfHat && isHatLane (h.lane) && h.lane != LaneHatPedal)
            {
                h.lane = (h.lane >= LaneHatOpen2) ? (std::uint8_t) LaneRideEdge
                                                  : (std::uint8_t) LaneRideBow;
            }
            else if (isHatLane (h.lane) && h.lane != LaneHatPedal && sec.hatOpenness > 0.0f)
            {
                // Walk the openness ladder rather than jumping to "open hat",
                // and open it where a drummer's foot actually comes up: on the
                // way out of the bar and on the off-beats. Opening whichever
                // strokes a random number picked is what made the hat sound
                // like a different instrument every other note.
                int step = 0;
                for (; step < kNumHatSteps; ++step)
                    if (kHatLadder[step] == h.lane)
                        break;
                if (step < kNumHatSteps)
                {
                    const float inBar   = h.beat - std::floor (h.beat / dstBar) * dstBar;
                    const float toEnd   = dstBar - inBar;
                    const bool  offBeat = subdivisionClass (inBar) == 2;

                    // Turned up, the hats ride open the way a punk drummer
                    // plays them: the foot comes off and stays off, so the
                    // off-beats and the way out of the bar are wide open and
                    // the down beat is the only stroke the foot leans back
                    // on. The knob has to read as a different instrument, not
                    // as a stroke or two of colour.
                    const float weight = (toEnd <= 0.51f) ? 1.00f
                                       : offBeat          ? 1.00f
                                                          : 0.72f;

                    // Square-rooted, so half the knob is already half open
                    // instead of one rung up a six-rung ladder.
                    const float amount = std::sqrt (sec.hatOpenness) * weight
                                         * (float) (kNumHatSteps - 1);

                    // The fraction is spent as a real stroke rather than
                    // rounded away, which is what keeps an open hat alive
                    // instead of eight identical open strokes.
                    const float frac  = amount - std::floor (amount);
                    const int   extra = (int) amount
                                      + (rand01 (seed, (std::uint64_t) (h.beat * 64.0f), 7) < frac
                                             ? 1 : 0);

                    if (extra > 0)
                    {
                        h.lane = (std::uint8_t) kHatLadder[std::min (kNumHatSteps - 1, step + extra)];

                        // A hat is opened by leaning into it as well as by
                        // lifting the foot, so the open stroke is heard.
                        h.velocity = (std::uint8_t) std::min (
                            127, (int) std::lround ((float) h.velocity * 1.10f));
                    }
                }
            }
        }

        // A piece switched out of a block does not leave a hole in the part: a
        // drummer moves the figure onto whatever is still in front of him, so
        // an intro of nothing but toms plays the song's groove on the toms
        // rather than a bar of silence with a couple of tom hits in it.
        {
            const auto on = [&s] (int lane) { return (s.laneMask & (1u << lane)) != 0; };
            const auto firstOn = [&on] (std::initializer_list<int> preference)
            {
                for (const int lane : preference)
                    if (on (lane))
                        return lane;
                return -1;
            };

            const int  tom     = firstOn ({ LaneTom2, LaneTom1, LaneTom3, LaneTom4 });
            const int  timeTom = firstOn ({ LaneTom1, LaneTom2, LaneTom3, LaneTom4 });
            const bool timeOn  = on (LaneHatClosed) || on (LaneRideBow);

            for (Raw& h : raw)
            {
                const int lane = (int) h.lane;
                if (on (lane))
                    continue;

                if (isSnareLane (lane) && ! on (LaneSnare))
                {
                    // While a cymbal is still keeping time, switching the snare
                    // out means the backbeat is gone, as it is in Logic. Only
                    // once the cymbals are out too - a tom-led intro - does the
                    // figure move onto the toms rather than disappear.
                    if (tom >= 0 && ! timeOn)
                        h.lane = (std::uint8_t) tom;
                }
                else if (isHatLane (lane) && on (LaneRideBow))
                {
                    h.lane = (std::uint8_t) LaneRideBow;
                }
                else if (isRideLane (lane) && on (LaneHatClosed))
                {
                    h.lane = (std::uint8_t) LaneHatClosed;
                }
                else if (isOrnamentLane (lane) && ! timeOn && timeTom >= 0)
                {
                    // Time on a tom is played on the beat, not as sixteenths:
                    // the off-beat strokes are dropped with the cymbal.
                    const float inBar = h.beat - std::floor (h.beat / dstBar) * dstBar;
                    if (std::abs (inBar - std::round (inBar)) < 0.01f)
                        h.lane = (std::uint8_t) timeTom;
                }
            }
        }

        raw.erase (std::remove_if (raw.begin(), raw.end(), [&] (const Raw& h)
                   { return (s.laneMask & (1u << h.lane)) == 0; }), raw.end());

        std::sort (raw.begin(), raw.end(),
                   [] (const Raw& a, const Raw& b)
                   {
                       if (a.beat < b.beat) return true;
                       if (b.beat < a.beat) return false;
                       if (a.lane != b.lane) return a.lane < b.lane;
                       return a.velocity < b.velocity;
                   });

        // --- feel, swing, drift, dynamics -----------------------------------
        const float gridUnit   = s.swingSixteenth ? 0.25f : 0.5f;
        const float swingShift = s.swing * gridUnit * 0.34f;
        const float feelShift  = (s.feel - 0.5f) * 0.05f;      // +/-25 ms at 120 bpm

        // Dynamics come from the block's own Intensity knob. The pad decides
        // which take is played; this decides how hard it is played, and the
        // two are deliberately independent.
        const float energy  = std::clamp (sec.sectionVelocity, 0.0f, 1.0f);
        const float velGain = 0.62f + 0.78f * energy;

        // Slow phrase breathing over eight bars, so bar 3 does not sit exactly
        // where bar 1 sat.
        const float driftPhase = (float) (phraseIndex * bars) / 8.0f;

        int lastBucket[NumLanes];
        int laneCount[NumLanes];
        int lastVariant[NumLanes];
        std::fill (std::begin (lastBucket), std::end (lastBucket), -1);
        std::fill (std::begin (laneCount), std::end (laneCount), 0);
        std::fill (std::begin (lastVariant), std::end (lastVariant), -1);

        // Which hand struck last. One counter for the whole kit, because a
        // player's hands keep alternating as they cross from snare to toms.
        int hand = 1;

        out.reserve (raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            const Raw& r = raw[i];
            const int  lane = r.lane;

            // Keep the drummer's own deviation and scale it, rather than adding
            // uncorrelated noise on top of a quantised grid. Humanize is the
            // only thing that moves a stroke off the grid, so at 0 the bar is
            // machine-tight and at 0.5 it is the take exactly as recorded.
            // A fill is what glues one section to the next, so it is left
            // where it was placed: swinging it, leaning it or drifting it is
            // what made fills read as a separate idea from the groove.
            float beat = r.beat;
            if (isAccentCymbal (lane))
            {
                // A crash marks a landmark. Leaning it, swinging it or drifting
                // it is what makes an arrangement sound like it is falling
                // over, so it lands exactly on the subdivision it marks.
                const float bar = std::floor (beat / dstBar) * dstBar;
                beat = bar + std::round ((beat - bar) * 4.0f) / 4.0f;
            }
            else if (! r.fill)
            {
                beat += r.dev * 2.0f * s.humanize + feelShift
                        + laneTimingBias (lane) * s.humanize;

                const int gridIndex = (int) std::llround (beat / gridUnit);
                if ((gridIndex & 1) != 0)
                    beat += swingShift;

                beat += 0.014f * s.humanize
                        * cycleSin (driftPhase + beat / (8.0f * dstBar));
            }

            if (beat < -0.02f || beat >= phraseBeats)
                continue;

            // --- velocity. The drummer's own dynamics carry the bar; the
            // corpus transition model only pulls a stroke gently towards what
            // usually follows the last one on this lane. Drawing a bucket at
            // random instead - which is what this used to do - throws the
            // performance away and replaces it with noise.
            float vel = (float) r.velocity;
            if (lastBucket[lane] >= 0)
            {
                if (const auto* row = corpus->velocityRow (lane, lastBucket[lane]))
                {
                    int   total    = 0;
                    float weighted = 0.0f;
                    for (int b = 0; b < GrooveCorpus::kVelocityBuckets; ++b)
                    {
                        total    += row[b];
                        weighted += (float) row[b] * ((float) b + 0.5f);
                    }
                    if (total > 0)
                    {
                        const float expected = weighted / (float) total * 128.0f
                                               / (float) GrooveCorpus::kVelocityBuckets;
                        vel = 0.88f * vel + 0.12f * expected;
                    }
                }
            }
            lastBucket[lane] = velocityBucket ((int) vel);

            // Metric accenting: the notes a player leans on stay up, the ones
            // between them sit back. This is the difference between a groove
            // and a row of identical hits.
            const float inBar = beat - std::floor (beat / dstBar) * dstBar;
            if (isOrnamentLane (lane))
            {
                vel *= 0.80f + 0.26f * metricWeight (inBar);

                // Time-keeping is played with the tip of the stick under the
                // rest of the kit. At full velocity every stroke reaches for
                // the hardest recorded layer, which is where the hat starts to
                // click and string across the groove instead of sitting in it.
                vel *= 0.94f;

                // Hat dynamics are the groove. A drummer leans on the strokes
                // that carry the beat and lets the ones between them fall
                // away; a row of strokes at one level is what reads as a
                // machine, so the accents run deeper on the hat than anywhere
                // else on the kit.
                if (isHatLane (lane) && lane != LaneHatPedal)
                    vel *= 0.70f + 0.42f * metricWeight (inBar);
            }

            // Phrase breathing: the take lifts slightly into its own cadence
            // rather than sitting at one level for eight bars.
            vel *= 0.965f + 0.07f * (beat / std::max (1.0f, phraseBeats));

            // The Ghost knob is the whole range of the thing: at zero the
            // ghost strokes and the percussion colour are simply not played,
            // at half they sit where the take had them, and at full a drummer
            // is leaning into them. Dropping the hit rather than only turning
            // it down is what makes the bottom of the knob mean anything.
            if (isGhostLane (lane) || (s.ghostMask & (1u << lane)) != 0)
            {
                if (s.ghostAmount <= 0.02f)
                    continue;

                // Hand percussion is a colour a drummer reaches for, not part
                // of the kit: it only plays when the knob is asking for it.
                if (lane == LanePerc && s.ghostAmount < 0.7f)
                    continue;
                vel *= 0.22f + 1.56f * s.ghostAmount;
            }

            vel *= velGain;
            vel *= 1.0f - 0.05f * s.humanize * rand01 (seed, i, 5);

            // No drummer repeats a stroke exactly, and no drummer holds one
            // level across a song: a couple of percent of movement per hit, and
            // a slow lift and fall across sixteen bars, so the arrangement
            // breathes rather than being stamped out.
            vel *= 1.0f + 0.03f * (rand01 (seed, i, 0x3Bu) - 0.5f) * 2.0f;
            vel *= 1.0f + 0.035f * cycleSin ((float) (phraseIndex * bars) / 16.0f
                                             + beat / (16.0f * dstBar));

            // However hard the section is played, the time keeper stays under
            // the kit: the top of a hat's velocity range is where it turns into
            // a click.
            if (isOrnamentLane (lane))
                vel = std::min (vel, 110.0f);

            // --- sticking. A player has two hands, and they take turns as they
            // move around the kit: right, left, right. The off hand is a shade
            // lighter and reaches for a different take of the sample, so a roll
            // reads as two hands rather than one sample retriggered. Even
            // round-robin slots are the lead hand, odd ones the off hand.
            const int slots = kRoundRobins;
            int variant = 0;

            if (isHandLane (lane))
            {
                hand ^= 1;
                if (hand != 0)
                    vel *= 0.94f;

                const int pairs = std::max (1, slots / 2);
                const int pair  = std::min (pairs - 1,
                                            (int) (rand01 (seed, i, 0x22u) * (float) pairs));
                variant = pair * 2 + hand;
            }
            else
            {
                // Feet and cymbals: just never the same sample twice running.
                variant = std::min (slots - 1,
                                    (int) (rand01 (seed, i, 0x22u) * (float) slots));
                if (variant == lastVariant[lane])
                    variant = (variant + 1 + (laneCount[lane] % (slots - 1))) % slots;
            }

            lastVariant[lane] = variant;
            ++laneCount[lane];

            out.push_back ({ std::max (0.0f, beat), r.lane,
                             (std::uint8_t) std::clamp ((int) std::lround (vel), 1, 127),
                             (std::uint8_t) variant });
        }

        addCrashes (s, sec, phraseIndex, bars, dstBar, seed, out);

        std::stable_sort (out.begin(), out.end(),
                          [] (const Hit& a, const Hit& b) { return a.beat < b.beat; });

        collapseDoubledCymbals (out);

        // Last word on how busy a bar is allowed to get. Even at the top of the
        // pad a bar is something a drummer plays, so once the groove, its
        // ornaments and the crashes are all in, the quietest colour strokes -
        // pedal hats, ghosts, rim taps - are dropped until the bar is inside
        // the pad's density. The kick, the backbeat and the cymbal keeping time
        // are never touched, so thinning takes clutter off rather than taking
        // the groove apart.
        {
            // Complexity alone decides the ceiling: Intensity is how hard the
            // kit is played, and it must never change which strokes are there.
            const float ceiling = std::max (24.0f,
                                            16.0f + 10.0f * std::clamp (s.complexity, 0.0f, 1.0f));
            const auto  ornament = [] (const Hit& h)
            {
                return h.lane == LaneSnareGhost || h.lane == LaneHatPedal
                    || h.lane == LanePerc       || h.lane == LaneHatSplash;
            };

            for (int bar = 0; bar < bars; ++bar)
            {
                const float from = (float) bar * dstBar, to = from + dstBar;
                std::vector<std::size_t> here;
                for (std::size_t i = 0; i < out.size(); ++i)
                    if (out[i].beat >= from - 0.001f && out[i].beat < to)
                        here.push_back (i);

                int over = (int) here.size() - (int) std::lround (ceiling);
                if (over <= 0)
                    continue;

                std::stable_sort (here.begin(), here.end(),
                                  [&] (std::size_t a, std::size_t b)
                                  {
                                      const bool oa = ornament (out[a]), ob = ornament (out[b]);
                                      if (oa != ob)
                                          return oa;
                                      return out[a].velocity < out[b].velocity;
                                  });

                std::vector<std::size_t> drop;
                for (std::size_t k = 0; k < here.size() && over > 0; ++k)
                    if (ornament (out[here[k]]))
                    {
                        drop.push_back (here[k]);
                        --over;
                    }

                std::sort (drop.begin(), drop.end(), std::greater<std::size_t>());
                for (const auto i : drop)
                    out.erase (out.begin() + (std::ptrdiff_t) i);
            }
        }
        return out;
    }

    std::vector<Hit> PerformanceEngine::fillForPhrase (const PerformanceSettings& base,
                                                       int   phraseIndex,
                                                       float fillBeats) const
    {
        std::vector<Hit> out;
        if (corpus == nullptr || fillBeats <= 0.0f)
            return out;

        const int  bars = std::max (1, base.phraseBars);
        const auto s    = settingsForBar (base, phraseIndex * bars);
        if (! phraseEndsWithFill (base, phraseIndex))
            return out;

        std::vector<Raw> raw;
        appendFill (s, phraseIndex, 0.0f, fillBeats,
                    mix (blockSeed (s) ^ (std::uint64_t) (phraseIndex + 1)), raw);

        out.reserve (raw.size());
        for (const auto& h : raw)
            if ((s.laneMask & (1u << h.lane)) != 0)
                out.push_back ({ h.beat, h.lane, h.velocity });

        std::sort (out.begin(), out.end(),
                   [] (const Hit& a, const Hit& b) { return a.beat < b.beat; });
        return out;
    }

    bool PerformanceEngine::phraseEndsWithFill (const PerformanceSettings& base,
                                                int phraseIndex) const
    {
        const int  bars = std::max (1, base.phraseBars);
        const auto s    = settingsForBar (base, phraseIndex * bars);
        if (s.fillAmount <= 0.001f)
            return false;

        // The last phrase of a block always turns the corner with a fill, so
        // no section of the song ever runs into the next one flat. Every 4th
        // phrase is the other natural cadence; the knob then fills in the
        // phrases between them as it is turned up.
        const bool blockEnd = blockIndexForBar (base, phraseIndex * bars)
                              != blockIndexForBar (base, (phraseIndex + 1) * bars);
        if (blockEnd)
            return true;

        const bool  cadence   = ((phraseIndex + 1) % 4) == 0;
        if (cadence)
            return true;

        return s.fillAmount > 0.55f
            || rand01 (mix (blockSeed (s) ^ 0xB00Bull), (std::uint64_t) phraseIndex)
                   < s.fillAmount * 0.6f;
    }

    std::vector<Hit> PerformanceEngine::renderPhrasePreview (const PerformanceSettings& s,
                                                             int phraseIndex) const
    {
        return renderPhrase (s, phraseIndex, phraseEndsWithFill (s, phraseIndex));
    }

    std::vector<Hit> PerformanceEngine::renderBars (const PerformanceSettings& s,
                                                    int startBar,
                                                    int numBars) const
    {
        std::vector<Hit> out;
        if (numBars <= 0)
            return out;

        const int   bars        = std::max (1, s.phraseBars);
        const float beatsPerBar = std::max (1.0f, s.beatsPerBar);
        const int   firstPhrase = (int) std::floor ((double) startBar / bars);
        const int   lastPhrase  = (int) std::floor ((double) (startBar + numBars - 1) / bars);

        const float rangeStart = (float) startBar * beatsPerBar;
        const float rangeEnd   = (float) (startBar + numBars) * beatsPerBar;

        for (int p = firstPhrase; p <= lastPhrase; ++p)
        {
            const float offset = (float) (p * bars) * beatsPerBar;
            for (auto h : renderPhrase (s, p, phraseEndsWithFill (s, p)))
            {
                h.beat += offset;
                if (h.beat >= rangeStart && h.beat < rangeEnd)
                    out.push_back (h);
            }
        }

        std::stable_sort (out.begin(), out.end(),
                          [] (const Hit& a, const Hit& b) { return a.beat < b.beat; });

        // A phrase boundary must not land a stroke on top of the last stroke
        // of the phrase before it either.
        collapseDoubledCymbals (out);

        // Round robin is guaranteed across the whole rendered range, not just
        // inside a phrase, so a phrase boundary cannot fire the same sample
        // twice in a row either.
        // Each arrangement block is its own performance, so the chain restarts
        // at a block boundary: editing one block cannot rotate the samples in
        // the block after it.
        int lastVariant[NumLanes];
        std::fill (std::begin (lastVariant), std::end (lastVariant), -1);
        int lastBlock = -1;
        for (auto& h : out)
        {
            const int block = blockIndexForBar (s, (int) std::floor (h.beat / beatsPerBar));
            if (block != lastBlock)
            {
                std::fill (std::begin (lastVariant), std::end (lastVariant), -1);
                lastBlock = block;
            }

            if ((int) h.variant == lastVariant[h.lane])
            {
                // Moving off a repeated sample must not change which hand
                // played the stroke, so on the drums that are stuck the slot
                // steps by a whole pair and keeps its parity.
                const int step = isHandLane ((int) h.lane) ? 2 : 1;
                h.variant = (std::uint8_t) (((int) h.variant + step) % kRoundRobins);
            }
            lastVariant[h.lane] = (int) h.variant;
        }
        return out;
    }
}
