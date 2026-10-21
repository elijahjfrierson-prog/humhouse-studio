#include "GrooveCorpus.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace hhx
{
    namespace
    {
        constexpr char          kMagic[4] = { 'H', 'H', 'C', 'X' };

        struct Reader
        {
            const std::uint8_t* p;
            const std::uint8_t* end;

            bool take (void* dest, std::size_t n)
            {
                if ((std::size_t) (end - p) < n)
                    return false;
                std::memcpy (dest, p, n);
                p += n;
                return true;
            }
        };
    }

    const char* laneName (int lane)
    {
        switch (lane)
        {
            case LaneKick:       return "Kick";
            case LaneSnare:      return "Snare";
            case LaneSnareRim:   return "Snare Rim";
            case LaneSideStick:  return "Side Stick";
            case LaneSnareGhost: return "Snare Ghost";
            case LaneSnareFlam:  return "Snare Flam";
            case LaneSnareRoll:  return "Snare Roll";
            case LaneHatClosed:  return "Hat Closed";
            case LaneHatTight:   return "Hat Tight";
            case LaneHatOpen1:   return "Hat Open 1";
            case LaneHatOpen2:   return "Hat Open 2";
            case LaneHatOpen3:   return "Hat Open 3";
            case LaneHatOpen4:   return "Hat Open 4";
            case LaneHatPedal:   return "Hat Pedal";
            case LaneHatSplash:  return "Hat Splash";
            case LaneHatBell:    return "Hat Bell";
            case LaneRideBow:    return "Ride Bow";
            case LaneRideBell:   return "Ride Bell";
            case LaneRideEdge:   return "Ride Edge";
            case LaneRideCrash:  return "Ride Crash";
            case LaneCrashL:     return "Crash L";
            case LaneCrashR:     return "Crash R";
            case LaneCrash3:     return "Crash 3";
            case LaneChina:      return "China";
            case LaneSplash:     return "Splash";
            case LaneTom1:       return "Tom 1";
            case LaneTom2:       return "Tom 2";
            case LaneTom3:       return "Tom 3";
            case LaneTom4:       return "Tom 4";
            case LanePerc:       return "Perc";
            default:             return "?";
        }
    }

    const char* sectionName (int section)
    {
        switch (section)
        {
            case SectionIntro:  return "Intro";
            case SectionVerse:  return "Verse";
            case SectionChorus: return "Chorus";
            case SectionBridge: return "Bridge";
            case SectionOutro:  return "Outro";
            case SectionFill:   return "Fill";
            default:            return "?";
        }
    }

    bool isSnareLane (int lane)
    {
        return lane == LaneSnare || lane == LaneSnareRim || lane == LaneSideStick
            || lane == LaneSnareGhost || lane == LaneSnareFlam || lane == LaneSnareRoll;
    }

    bool isHatLane (int lane)
    {
        return (lane >= LaneHatClosed && lane <= LaneHatOpen4)
            || lane == LaneHatPedal || lane == LaneHatSplash || lane == LaneHatBell;
    }

    bool isRideLane (int lane)
    {
        return lane >= LaneRideBow && lane <= LaneRideCrash;
    }

    bool isTomLane (int lane)
    {
        return lane >= LaneTom1 && lane <= LaneTom4;
    }

    bool isCymbalLane (int lane)
    {
        return (lane >= LaneCrashL && lane <= LaneSplash) || lane == LaneRideCrash;
    }

    int laneToNote (int lane)
    {
        // The HumHouse drum map: one distinct note per articulation, so a
        // per-instrument export or an external editor can address all 30
        // pieces. GM shares notes between articulations; this does not.
        switch (lane)
        {
            case LaneKick:       return 36;
            case LaneSnare:      return 38;
            case LaneSnareRim:   return 40;
            case LaneSideStick:  return 37;
            case LaneSnareGhost: return 39;
            case LaneSnareFlam:  return 33;
            case LaneSnareRoll:  return 34;
            case LaneHatClosed:  return 42;
            case LaneHatTight:   return 22;
            case LaneHatOpen1:   return 26;
            case LaneHatOpen2:   return 46;
            case LaneHatOpen3:   return 24;
            case LaneHatOpen4:   return 25;
            case LaneHatPedal:   return 44;
            case LaneHatSplash:  return 21;
            case LaneHatBell:    return 23;
            case LaneRideBow:    return 51;
            case LaneRideBell:   return 53;
            case LaneRideEdge:   return 59;
            case LaneRideCrash:  return 60;
            case LaneCrashL:     return 49;
            case LaneCrashR:     return 57;
            case LaneCrash3:     return 55;
            case LaneChina:      return 52;
            case LaneSplash:     return 27;
            case LaneTom1:       return 48;
            case LaneTom2:       return 47;
            case LaneTom3:       return 45;
            case LaneTom4:       return 41;
            case LanePerc:       return 56;
            default:             return 38;
        }
    }

    int laneToGmNote (int lane)
    {
        switch (lane)
        {
            case LaneKick:       return 36;
            case LaneSnare:      return 38;
            case LaneSnareRim:   return 40;
            case LaneSideStick:  return 37;
            case LaneSnareGhost: return 38;
            case LaneSnareFlam:  return 38;
            case LaneSnareRoll:  return 38;
            case LaneHatClosed:  return 42;
            case LaneHatTight:   return 42;
            case LaneHatOpen1:   return 46;
            case LaneHatOpen2:   return 46;
            case LaneHatOpen3:   return 46;
            case LaneHatOpen4:   return 46;
            case LaneHatPedal:   return 44;
            case LaneHatSplash:  return 46;
            case LaneHatBell:    return 42;
            case LaneRideBow:    return 51;
            case LaneRideBell:   return 53;
            case LaneRideEdge:   return 59;
            case LaneRideCrash:  return 51;
            case LaneCrashL:     return 49;
            case LaneCrashR:     return 57;
            case LaneCrash3:     return 52;
            case LaneChina:      return 52;
            case LaneSplash:     return 55;
            case LaneTom1:       return 48;
            case LaneTom2:       return 45;
            case LaneTom3:       return 43;
            case LaneTom4:       return 41;
            case LanePerc:       return 56;
            default:             return 38;
        }
    }

    int gmNoteToLane (int note)
    {
        // First exact match wins, so the primary articulation of each GM note
        // is chosen over the ones that only share it.
        for (int lane = 0; lane < NumLanes; ++lane)
            if (laneToGmNote (lane) == note)
                return lane;
        return -1;
    }

    int noteToLane (int note)
    {
        // The HumHouse map is one note per articulation, so an exported file
        // round-trips exactly; anything written against the GM map still lands
        // on the right piece.
        for (int lane = 0; lane < NumLanes; ++lane)
            if (laneToNote (lane) == note)
                return lane;
        return gmNoteToLane (note);
    }

    bool GrooveCorpus::loadFromMemory (const void* data, std::size_t numBytes)
    {
        loaded = false;
        beatPhrases.clear();
        fillPhrases.clear();
        characterNames.clear();
        velocityModel.clear();

        Reader r { static_cast<const std::uint8_t*> (data),
                   static_cast<const std::uint8_t*> (data) + numBytes };

        char magic[4] {};
        std::uint32_t version = 0, count = 0, numChars = 0;
        if (! r.take (magic, 4) || std::memcmp (magic, kMagic, 4) != 0)
            return false;
        if (! r.take (&version, 4) || version != GrooveCorpus::kFormatVersion)
            return false;
        if (! r.take (&count, 4) || ! r.take (&numChars, 4))
            return false;

        for (std::uint32_t c = 0; c < numChars; ++c)
        {
            std::uint8_t len = 0;
            if (! r.take (&len, 1))
                return false;
            std::string name ((std::size_t) len, '\0');
            if (len > 0 && ! r.take (name.data(), len))
                return false;
            characterNames.push_back (std::move (name));
        }

        velocityModel.resize ((std::size_t) NumLanes * kVelocityBuckets * kVelocityBuckets);
        if (! r.take (velocityModel.data(), velocityModel.size()))
            return false;

        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint8_t  kind = 0, bars = 0, cx = 0, in = 0, section = 0, styles = 0, swing = 0;
            std::uint8_t  derived = 0;
            std::uint8_t  sigNum = 4, sigDen = 4;
            std::uint16_t bpm = 0, charMask = 0, numHits = 0;
            if (! r.take (&kind, 1) || ! r.take (&bars, 1)
                || ! r.take (&sigNum, 1) || ! r.take (&sigDen, 1)
                || ! r.take (&cx, 1)
                || ! r.take (&in, 1) || ! r.take (&bpm, 2) || ! r.take (&section, 1)
                || ! r.take (&charMask, 2) || ! r.take (&styles, 1)
                || ! r.take (&swing, 1) || ! r.take (&derived, 1)
                || ! r.take (&numHits, 2))
                return false;

            Phrase phrase;
            phrase.isFill     = kind != 0;
            phrase.bars       = bars;
            phrase.sigNum     = sigNum > 0 ? sigNum : (std::uint8_t) 4;
            phrase.sigDen     = sigDen > 0 ? sigDen : (std::uint8_t) 4;
            phrase.bpm        = bpm;
            phrase.complexity = (float) cx / 255.0f;
            phrase.intensity  = (float) in / 255.0f;
            phrase.swing      = (float) swing / 255.0f;
            phrase.section    = section;
            phrase.charMask   = charMask;
            phrase.fillStyles = styles;
            phrase.derived    = derived != 0;
            phrase.hits.reserve (numHits);

            for (std::uint16_t h = 0; h < numHits; ++h)
            {
                SourceHit hit;
                if (! r.take (&hit.lane, 1) || ! r.take (&hit.velocity, 1)
                    || ! r.take (&hit.grid, 2) || ! r.take (&hit.dev, 1))
                    return false;
                if (hit.lane < NumLanes)
                    phrase.hits.push_back (hit);
            }

            // Total order, including the lane and velocity: simultaneous hits
            // must land in the same order on every platform, or the round-robin
            // and variant choices downstream diverge between builds.
            std::sort (phrase.hits.begin(), phrase.hits.end(),
                       [] (const SourceHit& a, const SourceHit& b)
                       {
                           const float ta = a.gridBeat() + a.devBeats();
                           const float tb = b.gridBeat() + b.devBeats();
                           if (ta < tb) return true;
                           if (tb < ta) return false;
                           if (a.lane != b.lane) return a.lane < b.lane;
                           return a.velocity < b.velocity;
                       });

            (phrase.isFill ? fillPhrases : beatPhrases).push_back (std::move (phrase));
        }

        loaded = ! beatPhrases.empty();
        return loaded;
    }

    std::vector<int> GrooveCorpus::rank (const std::vector<Phrase>& pool,
                                         float complexity,
                                         float intensity,
                                         std::uint16_t charMask,
                                         int   bars,
                                         int   section,
                                         int   maxResults,
                                         int   sigNum,
                                         int   sigDen) const
    {
        std::vector<int> idx;
        idx.reserve (pool.size());
        for (int i = 0; i < (int) pool.size(); ++i)
        {
            const auto& p = pool[(std::size_t) i];
            if (bars > 0 && p.bars != bars)
                continue;
            if (charMask != 0 && (p.charMask & charMask) == 0)
                continue;
            idx.push_back (i);
        }

        // Nothing in this character at this length: fall back to the whole
        // pool rather than leaving the bar empty.
        if (idx.empty())
        {
            idx.resize (pool.size());
            std::iota (idx.begin(), idx.end(), 0);
        }

        const auto distance = [&] (int i)
        {
            const auto& p = pool[(std::size_t) i];
            const float dc = p.complexity - complexity;
            const float di = p.intensity  - intensity;
            float d = dc * dc + di * di;
            // Section is a preference, not a filter.
            if (section >= 0 && p.section != (std::uint8_t) section)
                d += 0.02f;
            // A take played in the requested metre always beats a 4/4 take
            // folded into it, but a fold still beats an empty bar.
            if (sigNum > 0 && (p.sigNum != sigNum || p.sigDen != sigDen))
                d += 4.0f;
            // A bar someone actually played beats one re-voiced to fill a
            // corner of the pad, unless the recorded take is far away.
            if (p.derived)
                d += 0.03f;
            return d;
        };

        const int n = std::min<int> (maxResults, (int) idx.size());
        std::partial_sort (idx.begin(), idx.begin() + n, idx.end(),
                           [&] (int a, int b)
                           {
                               const float da = distance (a), db = distance (b);
                               return da != db ? da < db : a < b;
                           });
        idx.resize ((std::size_t) n);
        return idx;
    }

    std::vector<int> GrooveCorpus::neighbours (float complexity,
                                               float intensity,
                                               std::uint16_t charMask,
                                               int   bars,
                                               int   maxResults,
                                               int   sigNum,
                                               int   sigDen) const
    {
        if (beatPhrases.empty())
            return {};
        return rank (beatPhrases, complexity, intensity, charMask, bars, -1,
                     maxResults, sigNum, sigDen);
    }

    int GrooveCorpus::pickBeat (float complexity,
                                float intensity,
                                int   variation,
                                std::uint16_t charMask,
                                int   bars,
                                int   section,
                                int   sigNum,
                                int   sigDen) const
    {
        if (beatPhrases.empty())
            return -1;

        // 24 nearest real takes form the neighbourhood an XY position can
        // resolve to; the variation index walks that list.
        const auto ranked = rank (beatPhrases, complexity, intensity, charMask,
                                  bars, section, 24, sigNum, sigDen);
        if (ranked.empty())
            return -1;

        const int size = (int) ranked.size();
        const int v = ((variation % size) + size) % size;
        return ranked[(std::size_t) v];
    }

    int GrooveCorpus::pickFill (float complexity,
                                float intensity,
                                int   bars,
                                int   variation,
                                const std::vector<int>& avoid,
                                std::uint32_t laneMask,
                                std::uint8_t  styleMask,
                                int   sigNum,
                                int   sigDen,
                                std::uint16_t charMask) const
    {
        if (fillPhrases.empty())
            return -1;

        std::vector<int> candidates;
        candidates.reserve (fillPhrases.size());

        // Pass 0 wants this character's own fills, in the requested style and
        // metre; the later passes relax the metre, then the style, then the
        // character, so a narrow request still yields a fill rather than a
        // hole - but a character plays its own vocabulary wherever it has one.
        for (int pass = 0; pass < 4 && candidates.empty(); ++pass)
        {
            for (int i = 0; i < (int) fillPhrases.size(); ++i)
            {
                const auto& p = fillPhrases[(std::size_t) i];
                if (p.bars != bars)
                    continue;
                if (pass == 0 && sigNum > 0
                    && (p.sigNum != sigNum || p.sigDen != sigDen))
                    continue;
                if (pass < 2 && styleMask != 0 && (p.fillStyles & styleMask) == 0)
                    continue;
                if (pass < 3 && charMask != 0 && (p.charMask & charMask) == 0)
                    continue;

                int allowed = 0, total = 0;
                for (const auto& h : p.hits)
                {
                    ++total;
                    if ((laneMask & (1u << h.lane)) != 0)
                        ++allowed;
                }
                if (total == 0 || (float) allowed / (float) total < 0.8f)
                    continue;

                candidates.push_back (i);
            }
        }

        if (candidates.empty())
            return -1;

        std::sort (candidates.begin(), candidates.end(), [&] (int a, int b)
        {
            const auto& pa = fillPhrases[(std::size_t) a];
            const auto& pb = fillPhrases[(std::size_t) b];
            const float da = (pa.complexity - complexity) * (pa.complexity - complexity)
                           + (pa.intensity  - intensity)  * (pa.intensity  - intensity);
            const float db = (pb.complexity - complexity) * (pb.complexity - complexity)
                           + (pb.intensity  - intensity)  * (pb.intensity  - intensity);
            const float sa = da + (pa.derived ? 0.03f : 0.0f);
            const float sb = db + (pb.derived ? 0.03f : 0.0f);
            return sa != sb ? sa < sb : a < b;
        });

        const int pool = std::min<int> (16, (int) candidates.size());
        for (int step = 0; step < pool; ++step)
        {
            const int i = candidates[(std::size_t) (((variation % pool) + pool + step) % pool)];
            if (std::find (avoid.begin(), avoid.end(), i) == avoid.end())
                return i;
        }
        return candidates.front();
    }

    const std::uint8_t* GrooveCorpus::velocityRow (int lane, int fromBucket) const
    {
        if (velocityModel.empty() || lane < 0 || lane >= NumLanes)
            return nullptr;
        const int b = std::clamp (fromBucket, 0, kVelocityBuckets - 1);
        const std::size_t offset = ((std::size_t) lane * kVelocityBuckets
                                    + (std::size_t) b) * kVelocityBuckets;
        return velocityModel.data() + offset;
    }
}
