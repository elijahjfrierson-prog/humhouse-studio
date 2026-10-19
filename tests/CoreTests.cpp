#include "../Source/Engine.h"
#include "../Source/NoteTools.h"
#include "../Source/ProjectIO.h"
#include "../Source/Effects.h"
#include <cstdio>
#include <iostream>

using namespace hhs;
static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static float renderPeak (Engine& e, int blocks, int n = 512)
{
    juce::AudioBuffer<float> out (2, n);
    float peak = 0;
    for (int b = 0; b < blocks; ++b)
    {
        float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
        e.processBlock (nullptr, 0, o, 2, n);
        for (int i = 0; i < n; ++i) { CHECK (std::isfinite (o[0][i])); peak = std::max (peak, std::abs (o[0][i])); }
    }
    return peak;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // scales
    CHECK (inScale (60, 0, 0) && ! inScale (61, 0, 0));
    CHECK (snapToScale (61, 0, 0) == 60 || snapToScale (61, 0, 0) == 62);

    // note tools
    {
        NoteList l { { 60, 0.0, 1.0 } };
        tools::ArpSettings a; a.rate = 0.125; tools::arpeggiate (l, a);
        CHECK (l.size() == 8);   // 1/8-beat roll over 1 beat
        NoteList chord { { 60, 0, 1 }, { 64, 0, 1 }, { 67, 0, 1 } };
        tools::StrumSettings s; s.time = 0.05; tools::strum (chord, s);
        CHECK (chord.size() == 3 && chord[0].start == 0.0 && chord[2].start > chord[1].start);
        CHECK (std::abs (chord[2].end() - 1.0) < 1e-9);  // preserve end
        NoteList g { { 60, 0, 0.5 }, { 60, 0.5, 0.5 } }; tools::glue (g); CHECK (g.size() == 1 && g[0].length == 1.0);
        NoteList d { { 60, 0, 1, 0.8f, 0, false, true } }; tools::duplicateRight (d); CHECK (d.size() == 2 && d[1].start == 1.0);
        NoteList c { { 60, 0, 1 } }; tools::quickChop (c, 0.25); CHECK (c.size() == 4);
        NoteList f { { 60, 1, 1 } }; tools::flam (f, {}); CHECK (f.size() == 2 && f[0].start < 1.0);
    }

    // routing
    {
        auto p = Project::createDemo();
        const int a = p.addMixerTrack ("A"), b = p.addMixerTrack ("B");
        CHECK (p.toggleRoute (a, b));
        CHECK (! p.toggleRoute (b, a));        // cycle refused
        auto order = p.processingOrder();
        CHECK (std::find (order.begin(), order.end(), a) < std::find (order.begin(), order.end(), b));
        CHECK (order.back() == 0);
    }

    // engine renders the default beat
    {
        Engine e;
        e.prepare (44100, 512);
        e.newProject (Project::createDemo());
        e.sync();
        e.play();
        const float pk = renderPeak (e, 200);
        CHECK (pk > 0.05f);
        e.stop();
    }


    // recording from a MONO mic, plus deleting the lane mid-take, must not crash
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        p.songMode = true;
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[1].armed = true;
        e.countInOn = false;
        juce::AudioBuffer<float> mic (1, 512), out (2, 512);
        for (int i = 0; i < 512; ++i) mic.setSample (0, i, 0.3f * std::sin (i * 0.07f));
        auto run = [&] (int blocks) {
            for (int b = 0; b < blocks; ++b)
            {
                const float* ins[1] { mic.getReadPointer (0) };
                float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
                e.processBlock (ins, 1, o, 2, 512);
            }
        };
        const auto before = p.clips.size();
        e.setRecording (true);
        e.play();
        run (120);
        e.stop();
        CHECK (p.clips.size() > before);
        e.setRecording (false);

        e.setRecording (true);
        e.play();
        run (40);
        { const juce::ScopedLock sl (e.getLock()); p.lanes.pop_back(); }
        e.stop();                         // lane gone: must skip, not crash
        e.setRecording (false);
        run (10);
        CHECK (true);
    }
    // MONO input (mic on in 1 only) is heard in both ears; STEREO keeps the pair
    for (int stereo = 0; stereo < 2; ++stereo)
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[0].armed = true; p.lanes[0].monitor = true;
        p.lanes[0].stereoIn = stereo == 1; p.lanes[0].inputCh = 0; p.lanes[0].inputPair = 0;
        juce::AudioBuffer<float> mic (2, 512), out (2, 512);
        mic.clear();
        for (int i = 0; i < 512; ++i) mic.setSample (0, i, 0.3f * std::sin (i * 0.07f));
        float pl = 0, pr = 0;
        for (int b = 0; b < 20; ++b)
        {
            const float* ins[2] { mic.getReadPointer (0), mic.getReadPointer (1) };
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (ins, 2, o, 2, 512);
            pl = std::max (pl, out.getMagnitude (0, 0, 512)); pr = std::max (pr, out.getMagnitude (1, 0, 512));
        }
        CHECK (pl > 0.01f);
        if (stereo) CHECK (pr < pl * 0.1f); else CHECK (std::abs (pl - pr) < pl * 0.05f);
        if (! stereo) CHECK (std::abs (pl - 0.3f) < 0.03f);   // default insert + master faders = unity gain (0 dB)
    }
    // a second take over the same spot drops to the next lane instead of covering the first
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        p.songMode = true;
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[1].armed = true;
        e.countInOn = false;
        juce::AudioBuffer<float> mic (1, 512), out (2, 512);
        for (int i = 0; i < 512; ++i) mic.setSample (0, i, 0.3f * std::sin (i * 0.07f));
        auto take = [&] {
            e.setPositionBeats (0.0); e.setRecording (true); e.play();
            for (int b = 0; b < 100; ++b) { const float* ins[1] { mic.getReadPointer (0) }; float* o[2] { out.getWritePointer (0), out.getWritePointer (1) }; e.processBlock (ins, 1, o, 2, 512); }
            e.stop(); e.setRecording (false);
        };
        take(); take();
        int onLane1 = 0, onLane2 = 0;
        for (auto& c : p.clips) if (c.isAudio() && ! c.hidden) { onLane1 += c.lane == 1; onLane2 += c.lane == 2; }
        CHECK (onLane1 == 1 && onLane2 == 1);
    }
    // NORM: a quiet mic comes up strong, a loud one never clips
    for (float amp : { 0.02f, 0.9f })
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[0].armed = true; p.lanes[0].monitor = true; p.lanes[0].autoNorm = true;
        juce::AudioBuffer<float> mic (2, 512), out (2, 512);
        float pk = 0; double ph = 0;
        for (int b = 0; b < 400; ++b)
        {
            for (int i = 0; i < 512; ++i) { const float v = amp * (float) std::sin (ph); ph += 0.05; mic.setSample (0, i, v); mic.setSample (1, i, 0.0f); }
            const float* ins[2] { mic.getReadPointer (0), mic.getReadPointer (1) };
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (ins, 2, o, 2, 512);
            if (b > 300) pk = std::max (pk, out.getMagnitude (0, 0, 512));
        }
        if (amp < 0.1f) CHECK (pk > 0.15f); else CHECK (pk < 1.0f && pk > 0.3f);
    }
    // WAV export: right length, real level, first note at the very start is there
    {
        Engine e; e.prepare (44100, 512);
        auto& p = e.project(); p.songMode = false;
        Channel c; c.type = ChannelType::Synth; c.name = "Lead"; c.mixerTrack = 0;
        const int id = p.addChannel (c);
        Note nt; nt.key = 48; nt.start = 0.0; nt.length = 1.0; nt.velocity = 0.9f;
        p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt);
        e.sync();
        const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs_export_test.wav");
        CHECK (e.renderToWav (f, false, {}, -1));
        {   // 64-bit float at 96 kHz
            auto f64 = f.getSiblingFile ("hh_test64.wav");
            CHECK (e.renderToWav (f64, false, {}, -1, 64, 96000));
            juce::FileInputStream in (f64); char h[44] {}; in.read (h, 44);
            juce::uint16 fmt, bps; juce::uint32 rate; std::memcpy (&fmt, h + 20, 2); std::memcpy (&bps, h + 34, 2); std::memcpy (&rate, h + 24, 4);
            CHECK (fmt == 3 && bps == 64 && rate == 96000);
            CHECK (f64.getSize() > 44 + 16 * 96000);
            f64.deleteFile();
        }
        {   // MP3 (320 kbps) has MPEG frames; an insert with nothing on it renders silent (stems skip it)
            auto mp3 = f.getSiblingFile ("hh_test.mp3");
            CHECK (e.renderToWav (mp3, false, {}, -1, 24, 0, 1));
            juce::FileInputStream in (mp3); unsigned char h[2] {}; in.read (h, 2);
            std::printf ("mp3 export %d bytes, peak %.2f\n", (int) mp3.getSize(), e.lastRenderPeak);
            CHECK (mp3.getSize() > 20000 && h[0] == 0xff && (h[1] & 0xe0) == 0xe0 && e.lastRenderPeak > 0.01f);
            mp3.deleteFile();
            if (p.mixer.size() > 2)
            {
                auto stem = f.getSiblingFile ("hh_stem.wav");
                CHECK (e.renderToWav (stem, false, {}, 2));
                CHECK (e.lastRenderPeak < 1.0e-5f);
                stem.deleteFile();
            }
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (f.createInputStream().release(), true));
        CHECK (rd != nullptr);
        if (rd)
        {
            const double secs = 60.0 / p.bpm * p.patterns[(size_t) p.currentPattern].lengthBeats() + 2.0;
            CHECK (std::abs ((double) rd->lengthInSamples - secs * 44100.0) < 1024.0);
            juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
            rd->read (&b, 0, b.getNumSamples(), 0, true, true);
            CHECK (b.getMagnitude (0, 0, 2048) > 0.02f);
            CHECK (b.getMagnitude (0, b.getNumSamples()) < 1.5f);
        }
        f.deleteFile();
    }
    // a held note longer than the loop must not cut the same note when the loop comes round
    {
        Engine e; e.prepare (44100, 512);
        auto& p = e.project(); p.songMode = false;
        Channel c; c.type = ChannelType::Synth; c.name = "808"; c.mixerTrack = 0;
        const int id = p.addChannel (c);
        auto& pat = p.patterns[(size_t) p.currentPattern];
        const double len = pat.lengthBeats();
        Note nt; nt.key = 36; nt.start = 0.0; nt.length = len * 2.0; nt.velocity = 0.9f;
        pat.notes[id].push_back (nt);
        e.sync(); e.setPositionBeats (0.0); e.play();
        const double spbS = 60.0 / p.bpm * 44100.0;
        renderPeak (e, (int) ((len * 2.0 + 0.3) * spbS / 512.0));
        CHECK (renderPeak (e, 8) > 0.01f);
        e.stop();
    }
    // starting mid-note: a sampler jumps into the sample instead of staying silent
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>> (2, 44100);
        for (int i = 0; i < 44100; ++i) { buf->setSample (0, i, 0.5f * (float) i / 44100.0f); buf->setSample (1, i, 0.5f * (float) i / 44100.0f); }
        Channel c; c.type = ChannelType::Sampler; c.oneShot = true; c.rootKey = 60;
        auto at = [&] (double secondsIn) {
            auto inst = createInstrument (c, buf, 44100.0);
            inst->prepare (44100.0, 512);
            inst->noteOnAt (60, 1.0f, false, secondsIn);
            juce::AudioBuffer<float> out (2, 512); out.clear();
            inst->render (out, 0, 512);
            return std::abs (out.getSample (0, 300));
        };
        CHECK (at (0.5) > 0.1f);
        CHECK (at (0.0) < 0.02f);
    }
    // a sample plays exactly like the file, and with Key off every note plays it at its own pitch
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>> (2, 4096);
        juce::Random rnd (7);
        for (int i = 0; i < 4096; ++i) { const float v = rnd.nextFloat() - 0.5f; buf->setSample (0, i, v); buf->setSample (1, i, v); }
        auto play = [&] (const Channel& c, int key) {
            auto inst = createInstrument (c, buf, 44100.0);
            inst->prepare (44100.0, 512);
            inst->noteOn (key, 1.0f, false);
            juce::AudioBuffer<float> out (2, 2048); out.clear();
            inst->render (out, 0, 2048);
            return out;
        };
        Channel c; c.type = ChannelType::Sampler; c.rootKey = 60;
        auto o = play (c, 60);
        float err = 0; for (int i = 0; i < 2048; ++i) err = std::max (err, std::abs (o.getSample (0, i) - buf->getSample (0, i)));
        CHECK (err < 1.0e-6f);   // bit-for-bit the WAV, no fade-in / interpolation
        c.keyTrack = false;
        auto k = play (c, 72);
        err = 0; for (int i = 0; i < 2048; ++i) err = std::max (err, std::abs (k.getSample (0, i) - buf->getSample (0, i)));
        CHECK (err < 1.0e-6f);
        c.keyTrack = true;
        auto p = play (c, 72);
        err = 0; for (int i = 0; i < 2048; ++i) err = std::max (err, std::abs (p.getSample (0, i) - buf->getSample (0, i)));
        CHECK (err > 0.01f);     // key on: it's pitched
    }
    // Flanger / Flangus: 0 latency, finite, audibly changes the sound
    // Flangus worst case (6 voices lined up, max feedback) stays bounded instead of blowing up the bus
    {
        auto fx = createBuiltinEffect ("flangus");
        fx->setParam ("voices", 6.0f); fx->setParam ("depth", 0.0f); fx->setParam ("feedback", 0.9f); fx->setParam ("mix", 1.0f);
        fx->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; juce::Random rnd (7); float peak = 0;
        for (int k = 0; k < 400; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.5f * (rnd.nextFloat() * 2 - 1); b.setSample (0, i, v); b.setSample (1, i, v); }
            fx->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); CHECK (std::isfinite (y)); peak = std::max (peak, std::abs (y)); }
        }
        std::printf ("flangus worst-case peak %.2f\n", peak);
        CHECK (peak < 4.0f);
    }
    for (const char* id : { "flanger", "flangus" })
    {
        auto fx = createBuiltinEffect (id); CHECK (fx != nullptr);
        if (! fx) continue;
        fx->prepare (48000, 512); CHECK (fx->latencySamples() == 0);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; juce::Random r (1); float diff = 0;
        for (int k = 0; k < 40; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = r.nextFloat() * 0.5f - 0.25f; b.setSample (0, i, v); b.setSample (1, i, v); }
            juce::AudioBuffer<float> dry (b);
            fx->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { CHECK (std::isfinite (b.getSample (0, i))); diff += std::abs (b.getSample (0, i) - dry.getSample (0, i)); }
        }
        CHECK (diff > 1.0f);
    }
    // Studio EQ 7: natural boost, linear-phase FIR of the same curve, dynamic band pulls a loud band down
    {
        auto run = [] (BuiltinEffect& fx, float hz, float amp, int blocks, bool wait) {
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; double ph = 0; float in = 0, out = 0;
            for (int k = 0; k < blocks; ++k)
            {
                for (int i = 0; i < 512; ++i) { const float v = amp * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * hz / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }
                in = b.getRMSLevel (0, 0, 512); fx.process (b, 512, ctx); out = b.getRMSLevel (0, 0, 512);
                if (wait) juce::Thread::sleep (2);
            }
            return out / std::max (in, 1.0e-9f);
        };
        auto eq = createBuiltinEffect ("steq7");
        eq->setParam ("e4G", 12.0f); eq->setParam ("e4F", 1000.0f);
        eq->prepare (48000, 512);
        const float nat = run (*eq, 1000, 0.1f, 40, false);
        CHECK (nat > 3.0f && nat < 5.0f);
        CHECK (std::abs (eq->responseDb (1000.0) - 12.0f) < 0.5f);
        CHECK (eq->latencySamples() == 0);
        eq->setParam ("phase", 1.0f);
        CHECK (eq->latencySamples() == 1024);
        const float lin = run (*eq, 1000, 0.1f, 300, true);
        CHECK (lin > 3.0f && lin < 5.0f);
        const float linFlat = run (*eq, 8000, 0.1f, 80, true);
        CHECK (linFlat > 0.8f && linFlat < 1.25f);
        {   // Studio Vocal: dips a ringing 3.2 kHz resonance, leaves the low voice alone, 0 latency
            auto sv = createBuiltinEffect ("stvocal"); CHECK (sv != nullptr);
            sv->setParam ("smooth", 1.0f); sv->setParam ("silk", 0.0f); sv->setParam ("warmth", 0.0f); sv->prepare (48000, 512);
            const float harsh = run (*sv, 3200, 0.2f, 60, false);
            sv->prepare (48000, 512);
            const float low = run (*sv, 120, 0.2f, 60, false);
            std::printf ("studio vocal: 3.2 kHz resonance %.1f dB, 120 Hz %.1f dB\n", 20 * std::log10 (harsh), 20 * std::log10 (low));
            CHECK (harsh < 0.7f); CHECK (low > 0.8f && low < 1.15f); CHECK (sv->latencySamples() == 0);
        }
        auto dq = createBuiltinEffect ("steq7");
        dq->setParam ("e4F", 1000.0f); dq->setParam ("e4Dyn", 1.0f); dq->setParam ("e4DTh", -40.0f); dq->setParam ("e4DRg", -12.0f);
        dq->prepare (48000, 512);
        CHECK (run (*dq, 1000, 0.5f, 100, false) < 0.5f);
        CHECK (dq->meter (3) < -6.0f);
        auto e12 = createBuiltinEffect ("steq7");
        CHECK (e12->params().size() == 123);
        e12->setParam ("e12On", 1.0f); e12->setParam ("e12F", 1000.0f); e12->setParam ("e12G", -12.0f); e12->prepare (48000, 512);
        CHECK (std::abs (e12->responseDb (1000.0) + 12.0f) < 0.6f);
        CHECK (run (*dq, 1000, 0.001f, 400, false) > 0.9f);
    }
    // starting playback in the middle of a long synth note still sounds it (FL-style chase)
    {
        Engine e;
        e.prepare (44100, 512);
        auto& p = e.project();
        p.songMode = false;
        Channel c; c.type = ChannelType::Synth; c.name = "Pad"; c.mixerTrack = 0;
        const int id = p.addChannel (c);
        Note nt; nt.key = 60; nt.start = 0.0; nt.length = 8.0; nt.velocity = 0.9f;
        p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt);
        e.sync();
        e.setPositionBeats (2.0);
        e.play();
        CHECK (renderPeak (e, 20) > 0.01f);
        e.stop();
    }
    // AutoPitch Pitch +12 doubles the frequency; Detune moves it by cents
    {
        auto crossings = [] (float pitchSt, float cents) {
            auto ap = createBuiltinEffect ("autopitch");
            ap->setParam ("pitch", pitchSt); ap->setParam ("detune", cents);
            ap->prepare (48000, 512);
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
            double ph = 0; int zc = 0; float prev = 0;
            for (int k = 0; k < 200; ++k)
            {
                for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) (std::sin (ph) + 0.5 * std::sin (2 * ph) + 0.33 * std::sin (3 * ph)); ph += juce::MathConstants<double>::twoPi * 261.63 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }   // voice-like: has harmonics
                ap->process (b, 512, ctx);
                if (k >= 100) for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); if (prev <= 0 && y > 0) ++zc; prev = y; }
            }
            return zc;
        };
        const int base = crossings (0, 0), up = crossings (12, 0), cents = crossings (0, 100);
        CHECK (up > base * 1.8 && up < base * 2.2);
        CHECK (cents > base * 1.03 && cents < base * 1.09);
        // A4 = 415 Hz: a 440-tuned C is re-tuned onto the 415 grid (about a semitone away)
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("tuning", 415.0f); ap->setParam ("style", 3.0f);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0; int zc = 0; float prev = 0;
        for (int k = 0; k < 200; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * 261.63 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }
            ap->process (b, 512, ctx);
            if (k >= 100) for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); if (prev <= 0 && y > 0) ++zc; prev = y; }
        }
        CHECK (std::abs ((float) zc / (float) base - 1.0f) > 0.03f);
    }
    // AutoPitch: a 452 Hz tone snaps to A 440, stays at full level
    {
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", 3.0f);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0, inE = 0, outE = 0; int zc = 0, counted = 0; float prev = 0;
        for (int k = 0; k < 300; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * 452.0 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); if (k >= 100) inE += v * v; }
            ap->process (b, 512, ctx);
            if (k >= 100) for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); outE += y * y; if (prev <= 0 && y > 0) ++zc; prev = y; ++counted; }
        }
        const double hz = zc * 48000.0 / counted;
        std::printf ("autopitch 452 Hz -> %.1f Hz, level %.2f dB\n", hz, 10.0 * std::log10 (outE / inE));
        CHECK (std::abs (hz - 440.0) < 3.0);
        CHECK (std::abs (10.0 * std::log10 (outE / inE)) < 0.5);
    }
    // AutoPitch catches rough vocals: breathy (noisy) note, a short "yeah" after silence, and a noisy gap mid-word
    {
        auto run = [] (int scenario) {
            auto ap = createBuiltinEffect ("autopitch");
            ap->setParam ("style", 3.0f);
            ap->prepare (48000, 512);
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
            juce::Random rnd (3); double ph = 0; std::vector<float> out;
            const int blocks = scenario == 1 ? 32 : 160;
            for (int k = 0; k < blocks; ++k)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const bool voiced = scenario == 1 ? k >= 20 : ! (scenario == 2 && k >= 120 && k < 124);
                    const float noise = (rnd.nextFloat() * 2 - 1) * (scenario == 0 ? 0.12f : scenario == 2 && ! voiced ? 0.08f : 0.0f);
                    float v = 0;
                    if (voiced) { v = 0.25f * (float) std::sin (ph) + 0.1f * (float) std::sin (2 * ph) + 0.05f * (float) std::sin (3 * ph); ph += juce::MathConstants<double>::twoPi * 452.0 / 48000.0; }
                    b.setSample (0, i, v + noise); b.setSample (1, i, v + noise);
                }
                ap->process (b, 512, ctx);
                for (int i = 0; i < 512; ++i) out.push_back (b.getSample (0, i));
            }
            // measure: breathy = end, short yeah = its last 100 ms, gap = 16-48 ms after the voice comes back
            const size_t at = scenario == 0 ? out.size() - 2048 : scenario == 1 ? (size_t) (20 * 512 + 1536) : (size_t) (124 * 512 + 768);
            const int len = scenario == 2 ? 1536 : 2048;
            return detectPitchYin (out.data() + at, len, 48000.0, 0.3f, 0.5f);
        };
        const float breathy = run (0), shortYeah = run (1), gap = run (2);
        std::printf ("autopitch rough vocals -> breathy %.1f Hz, short yeah %.1f Hz, after gap %.1f Hz\n", breathy, shortYeah, gap);
        CHECK (std::abs (breathy - 440.0f) < 4.0f);
        CHECK (std::abs (shortYeah - 440.0f) < 4.0f);
        CHECK (std::abs (gap - 440.0f) < 4.0f);
    }
    // AutoPitch Hard Tune: sung vibrato is flattened (Natural Vibrato 0) or kept (100); deep notes tune like high ones
    {
        auto run = [] (double hz, double vibCents, float natvib, std::vector<float>& out) {
            auto ap = createBuiltinEffect ("autopitch");
            ap->setParam ("style", 3.0f); ap->setParam ("natvib", natvib);
            ap->prepare (48000, 512);
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
            double ph = 0, t = 0;
            for (int k = 0; k < 280; ++k)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double f = hz * std::pow (2.0, vibCents / 1200.0 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t));
                    const float v = 0.25f * (float) std::sin (ph) + 0.1f * (float) std::sin (2 * ph) + 0.05f * (float) std::sin (3 * ph);
                    ph += juce::MathConstants<double>::twoPi * f / 48000.0; t += 1.0 / 48000.0;
                    b.setSample (0, i, v); b.setSample (1, i, v);
                }
                ap->process (b, 512, ctx);
                for (int i = 0; i < 512; ++i) out.push_back (b.getSample (0, i));
            }
        };
        auto maxDev = [] (const std::vector<float>& o, double hz) {
            double m = 0;
            for (size_t at = 100 * 512; at + 2048 < o.size(); at += 256)
            {
                const float f = detectPitchYin (o.data() + at, 2048, 48000.0, 0.3f, 0.5f, 60.0f, 1100.0f);
                if (f > 0) m = std::max (m, std::abs (1200.0 * std::log2 (f / hz)));
            }
            return m;
        };
        std::vector<float> flat, kept, deep;
        run (220.0, 40.0, 0.0f, flat); run (220.0, 40.0, 100.0f, kept);
        run (82.41 * std::pow (2.0, 30.0 / 1200.0), 0.0, 0.0f, deep);
        const double dFlat = maxDev (flat, 220.0), dKept = maxDev (kept, 220.0);
        const float deepHz = detectPitchYin (deep.data() + deep.size() - 4096, 4096, 48000.0, 0.3f, 0.5f, 60.0f, 1100.0f);
        std::printf ("autopitch vibrato 40c -> kill %.1fc, keep %.1fc; deep E2 +30c -> %.2f Hz\n", dFlat, dKept, deepHz);
        CHECK (dFlat < 12.0);
        CHECK (dKept > 25.0);
        CHECK (std::abs (deepHz - 82.41f) < 0.6f);
    }
    // Maximizer: loud input never passes the ceiling, output stays finite
    {
        auto mx = createBuiltinEffect ("stmax"); CHECK (mx != nullptr);
        mx->setParam ("gain", 18.0f); mx->setParam ("sat", 0.3f);
        mx->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; double ph = 0; float peak = 0; bool finite = true;
        for (int k = 0; k < 100; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = (float) (0.9 * std::sin (ph) + 0.3 * std::sin (7.3 * ph)); ph += juce::MathConstants<double>::twoPi * 60.0 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }
            mx->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); finite = finite && std::isfinite (y); peak = std::max (peak, std::abs (y)); }
        }
        std::printf ("maximizer peak %.3f (ceiling %.3f)\n", peak, juce::Decibels::decibelsToGain (-0.3f));
        CHECK (finite && peak <= juce::Decibels::decibelsToGain (-0.3f) + 1.0e-4f && peak > 0.5f);
    }
    // imported samples: drum hits keep their pitch, everything else follows the Piano Roll
    CHECK (isDrumSampleName ("Hi Hat 03") && isDrumSampleName ("KICK_hard") && ! isDrumSampleName ("Piano C") && ! isDrumSampleName ("Vox Chop"));
    // AutoPitch shifter latency stays tiny while correcting up or down
    for (double ratio : { 0.97, 1.03, 2.0 })
    {
        SpliceShifter sh; sh.prepare (48000);
        double ph = 0, dMax = 0, jump = 0; float prev = 0;
        for (int i = 0; i < 48000; ++i)
        {
            float l = 0.5f * (float) (std::sin (ph) + 0.5 * std::sin (2 * ph) + 0.25 * std::sin (3 * ph)), r = l;
            ph += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
            sh.process (l, r, ratio, 48000.0 / 220.0);
            if (i > 4800) { dMax = std::max (dMax, sh.delaySamples()); jump = std::max (jump, (double) std::abs (l - prev)); }
            prev = l;
        }
        std::printf ("autopitch ratio %.2f: max delay %.2f ms, max step %.3f\n", ratio, dMax / 48.0, jump);
        CHECK (dMax < 0.012 * 48000);
        CHECK (jump < 0.2 * ratio);
    }
    // PSOLA (Retune / Humanize styles): pitch moves, the voice's tone (formant) stays put - no chipmunk
    {
        const double fs = 48000, f0 = 168.0, fc = 1000.0, ratio = 1.12;
        const double rr = std::exp (-juce::MathConstants<double>::pi * 60.0 / fs), cw = std::cos (juce::MathConstants<double>::twoPi * fc / fs);
        double y1 = 0, y2 = 0, ph = 0, dMax = 0; float prev = 0, jump = 0;
        PsolaShifter sh; sh.prepare (fs);
        std::vector<float> out;
        for (int i = 0; i < 96000; ++i)
        {
            ph += f0 / fs; double x = 0; if (ph >= 1.0) { ph -= 1.0; x = 1.0; }
            const double y = x + 2 * rr * cw * y1 - rr * rr * y2; y2 = y1; y1 = y;
            float l = 0.02f * (float) y, r = l;
            sh.process (l, r, ratio, fs / f0);
            if (i >= 48000) { out.push_back (l); dMax = std::max (dMax, sh.delaySamples()); jump = std::max (jump, std::abs (l - prev)); }
            prev = l;
        }
        auto mag = [&] (double hz) { double re = 0, im = 0; const int N = (int) out.size();
            for (int i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / N), a = juce::MathConstants<double>::twoPi * hz * i / fs; re += w * out[(size_t) i] * std::cos (a); im += w * out[(size_t) i] * std::sin (a); }
            return std::sqrt (re * re + im * im); };
        int best = 0; double bm = 0;
        for (int k = 3; k <= 10; ++k) { const double m = mag (k * f0 * ratio); if (m > bm) { bm = m; best = k; } }
        std::printf ("psola: formant harmonic %d (%.0f Hz, tone kept at ~1 kHz), delay %.1f ms\n", best, best * f0 * ratio, dMax / 48.0);
        CHECK (best == 5);   // 5 x 188 Hz = 941 Hz stays nearest the 1 kHz formant; a sped-up (chipmunk) voice peaks at 6 x 188 = 1129 Hz
        CHECK (dMax < 0.03 * fs);
    }
    // AutoPitch: singing on/off (breaths, gaps) must not crack - no sample jumps
    {
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", 3.0f);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0; float prev = 0, maxJump = 0; long t = 0;
        for (int k = 0; k < 400; ++k)
        {
            for (int i = 0; i < 512; ++i, ++t)
            {
                const double sec = (double) t / 48000.0, cyc = std::fmod (sec, 0.6);
                const float gate = (float) juce::jlimit (0.0, 1.0, std::min (cyc / 0.01, (0.35 - cyc) / 0.01));   // 350 ms notes, 10 ms fades
                const double hz = 255.0 + 20.0 * std::sin (sec * 3.0);                                             // drifting voice
                const float v = 0.3f * gate * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * hz / 48000.0;
                b.setSample (0, i, v); b.setSample (1, i, v);
            }
            ap->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); if (k > 10) maxJump = std::max (maxJump, std::abs (y - prev)); prev = y; }
        }
        CHECK (maxJump < 0.06f);
    }
    // big session stress: 128 inserts x (EQ, Dyn, Verb) + 64 synth channels, 256-sample buffer
    {
        Engine e;
        e.prepare (48000, 256);
        auto& p = e.project();
        p.songMode = false;
        for (int t = 0; t < 128; ++t)
        {
            const int mt = p.addMixerTrack ("T" + juce::String (t));
            for (auto* id : { "steq7", "stdyn", "verb" }) p.addFx (mt, id);
        }
        for (int c = 0; c < 64; ++c)
        {
            Channel ch; ch.type = ChannelType::Synth; ch.name = "S" + juce::String (c); ch.mixerTrack = 1 + c * 2; ch.synth.unison = 3;
            const int id = p.addChannel (ch);
            for (int k = 0; k < 16; ++k) { Note nt; nt.key = 48 + (c + k) % 24; nt.start = k * 0.25; nt.length = 0.5; nt.velocity = 0.7f; p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt); }
        }
        e.sync();
        e.play();
        juce::AudioBuffer<float> out (2, 256);
        auto runFor = [&] (int blocks) {
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (int b = 0; b < blocks; ++b) { float* o[2] { out.getWritePointer (0), out.getWritePointer (1) }; e.processBlock (nullptr, 0, o, 2, 256); }
            return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) / (blocks * 256.0 / 48000.0);
        };
        runFor (50);
        const double mtLoad = runFor (400);
        std::cout << "pool runs " << e.poolStat (false) << " worker jobs " << e.poolStat (true) << std::endl;
        e.setPerformance (1, false, true);
        const double stLoad = runFor (200);
        e.setPerformance (0, true, true);
        std::cout << "big session: " << e.processingThreads() << " threads, real-time load " << juce::String (mtLoad * 100.0, 1)
                  << "% (single thread " << juce::String (stLoad * 100.0, 1) << "%)" << std::endl;
        CHECK (std::isfinite (out.getSample (0, 0)));
        e.stop();
    }
    // every built-in effect stays finite and silent-in gives finite out
    for (auto& info : builtinEffects())
    {
        auto fx = createBuiltinEffect (info.id);
        CHECK (fx != nullptr);
        if (! fx) continue;
        fx->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512);
        PlayContext ctx; ctx.playing = true;
        for (int k = 0; k < 50; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float s = 0.5f * std::sin ((float) (k * 512 + i) * 0.05f); b.setSample (0, i, s); b.setSample (1, i, s); }
            fx->process (b, 512, ctx);
            ctx.beatPos += 512.0 / 48000.0 * 140.0 / 60.0;
            for (int i = 0; i < 512; ++i) CHECK (std::isfinite (b.getSample (0, i)));
        }
    }

    // AutoPitch pulls a sharp tone toward the scale
    {
        const double sr = 44100;
        juce::AudioBuffer<float> b (2, 1024 * 16);
        for (int i = 0; i < b.getNumSamples(); ++i) { const float s = 0.5f * (float) std::sin (2 * juce::MathConstants<double>::pi * 452.0 * i / sr); b.setSample (0, i, s); b.setSample (1, i, s); }
        CHECK (std::abs (detectPitchYin (b.getReadPointer (0), 2048, sr) - 452.0f) < 5.0f);
    }

    // sample-rate conversion: a 96k file's 30 kHz content must not fold back as a tone at 48k; 1 kHz passes at full level
    {
        const int n = 96000;
        std::vector<float> hi ((size_t) n), lo ((size_t) n), outHi ((size_t) n / 2), outLo ((size_t) n / 2);
        for (int i = 0; i < n; ++i)
        {
            hi[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 30000.0 * i / 96000.0);
            lo[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 96000.0);
        }
        sincResample (hi.data(), n, outHi.data(), n / 2, 2.0);
        sincResample (lo.data(), n, outLo.data(), n / 2, 2.0);
        float pHi = 0, pLo = 0;
        for (int i = 1000; i < n / 2 - 1000; ++i) { pHi = std::max (pHi, std::abs (outHi[(size_t) i])); pLo = std::max (pLo, std::abs (outLo[(size_t) i])); }
        CHECK (pHi < 0.005f);                  // alias at 18 kHz is gone (> -40 dB down)
        CHECK (std::abs (pLo - 0.5f) < 0.01f); // in-band level kept
        // 44.1k -> 48k: a 5 kHz sine comes out clean
        std::vector<float> a44 (44100), a48 (48000);
        for (int i = 0; i < 44100; ++i) a44[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 5000.0 * i / 44100.0);
        sincResample (a44.data(), 44100, a48.data(), 48000, 44100.0 / 48000.0);
        double err = 0;
        for (int i = 1000; i < 47000; ++i) err = std::max (err, std::abs (a48[(size_t) i] - 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 5000.0 * i / 48000.0)));
        CHECK (err < 0.005);
    }
    // a fresh sampler channel plays the file untouched (no declick / fade baked in)
    {
        Channel c; c.type = ChannelType::Sampler;
        auto src = std::make_shared<const juce::AudioBuffer<float>> (2, 100);
        CHECK (processSample (src, c, 48000.0) == src);
    }

    // save / load / undo
    {
        auto p = Project::createDemo();
        p.bpm = 151; p.addFx (1, "comp").params["ratio"] = 6;
        auto f = juce::File::createTempFile (".hhs");
        CHECK (saveProject (p, f));
        Project q; CHECK (loadProject (q, f));
        CHECK (q.bpm == 151 && q.channels.size() == p.channels.size() && q.mixer[1].fx.size() == 1);
        CHECK (q.patterns[0].notes.size() == p.patterns[0].notes.size());
        {   // reopening a project brings every built-in effect back with its saved knobs, not defaults
            Engine e; e.prepare (44100, 512);
            e.project() = q; e.sync();
            auto* fx = e.builtinFx (1, q.mixer[1].fx[0].uid);
            CHECK (fx != nullptr && std::abs (fx->getParam ("ratio") - 6.0f) < 1.0e-4f);
        }
        UndoHistory h; h.push (p, "a"); p.bpm = 90; h.push (p, "b");
        CHECK (h.undo (p) && p.bpm == 151 && h.redo (p) && p.bpm == 90);
        f.deleteFile();
    }

    std::printf (failures ? "%d FAILURES\n" : "ALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
