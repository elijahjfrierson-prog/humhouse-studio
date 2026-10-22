#include "UI.h"
#include <set>
#include "Drummer/DrummerCore.h"
#include "StemSplit.h"
#include "Instruments.h"

// FL-style clip stretch: renders the clip's audio to its own WAV in the song's Recordings folder (glued with the song).
// Resample = tape (speed and pitch together), Stretch = length only, pitch kept
static juce::File bakeClipStretch (const juce::File& src, double offSec, double lenSec, double factor, bool keepPitch, const juce::File& dir)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (src));
    if (rd == nullptr || rd->sampleRate <= 0) return {};
    const double sr = rd->sampleRate;
    const auto a = juce::jmax ((juce::int64) 0, (juce::int64) (offSec * sr));
    const int len = (int) juce::jmin ((juce::int64) (lenSec * sr), rd->lengthInSamples - a);
    if (len < 64) return {};
    const int nch = juce::jlimit (1, 2, (int) rd->numChannels);
    juce::AudioBuffer<float> b (nch, len + 16); b.clear();
    rd->read (&b, 0, len, a, true, nch > 1);
    juce::AudioBuffer<float> out;
    if (keepPitch) { b.setSize (nch, len, true); out = hhs::olaStretch (b, factor); out.setSize (nch, juce::jmax (1, (int) std::lround (len * factor)), true); }
    else
    {
        const int outLen = juce::jmax (1, (int) std::lround (len * factor));
        out.setSize (nch, outLen);
        for (int ch = 0; ch < nch; ++ch) { juce::LagrangeInterpolator li; li.process (1.0 / factor, b.getReadPointer (ch), out.getWritePointer (ch), outLen); }
    }
    dir.createDirectory();
    const auto f = dir.getNonexistentChildFile (src.getFileNameWithoutExtension() + (keepPitch ? "_stretch" : "_resample"), ".wav", false);
    juce::WavAudioFormat wav;
    auto* os = f.createOutputStream().release();
    if (os == nullptr) return {};
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os, sr, (unsigned int) nch, 24, {}, 0));
    if (w == nullptr) { delete os; return {}; }
    w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    w.reset();
    return f;
}

namespace hhs
{
namespace
{
constexpr int toolbarH = 32, headerW = 220, rulerH = 24;

juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager f;
    if (f.getNumKnownFormats() == 0) f.registerBasicFormats();
    return f;
}

double audioSeconds (const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats().createReaderFor (f));
    return r && r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
}

void autoCrossfade (Project& p)
{
    for (auto& a : p.clips)
        for (auto& b : p.clips)
        {
            if (&a == &b || ! a.isAudio() || ! b.isAudio() || a.hidden || b.hidden || a.lane != b.lane) continue;
            if (a.start < b.start && a.start + a.length > b.start && a.start + a.length < b.start + b.length)
            {
                const double ov = a.start + a.length - b.start;
                a.fadeOut = juce::jmax (a.fadeOut, ov);
                b.fadeIn = juce::jmax (b.fadeIn, ov);
            }
        }
}

static void deleteLanes (Project& p, juce::SortedSet<int> which)
{
    for (int k = which.size(); --k >= 0;)
    {
        const int i = which[k];
        if (i < 0 || i >= (int) p.lanes.size()) continue;
        p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [i] (const Clip& c) { return c.lane == i; }), p.clips.end());
        for (auto& c : p.clips) if (c.lane > i) --c.lane;
        p.lanes.erase (p.lanes.begin() + i);
    }
}

static void moveLane (Project& p, int from, int to)
{
    const int n = (int) p.lanes.size();
    if (from < 0 || from >= n) return;
    to = juce::jlimit (0, n - 1, to);
    if (to == from) return;
    const Lane l = p.lanes[(size_t) from];
    p.lanes.erase (p.lanes.begin() + from);
    p.lanes.insert (p.lanes.begin() + to, l);
    for (auto& c : p.clips)
    {
        if (c.lane == from) c.lane = to;
        else if (from < to && c.lane > from && c.lane <= to) --c.lane;
        else if (from > to && c.lane >= to && c.lane < from) ++c.lane;
    }
}

static void insertLane (Project& p, int at)
{
    at = juce::jlimit (0, (int) p.lanes.size(), at);
    for (auto& c : p.clips) if (c.lane >= at) ++c.lane;
    Lane l; l.name = "Track " + juce::String (p.lanes.size() + 1); l.mixerTrack = p.addMixerTrack (l.name);
    p.lanes.insert (p.lanes.begin() + at, l);
}

// Pro Tools style: lay a clip over a track, cutting what's under it (the rest keeps playing, 10 ms crossfades)
static void placeOver (Project& p, Clip piece, int lane)
{
    const double r0 = piece.start, r1 = piece.start + piece.length;
    std::vector<Clip> keep;
    for (auto& c : p.clips)
    {
        if (c.lane != lane || ! c.isAudio() || c.hidden || c.uid == piece.uid || c.start >= r1 || c.start + c.length <= r0) { keep.push_back (c); continue; }
        if (c.start < r0) { Clip x = c; x.uid = p.newUid(); x.length = r0 - c.start; x.fadeOut = 0.01; keep.push_back (x); }
        if (c.start + c.length > r1)
        { Clip y = c; y.uid = p.newUid(); y.sourceOffset += (r1 - c.start) * 60.0 / p.bpm; y.start = r1; y.length = c.start + c.length - r1; y.fadeIn = 0.01; keep.push_back (y); }
    }
    keep.erase (std::remove_if (keep.begin(), keep.end(), [&] (const Clip& c) { return c.uid == piece.uid; }), keep.end());
    piece.lane = lane; piece.hidden = false; piece.muted = false; piece.selected = false; piece.takeGroup = 0; piece.group = 0;
    piece.fadeIn = juce::jmax (piece.fadeIn, 0.01); piece.fadeOut = juce::jmax (piece.fadeOut, 0.01);
    keep.push_back (piece);
    p.clips = std::move (keep);
}

// Comp takes: one muted take lane under the track per take (every recorded file on it, hidden alternates and
// surprise takes too); pick pieces with "Use in comp" and they land on the track itself
static void makeCompLanes (Project& p, int lane)
{
    if (lane < 0 || lane >= (int) p.lanes.size()) return;
    std::vector<juce::File> files;
    for (auto& c : p.clips)
        if (c.lane == lane && c.isAudio() && std::find (files.begin(), files.end(), c.audioFile) == files.end()) files.push_back (c.audioFile);
    if (files.empty()) return;
    const int at = lane + 1, n = (int) files.size();
    for (auto& c : p.clips) if (c.lane >= at) c.lane += n;
    for (auto& l : p.lanes) if (l.compLane >= at) l.compLane += n;
    const auto base = p.lanes[(size_t) lane];
    for (int k = 0; k < n; ++k)
    {
        Lane l; l.name = "Take " + juce::String (k + 1) + " - " + base.name; l.mixerTrack = base.mixerTrack; l.muted = true; l.compLane = lane;
        l.colour = base.colour; l.height = juce::jmin (base.height, 44.0f);
        p.lanes.insert (p.lanes.begin() + at + k, l);
    }
    const auto n0 = p.clips.size();
    for (size_t i = 0; i < n0; ++i)
    {
        const auto c = p.clips[i];
        if (c.lane != lane || ! c.isAudio()) continue;
        Clip t = c; t.uid = p.newUid(); t.hidden = false; t.selected = false; t.takeGroup = 0; t.group = 0;
        t.lane = at + (int) (std::find (files.begin(), files.end(), c.audioFile) - files.begin());
        p.clips.push_back (t);
    }
}

// Exit comp: drop the take lanes under a comp track (the comp you built stays on the track)
static void exitComp (Project& p, int lane)
{
    for (int i = (int) p.lanes.size(); --i >= 0;)
    {
        if (p.lanes[(size_t) i].compLane != lane) continue;
        p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [i] (const Clip& c) { return c.lane == i; }), p.clips.end());
        for (auto& c : p.clips) if (c.lane > i) --c.lane;
        p.lanes.erase (p.lanes.begin() + i);
        for (auto& l : p.lanes) if (l.compLane > i) --l.compLane;
        if (i < lane) --lane;
    }
}

// ---------- HumTone (pitch of each note) and HumTime (warp hits to the grid): offline clip editors, no formant shift
static bool readClipAudio (const Clip& c, double bpm, juce::AudioBuffer<float>& out, double& sr)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (c.audioFile));
    if (r == nullptr || r->sampleRate <= 0) return false;
    sr = r->sampleRate;
    const auto from = (juce::int64) (juce::jmax (0.0, c.sourceOffset) * sr);
    const int len = (int) juce::jmin ((juce::int64) (beatsToSeconds (c.length, bpm) * sr), r->lengthInSamples - from);
    if (len <= 0) return false;
    out.setSize (2, len); out.clear();
    r->read (&out, 0, len, from, true, true);
    if (r->numChannels == 1) out.copyFrom (1, 0, out, 0, 0, len);
    return true;
}
static bool writeClipAudio (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    auto os = std::make_unique<juce::FileOutputStream> (f);
    if (! os->openedOk()) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (w == nullptr) return false;
    os.release();
    return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}
static void useRendered (Studio& s, int uid, const juce::File& f, const juce::String& what)
{
    s.edit (what, [uid, f] (Project& p) { for (auto& c : p.clips) if (c.uid == uid) { c.audioFile = f; c.sourceOffset = 0; } });
    s.engine.sync();
}

struct ToneNote { int a = 0, b = 0; float sung = 0, target = 0; };   // analysis frames [a, b)
constexpr int toneHop = 256, toneWin = 2048;
static std::vector<float> toneCurve (const juce::AudioBuffer<float>& b, double sr)
{
    std::vector<float> curve, w ((size_t) toneWin);
    for (int at = 0; at + toneWin <= b.getNumSamples(); at += toneHop)
    {
        double e = 0;
        for (int i = 0; i < toneWin; ++i) { w[(size_t) i] = 0.5f * (b.getSample (0, at + i) + b.getSample (1, at + i)); e += w[(size_t) i] * w[(size_t) i]; }
        const float hz = std::sqrt (e / toneWin) > 0.004 ? detectPitchYin (w.data(), toneWin, sr, 0.15f, 0.0f, 55.0f, 1100.0f) : 0.0f;
        curve.push_back (hz > 0.0f ? 69.0f + 12.0f * std::log2 (hz / 440.0f) : 0.0f);
    }
    return curve;
}
static std::vector<ToneNote> toneNotes (const std::vector<float>& cv)
{
    std::vector<ToneNote> out;
    const int n = (int) cv.size();
    for (int i = 0; i < n;)
    {
        if (cv[(size_t) i] <= 0.0f) { ++i; continue; }
        int j = i; float ref = cv[(size_t) i]; std::vector<float> seg;
        while (j < n && cv[(size_t) j] > 0.0f && std::abs (cv[(size_t) j] - ref) < 0.8f) { seg.push_back (cv[(size_t) j]); ref += 0.2f * (cv[(size_t) j] - ref); ++j; }
        if (j - i >= 6)
        {
            std::nth_element (seg.begin(), seg.begin() + (long) seg.size() / 2, seg.end());
            ToneNote t; t.a = i; t.b = j; t.sung = t.target = seg[seg.size() / 2];
            out.push_back (t);
        }
        i = juce::jmax (j, i + 1);
    }
    return out;
}
static juce::AudioBuffer<float> toneRender (const juce::AudioBuffer<float>& in, double sr, const std::vector<ToneNote>& notes)
{
    juce::AudioBuffer<float> b (in);
    auto fx = createBuiltinEffect ("pitchshift");
    if (fx == nullptr) return b;
    fx->prepare (sr, 64);
    const PlayContext ctx;
    const int n = b.getNumSamples();
    for (int at = 0; at < n; at += 64)
    {
        const int num = juce::jmin (64, n - at);
        const int fr = (at - toneWin / 2) / toneHop;
        float off = 0.0f;
        for (auto& t : notes) if (fr >= t.a && fr < t.b) { off = t.target - t.sung; break; }
        const float semis = std::round (off);
        fx->setParamQuiet (0, semis); fx->setParamQuiet (1, (off - semis) * 100.0f);
        juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 2, at, num);
        fx->process (blk, num, ctx);
    }
    return b;
}
static juce::AudioBuffer<float> timeRender (const juce::AudioBuffer<float>& in, double sr, double bpm, double startBeat, double grid, float strength)
{
    const int n = in.getNumSamples(), hop = 256;
    std::vector<float> e;
    for (int at = 0; at + 512 <= n; at += hop)
    {
        double q = 0;
        for (int i = 0; i < 512; ++i) q += in.getSample (0, at + i) * in.getSample (0, at + i) + in.getSample (1, at + i) * in.getSample (1, at + i);
        e.push_back ((float) std::sqrt (q / 1024.0));
    }
    const float mx = e.empty() ? 0.0f : *std::max_element (e.begin(), e.end());
    const double total = n / sr;
    std::vector<double> src { 0.0 }, dst { 0.0 };
    double lastOn = -1.0;
    for (int k = 8; k < (int) e.size(); ++k)
    {
        float avg = 0; for (int j = k - 8; j < k; ++j) avg += e[(size_t) j]; avg /= 8.0f;
        if (! (e[(size_t) k] > 1.8f * avg + 1.0e-4f && e[(size_t) k] > 0.08f * mx)) continue;
        const double t = k * hop / sr;
        if (t - lastOn < 0.07) continue;
        lastOn = t;
        const double beat = startBeat + t * bpm / 60.0, want = std::round (beat / grid) * grid;
        const double d = t + strength * (want - beat) * 60.0 / bpm;
        if (std::abs (d - t) < 0.002 || d <= dst.back() + 0.02 || t <= src.back() + 0.02 || d >= total - 0.02 || t >= total - 0.02) continue;
        src.push_back (t); dst.push_back (d);
    }
    src.push_back (total); dst.push_back (total);
    juce::AudioBuffer<float> out (2, n); out.clear();
    for (size_t i = 0; i + 1 < src.size(); ++i)
    {
        const int s0 = (int) (src[i] * sr), s1 = juce::jmin (n, (int) (src[i + 1] * sr));
        const int d0 = (int) (dst[i] * sr), d1 = juce::jmin (n, (int) (dst[i + 1] * sr));
        if (s1 <= s0 || d1 <= d0) continue;
        juce::AudioBuffer<float> seg (2, s1 - s0);
        for (int c = 0; c < 2; ++c) seg.copyFrom (c, 0, in, c, s0, s1 - s0);
        const double factor = (double) (d1 - d0) / (s1 - s0);
        juce::AudioBuffer<float> st = std::abs (factor - 1.0) < 0.002 ? seg : olaStretch (seg, factor);
        const int len = juce::jmin (d1 - d0, st.getNumSamples());
        const int fade = juce::jmin (16, len / 2);
        for (int c = 0; c < 2; ++c)
        {
            out.copyFrom (c, d0, st, c, 0, len);
            if (i > 0) out.applyGainRamp (c, d0, fade, 0.0f, 1.0f);
            out.applyGainRamp (c, d0 + len - fade, fade, 1.0f, 0.0f);
        }
    }
    return out;
}

class ToolWindow : public juce::DocumentWindow
{
public:
    ToolWindow (const juce::String& t, juce::Component* c) : juce::DocumentWindow (t, col::panel, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true); setContentOwned (c, true); setResizable (true, false);
        centreWithSize (getWidth(), getHeight()); setVisible (true);
    }
    void closeButtonPressed() override { juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<juce::Component> (this)] { delete sp.getComponent(); }); }
};

// HumTone: every sung note is a block; drag it up / down (snaps to semitones, Alt = free), SNAP ALL centres every note
class HumToneView : public juce::Component
{
public:
    HumToneView (Studio& st, int clipUid) : s (st), uid (clipUid)
    {
        setupButton (snapBtn, "SNAP ALL", "Centre every note on its nearest semitone");
        setupButton (resetBtn, "RESET", "Put every note back where it was sung");
        setupButton (applyBtn, "APPLY", "Render the new pitches into the clip (Ctrl+Z undoes)");
        setupButton (closeBtn, "X  CLOSE", "Close HumTone without applying");
        for (auto* b : { &snapBtn, &resetBtn, &applyBtn, &closeBtn }) addAndMakeVisible (*b);
        snapBtn.onClick = [this] { for (auto& t : notes) t.target = std::round (t.sung); repaint(); };
        resetBtn.onClick = [this] { for (auto& t : notes) t.target = t.sung; repaint(); };
        closeBtn.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->closeButtonPressed(); };
        applyBtn.onClick = [this] { apply(); };
        for (auto& c : s.p().clips) if (c.uid == uid) clip = c;
        if (readClipAudio (clip, s.p().bpm, audio, sr)) { curve = toneCurve (audio, sr); notes = toneNotes (curve); }
        lo = 127; hi = 0;
        for (auto& t : notes) { lo = juce::jmin (lo, (int) t.sung - 3); hi = juce::jmax (hi, (int) t.sung + 4); }
        if (hi <= lo) { lo = 48; hi = 72; }
        setSize (960, 460);
    }
    void resized() override
    {
        auto r = getLocalBounds().removeFromTop (34).reduced (6, 4);
        for (auto* b : { &snapBtn, &resetBtn, &applyBtn }) { b->setBounds (r.removeFromLeft (90)); r.removeFromLeft (6); }
        closeBtn.setBounds (r.removeFromRight (100));
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel.darker (0.4f));
        const auto a = area();
        const float rowH = a.getHeight() / (float) (hi - lo);
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        for (int k = lo; k < hi; ++k)
        {
            const float y = yOf (k + 0.5f);
            g.setColour (juce::Colours::white.withAlpha ((k % 12 == 1 || k % 12 == 3 || k % 12 == 6 || k % 12 == 8 || k % 12 == 10) ? 0.03f : 0.07f));
            g.fillRect (a.getX(), y, a.getWidth(), rowH - 1);
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.setFont (11.0f);
            g.drawText (juce::String (names[k % 12]) + juce::String (k / 12 - 1), 2, (int) y, 40, (int) rowH, juce::Justification::centredLeft);
        }
        if (notes.empty())
        {
            g.setColour (juce::Colours::white);
            g.drawText ("No sung notes found in this clip", a, juce::Justification::centred);
        }
        for (auto& t : notes)
        {
            const float x0 = xOf (t.a), x1 = xOf (t.b);
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.drawRect (juce::Rectangle<float> (x0, yOf (t.sung + 0.5f), x1 - x0, rowH), 1.0f);
            g.setColour (col::accent.withAlpha (0.75f));
            g.fillRoundedRectangle (juce::Rectangle<float> (x0, yOf (t.target + 0.5f), x1 - x0, rowH).reduced (0, 1), 3.0f);
            juce::Path pc;
            for (int f = t.a; f < t.b; ++f)
            {
                const float y = yOf (curve[(size_t) f] + t.target - t.sung);
                if (f == t.a) pc.startNewSubPath (xOf (f), y); else pc.lineTo (xOf (f), y);
            }
            g.setColour (juce::Colours::white);
            g.strokePath (pc, juce::PathStrokeType (1.5f));
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = -1;
        for (int i = 0; i < (int) notes.size(); ++i)
            if (e.x >= xOf (notes[(size_t) i].a) && e.x <= xOf (notes[(size_t) i].b)) { drag = i; dragFrom = notes[(size_t) i].target; }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag < 0) return;
        const float rowH = area().getHeight() / (float) (hi - lo);
        float t = dragFrom - e.getDistanceFromDragStartY() / rowH;
        if (! e.mods.isAltDown()) t = std::round (t);
        notes[(size_t) drag].target = juce::jlimit ((float) lo, (float) hi - 1, t);
        repaint();
    }
private:
    juce::Rectangle<float> area() const { return getLocalBounds().withTrimmedTop (38).withTrimmedLeft (44).toFloat().reduced (2); }
    float xOf (int f) const { const auto a = area(); return a.getX() + a.getWidth() * f / (float) juce::jmax (1, (int) curve.size()); }
    float yOf (float midi) const { const auto a = area(); return a.getBottom() - a.getHeight() * (midi - lo) / (float) (hi - lo); }
    void apply()
    {
        if (audio.getNumSamples() == 0) return;
        s.engine.stop();
        const auto out = toneRender (audio, sr, notes);
        const auto f = s.p().recordingsFolder().getChildFile ("HumTone").getNonexistentChildFile (clip.audioFile.getFileNameWithoutExtension() + "_humtone", ".wav");
        if (writeClipAudio (f, out, sr)) useRendered (s, uid, f, "HumTone");
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->closeButtonPressed();
    }
    Studio& s; int uid; Clip clip;
    juce::AudioBuffer<float> audio; double sr = 44100;
    std::vector<float> curve; std::vector<ToneNote> notes;
    int lo = 48, hi = 72, drag = -1; float dragFrom = 0;
    juce::TextButton snapBtn, resetBtn, applyBtn, closeBtn;
};

static bool isSurprise (const Clip& c) { return c.isAudio() && c.audioFile.getFileName().contains ("_surprise"); }

struct LaneHeader : public juce::Component
{
    juce::SortedSet<int> targets() const
    {
        if (s.selectedLanes.contains (index)) return s.selectedLanes;
        juce::SortedSet<int> one; one.add (index); return one;
    }
    LaneHeader (Studio& st, int i) : s (st), index (i)
    {
        name.setEditable (false, true);
        rightClick.f = [this] (const juce::MouseEvent& e) { mouseDown (e.getEventRelativeTo (this)); };
        name.addMouseListener (&rightClick, false);   // right-click on the track name opens the track menu too
        name.setTooltip ("Track name (double-click to rename). Right-click for color, HumHouse Drummer, routing, delete");
        name.onTextChange = [this] { const auto t = name.getText(); s.edit ("Rename track", [&] (Project& p) { if (index < (int) p.lanes.size()) p.lanes[(size_t) index].name = t; }); };
        setupButton (arm, "R", "Arm for recording - every armed track records at once to its own insert (R then Space)", true);
        arm.setColour (juce::TextButton::buttonOnColourId, col::rec);
        arm.onClick = [this] { const bool on = arm.getToggleState(); const auto t = targets(); s.edit ("Arm", [&] (Project& p) { for (int i : t) if (i < (int) p.lanes.size()) p.lanes[(size_t) i].armed = on; }); };
        setupButton (mute, "M", "Mute this track", true);
        mute.setColour (juce::TextButton::buttonOnColourId, col::dim);
        mute.onClick = [this] { const bool on = mute.getToggleState(); const auto t = targets(); s.edit ("Mute track", [&] (Project& p) { for (int i : t) if (i < (int) p.lanes.size()) p.lanes[(size_t) i].muted = on; }); };
        soft.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag); soft.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        soft.setRange (-80.0, 80.0, 1.0); soft.setDoubleClickReturnValue (true, 0.0); soft.setTextValueSuffix (" ms");
        soft.setTooltip ("Soft: slide this whole track earlier (left) or later (right) in ms to line the vocal up with the beat. Double-click = 0");
        soft.setPopupDisplayEnabled (true, true, nullptr);
        soft.onValueChange = [this] {
            // ms off the beat: clips selected on this track move alone; nothing selected = the whole track shifts
            const float v = (float) soft.getValue(), d = v - softPrev; softPrev = v;
            bool sel = false;
            for (auto& c : s.p().clips) sel |= c.selected && c.lane == index;
            s.live ([&] (Project& p) {
                if (sel) { const double db = d * 0.001 * p.bpm / 60.0; for (auto& c : p.clips) if (c.selected && c.lane == index) c.start = juce::jmax (0.0, c.start + db); }
                else if (index < (int) p.lanes.size()) p.lanes[(size_t) index].shiftMs = v; });
            if (! soft.isMouseButtonDown() && sel) s.commit ("Nudge clips (ms)");
            s.engine.sync(); };
        soft.onDragEnd = [this] { s.sendChangeMessage(); };
        addAndMakeVisible (soft);
        setupButton (norm, "N", "N = Auto-normalize: brings your voice to a steady, strong level while recording / monitoring (0 latency)", true);
        norm.setColour (juce::TextButton::buttonOnColourId, col::good);
        norm.onClick = [this] { const bool on = norm.getToggleState(); s.edit ("Auto normalize", [&] (Project& p) { p.lanes[(size_t) index].autoNorm = on; }); };
        setupButton (mon, "L", "L = Listen (input monitoring): hear the input live through this track's insert FX (AutoPitch, EQ, comp...)", true);
        mon.setColour (juce::TextButton::buttonOnColourId, col::accent2);
        mon.onClick = [this] { const bool on = mon.getToggleState(); s.edit ("Monitor", [&] (Project& p) { p.lanes[(size_t) index].monitor = on; }); };
        input.setTooltip ("Recording input: MONO puts one mic in the centre (both ears), STEREO records an input pair");
        input.onChange = [this] { const int id = input.getSelectedId(); if (id <= 0) return;
            s.edit ("Input", [&] (Project& p) { auto& l = p.lanes[(size_t) index];
                if (id >= 1000) { l.stereoIn = true; l.inputPair = id - 1000; } else { l.stereoIn = false; l.inputCh = id - 1; } }); };
        insert.setTooltip ("Mixer insert this track plays and records through - every take stays on it");
        insert.onChange = [this] { const int v = insert.getSelectedItemIndex(); s.edit ("Route track", [&] (Project& p) { p.lanes[(size_t) index].mixerTrack = v; }); };
        for (juce::Component* c : { (juce::Component*) &name, (juce::Component*) &arm, (juce::Component*) &mute, (juce::Component*) &mon, (juce::Component*) &norm,
                                    (juce::Component*) &input, (juce::Component*) &insert })
            addAndMakeVisible (c);
        refresh();
    }
    void refresh()
    {
        auto& p = s.p();
        if (index >= (int) p.lanes.size()) return;
        auto& l = p.lanes[(size_t) index];
        name.setText (l.name, juce::dontSendNotification);
        name.setColour (juce::Label::textColourId, l.nameColour.isTransparent() ? col::text : l.nameColour);
        name.setFont (col::laneNameFont (l.nameFont, 15.0f));
        arm.setToggleState (l.armed, juce::dontSendNotification);
        mute.setToggleState (l.muted, juce::dontSendNotification);
        mon.setToggleState (l.monitor, juce::dontSendNotification);
        norm.setToggleState (l.autoNorm, juce::dontSendNotification);
        if (! soft.isMouseButtonDown()) { soft.setValue (l.shiftMs, juce::dontSendNotification); softPrev = l.shiftMs; }
        input.clear (juce::dontSendNotification);
        int ins = 2;
        if (auto* d = s.engine.deviceManager.getCurrentAudioDevice()) ins = juce::jmax (2, d->getInputChannelNames().size());
        input.addSectionHeading ("Mono (both ears)");
        for (int i = 0; i < juce::jmax (1, ins); ++i) input.addItem (juce::String (i + 1), i + 1);
        input.addSectionHeading ("Stereo");
        for (int i = 0; i < juce::jmax (1, ins / 2); ++i) input.addItem (juce::String (i * 2 + 1) + "/" + juce::String (i * 2 + 2), 1000 + i);
        input.setSelectedId (l.stereoIn ? 1000 + l.inputPair : 1 + l.inputCh, juce::dontSendNotification);
        insert.clear (juce::dontSendNotification);
        for (size_t i = 0; i < p.mixer.size(); ++i) insert.addItem ((i == 0 ? juce::String ("M") : juce::String ((int) i)), (int) i + 1);
        insert.setSelectedItemIndex (juce::jlimit (0, (int) p.mixer.size() - 1, l.mixerTrack), juce::dontSendNotification);
        input.setTooltip ("Input: " + (l.stereoIn ? "stereo pair " + juce::String (l.inputPair * 2 + 1) + "/" + juce::String (l.inputPair * 2 + 2) : "mono input " + juce::String (l.inputCh + 1) + ", centred in both ears")
                          + ". 1, 2... = one mic (mono), 1/2 = stereo pair");
        const int mt = juce::jlimit (0, (int) p.mixer.size() - 1, l.mixerTrack);
        insert.setTooltip ("Mixer insert: " + (mt == 0 ? juce::String ("Master") : juce::String (mt) + " " + p.mixer[(size_t) mt].name) + ". This track plays and records through it");
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        if (s.selectedLanes.contains (index))
        {
            g.setColour (col::accent.withAlpha (0.16f)); g.fillRect (getLocalBounds());
            g.setColour (col::accent); g.fillRect (getLocalBounds().removeFromLeft (3));
        }
        if (index < (int) s.p().lanes.size() && ! s.p().lanes[(size_t) index].colour.isTransparent())
        {
            g.setColour (s.p().lanes[(size_t) index].colour); g.fillRect (getLocalBounds().removeFromRight (5));
        }
        g.setColour (col::gridBar); g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        if (arm.getToggleState() && s.engine.isRecording()) { g.setColour (col::rec.withAlpha (0.25f)); g.fillRect (getLocalBounds().removeFromLeft (4)); }
        const auto gr = grip().toFloat();   // FL-style mini box: drag = this track's height, double-click = minimize / large
        g.setColour (col::dim.withAlpha (0.55f)); g.drawRoundedRectangle (gr, 1.5f, 1.0f);
        g.fillRect (gr.reduced (2.5f, 3.0f));
    }
    juce::Rectangle<int> grip() const { return { getWidth() - 19, getHeight() - 10, 11, 8 }; }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 3);
        auto top = r.removeFromTop (r.getHeight() / 2);
        arm.setBounds (top.removeFromLeft (24)); top.removeFromLeft (3);
        mute.setBounds (top.removeFromLeft (24)); top.removeFromLeft (3);
        name.setBounds (top);
        r.removeFromTop (2);
        mon.setBounds (r.removeFromLeft (38)); r.removeFromLeft (3);
        norm.setBounds (r.removeFromLeft (44)); r.removeFromLeft (3);
        soft.setBounds (r.removeFromLeft (24)); r.removeFromLeft (3);
        input.setBounds (r.removeFromLeft (70)); r.removeFromLeft (3);
        insert.setBounds (r);
    }
    std::function<void (int lane, int height)> onHeight;
    std::function<void (int size)> onAllHeights;
    bool edgeDrag = false; int dragStartH = 0;
    bool nearBottomEdge (const juce::MouseEvent& e) const { return e.y >= getHeight() - 5 || grip().expanded (3).contains (e.getPosition()); }
    juce::Slider soft; float softPrev = 0.0f;
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (grip().expanded (3).contains (e.getPosition()) && onHeight) onHeight (index, getHeight() < 60 ? 150 : 26); }
    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (nearBottomEdge (e) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (edgeDrag && onHeight) onHeight (index, dragStartH + e.getDistanceFromDragStartY());
        else if (! e.mods.isPopupMenu() && std::abs (e.getDistanceFromDragStartY()) > 6) setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && nearBottomEdge (e)) { edgeDrag = true; dragStartH = getHeight(); return; }
        auto& sel = s.selectedLanes;
        if (e.mods.isPopupMenu()) { if (! sel.contains (index)) { sel.clear(); sel.add (index); s.laneAnchor = index; } }
        else if (e.mods.isShiftDown() && s.laneAnchor >= 0)
        {
            sel.clear();
            for (int i = juce::jmin (s.laneAnchor, index); i <= juce::jmax (s.laneAnchor, index); ++i) sel.add (i);
        }
        else if (e.mods.isCommandDown()) { if (sel.contains (index)) sel.removeValue (index); else sel.add (index); s.laneAnchor = index; }
        else { sel.clear(); sel.add (index); s.laneAnchor = index; }
        if (getParentComponent() != nullptr) getParentComponent()->repaint();
        for (auto* c : getParentComponent()->getChildren()) c->repaint();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (edgeDrag) { edgeDrag = false; return; }
        if (! e.mods.isPopupMenu())
        {
            // drag a track's header up/down to reorder; the tracks in between shift to make room, each keeps its mixer insert
            const int dy = e.getDistanceFromDragStartY(), h = juce::jmax (1, getHeight());
            setMouseCursor (juce::MouseCursor::NormalCursor);
            if (std::abs (dy) > h / 2)
            {
                const int from = index, to = juce::jlimit (0, juce::jmax (0, (int) s.p().lanes.size() - 1), index + (int) std::lround ((double) dy / h));
                if (to != from)
                {
                    s.selectedLanes.clear(); s.selectedLanes.add (to); s.laneAnchor = to;
                    s.edit ("Move track", [from, to] (Project& p) { moveLane (p, from, to); });
                }
            }
            return;
        }
        juce::PopupMenu m;
        const int n = targets().size();
        m.addItem (1, "Rename..."); m.addItem (2, "New mixer insert for this track"); m.addItem (3, "Open insert in Mixer");
        if (index < (int) s.p().lanes.size()) m.addSubMenu ("Presets", trackPresetMenu (s, s.p().lanes[(size_t) index].mixerTrack));
        m.addSubMenu ("Color", colourMenu (30));
        { auto nc = colourMenu (50); nc.addSeparator(); nc.addItem (49, "Automatic (opposite of the clip color)"); m.addSubMenu ("Name color", nc); }
        { juce::PopupMenu fm; const char* fonts[] { "Default", "Bold", "Italic", "Mono", "Serif", "Sans" }; for (int k = 0; k < 6; ++k) fm.addItem (70 + k, fonts[k]); m.addSubMenu ("Name font", fm); }
        {
            bool takes = false, surprise = false;
            for (auto& c : s.p().clips) if (c.lane == index) { takes |= c.isAudio(); surprise |= isSurprise (c) && c.hidden; }
            m.addSeparator();
            m.addItem (8, "Comp takes (a take lane under this track for every take)", takes);
            m.addItem (9, "Recover surprise take (sung while armed, record off)", surprise);
            bool comping = false;
            for (auto& l : s.p().lanes) comping |= l.compLane == index;
            if (comping) m.addItem (10, "Exit comp (close the take lanes, keep the comp)");
        }
        m.addSeparator(); m.addItem (5, "Insert track above"); m.addItem (6, "Insert track below");
       #if ! HHS_LICENSED
        m.addItem (7, "HumHouse Drummer (turn this track into drums)");
       #endif
        {
            juce::PopupMenu hm;
            hm.addItem (20, "Small"); hm.addItem (21, "Medium"); hm.addItem (22, "Large");
            m.addSubMenu ("Make all tracks", hm);
        }
        m.addSeparator(); m.addItem (4, n > 1 ? "Delete " + juce::String (n) + " selected tracks" : juce::String ("Delete track"));
        m.showMenuAsync ({}, [this] (int r) {
            const int i = index;
            if (r == 1) name.showEditor();
            else if (r == 2) s.edit ("New insert", [i] (Project& p) { p.lanes[(size_t) i].mixerTrack = p.addMixerTrack (p.lanes[(size_t) i].name); });
            else if (r == 3) { s.selectedTrack = s.p().lanes[(size_t) i].mixerTrack; if (s.showView) s.showView (2); s.sendChangeMessage(); }
            else if (r >= 20 && r <= 22) { if (onAllHeights) onAllHeights (r - 20); }
            else if (r >= 30 && r <= 42) { const auto t = targets(); const auto c = pickedColour (r - 30); s.edit ("Track color", [t, c] (Project& p) { for (int li : t) if (li >= 0 && li < (int) p.lanes.size()) p.lanes[(size_t) li].colour = c; }); }
            else if (r == 49 || (r >= 50 && r <= 62)) { const auto t = targets(); const auto c = r == 49 ? juce::Colour() : pickedColour (r - 50); s.edit ("Track name color", [t, c] (Project& p) { for (int li : t) if (li >= 0 && li < (int) p.lanes.size()) p.lanes[(size_t) li].nameColour = c; }); }
            else if (r >= 70 && r <= 75) { const auto t = targets(); static const char* fonts[] { "", "Bold", "Italic", "Mono", "Serif", "Sans" }; const juce::String f (fonts[r - 70]); s.edit ("Track name font", [t, f] (Project& p) { for (int li : t) if (li >= 0 && li < (int) p.lanes.size()) p.lanes[(size_t) li].nameFont = f; }); }
            else if (r == 4) { const auto t = targets(); s.selectedLanes.clear(); s.edit ("Delete track", [t] (Project& p) { deleteLanes (p, t); }); }
            else if (r == 8) { s.selectedLanes.clear(); s.edit ("Comp takes", [i] (Project& p) { makeCompLanes (p, i); }); }
            else if (r == 10) { s.selectedLanes.clear(); s.edit ("Exit comp", [i] (Project& p) { exitComp (p, i); }); }
            else if (r == 9) s.edit ("Recover surprise take", [i] (Project& p) {
                const Clip* best = nullptr;
                for (auto& c : p.clips) if (c.lane == i && c.hidden && isSurprise (c) && (best == nullptr || c.uid > best->uid)) best = &c;
                if (best != nullptr) { const Clip c = *best; placeOver (p, c, i); } });
            else if (r == 5) { s.selectedLanes.clear(); s.edit ("Insert track", [i] (Project& p) { insertLane (p, i); }); }
            else if (r == 6) { s.selectedLanes.clear(); s.edit ("Insert track", [i] (Project& p) { insertLane (p, i + 1); }); }
            else if (r == 7)
            {
                auto id = std::make_shared<int> (-1);
                const double at = std::floor (juce::jmax (0.0, s.engine.positionBeats()) / 4.0) * 4.0;
                s.edit ("Session Drummer", [i, id, at] (Project& p) { *id = makeLaneDrummer (p, i, at, 8); });
                if (*id >= 0) openChannelEditor (s, *id);
            }
        });
    }
    Studio& s; int index;
    struct RightClick : juce::MouseListener { std::function<void (const juce::MouseEvent&)> f; void mouseDown (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu() && f) f (e); } } rightClick;
    juce::Label name; juce::TextButton arm, mute, mon, norm; juce::ComboBox input, insert;
};

class Playlist : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget, public juce::TooltipClient,
                 private juce::ChangeListener, private juce::Timer
{
public:
    explicit Playlist (Studio& st) : s (st)
    {
        s.addChangeListener (this);
        setupButton (addDrummer, "HumHouse Drums", "HumHouse Drums: adds a rock drum track (Logic-style drummer region + pad window). Or right-click any track name > HumHouse Drummer");
        addDrummer.onClick = [this] {
            auto id = std::make_shared<int> (-1);
            const double at = std::floor (juce::jmax (0.0, s.engine.positionBeats()) / 4.0) * 4.0;
            s.selectedLanes.clear();
            s.edit ("HumHouse Drummer", [id, at] (Project& p) { insertLane (p, 0); *id = makeLaneDrummer (p, 0, at, 8); });   // always track 1
            if (*id >= 0) openChannelEditor (s, *id);
        };
       #if ! HHS_LICENSED
        addAndMakeVisible (addDrummer);
       #endif
        setupButton (addTrack, "+ Track", "New audio track with its own mixer insert (vocals, guitar...)");
        addTrack.onClick = [this] {
            s.edit ("Add track", [] (Project& p) {
                Lane l; l.name = "Track " + juce::String (p.lanes.size() + 1); l.mixerTrack = p.addMixerTrack (l.name); p.lanes.push_back (l);
            });
        };
        {
            const char* labels[] { "SEL", "PAINT", "DEL", "MUTE", "SLICE" };
            const char* tips[] { "Select / move (E) - click a clip to select, drag to move", "Paint (B) - click empty space to paint copies of the last clip",
                                 "Delete (D) - click clips to delete them", "Mute (T) - click a clip to mute / unmute it",
                                 "Slice (C) - click a clip (audio, pattern or drummer) to cut it at the snap line" };
            for (int i = 0; i < 5; ++i)
            {
                auto* b = toolBtns.add (new juce::TextButton());
                setupButton (*b, labels[i], tips[i], true);
                b->setRadioGroupId (4301);
                b->onClick = [this, i] { tool = (PTool) i; };
                addAndMakeVisible (b);
            }
            toolBtns[0]->setToggleState (true, juce::dontSendNotification);
            setupButton (groupBtn, "GROUP", "Group the selected clips so they select and move together (Ctrl/Cmd+G, ungroup Ctrl/Cmd+Shift+G)");
            groupBtn.onClick = [this] { groupSelected (true); };
            addAndMakeVisible (groupBtn);
            for (int i = 0; i < kNumSnaps; ++i) snapBox.addItem (juce::String ("Snap: ") + kSnapNames[i], i + 1);
            snapBox.setSelectedItemIndex (9, juce::dontSendNotification);
            snapBox.setTooltip ("Playlist snap: (none) = free placement; 1/2, 1/3, 1/6 step or beat for meticulous edits (hold Alt while dragging to bypass)");
            snapBox.onChange = [this] { plSnap = kSnapValues[juce::jmax (0, snapBox.getSelectedItemIndex())]; repaint(); };
            addAndMakeVisible (snapBox);
            vZoom.setRange (0.35, 4.0); vZoom.setSkewFactorFromMidPoint (1.0); vZoom.setValue (zoomH, juce::dontSendNotification);
            vZoom.setTooltip ("Track height: slide to squeeze all tracks skinny or stretch them large");
            vZoom.setColour (juce::Slider::thumbColourId, col::accent); vZoom.setColour (juce::Slider::trackColourId, col::panel2);
            vZoom.onValueChange = [this] { zoomH = (float) vZoom.getValue(); layoutHeaders(); repaint(); };
            addAndMakeVisible (vZoom);
        }
        setupButton (loopRec, "LOOP REC", "Loop recording: the punch range loops and every pass becomes a take", true);
        loopRec.onClick = [this] { const bool on = loopRec.getToggleState(); s.edit ("Loop record", [on] (Project& p) { p.loopRecord = on; }); };
        setupButton (rts, "RETURN", "On stop, jump back to where playback started (FL style)", true);
        rts.onClick = [this] { const bool on = rts.getToggleState(); s.edit ("Return to start", [on] (Project& p) { p.returnToStart = on; }); };
        setupButton (preroll, "PRE-ROLL", "Start 1 bar before the punch-in so you can catch the vibe", true);
        preroll.onClick = [this] { const bool on = preroll.getToggleState(); s.edit ("Pre-roll", [on] (Project& p) { p.preRollBars = on ? 1.0 : 0.0; }); };
        setupButton (clearPunch, "CLEAR PUNCH", "Remove the punch-in / out range (or double-click the ruler)");
        clearPunch.onClick = [this] { s.edit ("Clear punch", [] (Project& p) { p.punchIn = p.punchOut = -1.0; }); };
        for (auto* b : { &addTrack, &loopRec, &rts, &preroll, &clearPunch }) addAndMakeVisible (b);
        thumbCache = std::make_unique<juce::AudioThumbnailCache> (256);
        setWantsKeyboardFocus (true);
        refresh();
        startTimerHz (30);
    }
    ~Playlist() override { s.removeChangeListener (this); }

    juce::String getTooltip() override
    {
        if (lastMouse.y >= toolbarH && lastMouse.y < toolbarH + rulerH && lastMouse.x > headerW)
            return "Ruler: click = move playhead, drag the triangle = scrub (Alt = no snap), drag the numbers = loop range (E: also selects), double-click = clear";
        if (lastMouse.x > headerW && lastMouse.y > toolbarH + rulerH)
            return "Double-click = place pattern | drag clip = move | drag edges = trim | Alt+drag right edge = stretch | top corners = fades (Alt = curve) | right-click = takes, routing, split";
        return {};
    }

    void resized() override
    {
        placeSplitBars();
        laneH = juce::jmax (60, (getHeight() - toolbarH - rulerH - 8) / juce::jmax (1, (int) s.p().lanes.size()));
        auto t = getLocalBounds().removeFromTop (toolbarH).reduced (6, 4);
        addTrack.setBounds (t.removeFromLeft (80)); t.removeFromLeft (6);
        t.removeFromLeft (12);
        for (auto* b : { &loopRec, &rts, &preroll, &clearPunch }) { b->setBounds (t.removeFromLeft (b == &clearPunch ? 100 : 78)); t.removeFromLeft (4); }
        t.removeFromLeft (10);
        for (auto* b : toolBtns) { b->setBounds (t.removeFromLeft (50)); t.removeFromLeft (2); }
        t.removeFromLeft (6);
        groupBtn.setBounds (t.removeFromLeft (60)); t.removeFromLeft (8);
        snapBox.setBounds (t.removeFromLeft (120));
        if (addDrummer.isVisible()) { t.removeFromLeft (6); addDrummer.setBounds (t.removeFromLeft (120)); }
        vZoom.setBounds (getWidth() - 16, toolbarH + rulerH + 4, 14, juce::jmax (40, getHeight() - toolbarH - rulerH - 30));
        layoutHeaders();
    }

    void paint (juce::Graphics& g) override
    {
        auto& p = s.p();
        g.fillAll (col::bg);
        g.setColour (col::panel); g.fillRect (0, 0, getWidth(), toolbarH);
        auto area = clipArea();
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (area);
            for (size_t i = 0; i < p.lanes.size(); ++i)
            {
                const int y = laneY ((int) i);
                g.setColour (i % 2 ? col::panel.darker (0.2f) : col::bg.brighter (0.03f));
                g.fillRect (area.getX(), y, area.getWidth(), laneHOf ((int) i));
                g.setColour (col::grid); g.drawHorizontalLine (y + laneHOf ((int) i) - 1, (float) area.getX(), (float) area.getRight());
            }
            const double b1 = xToBeat ((float) area.getRight());
            double step = 1.0;
            while (step * pxPerBeat < 10) step *= 2;
            for (double b = std::floor (scrollX / step) * step; b <= b1; b += step)
            {
                const bool bar = std::fmod (b + 1e-9, (double) p.beatsPerBar) < 1e-6;
                g.setColour (bar ? col::gridBar : col::grid);
                g.drawVerticalLine ((int) beatToX (b), (float) area.getY(), (float) area.getBottom());
            }
            if (plSnap > 0 && plSnap < 1.0 && plSnap * pxPerBeat >= 6)
            {
                g.setColour (col::grid.withAlpha (0.35f));
                for (double b = std::floor (scrollX / plSnap) * plSnap; b <= b1; b += plSnap)
                    if (std::abs (b - std::round (b)) > 1e-6) g.drawVerticalLine ((int) beatToX (b), (float) area.getY(), (float) area.getBottom());
            }
            if (p.hasPunch())
            {
                g.setColour (col::rec.withAlpha (0.07f));
                g.fillRect (beatToX (p.punchIn), (float) area.getY(), (float) ((p.punchOut - p.punchIn) * pxPerBeat), (float) area.getHeight());
            }
            if (timeSelB > timeSelA)
            {
                g.setColour (col::accent.withAlpha (0.10f));
                g.fillRect (beatToX (timeSelA), (float) area.getY(), (float) ((timeSelB - timeSelA) * pxPerBeat), (float) area.getHeight());
            }
            for (auto& c : p.clips) if (! c.hidden && g.clipRegionIntersects (clipRect (c).getSmallestIntegerContainer().expanded (2))) drawClip (g, c);
            // take being recorded: red clip growing with its live waveform (scaled up so quiet singing is visible)
            s.engine.forEachLiveTake ([&] (int lane, double startBeat, const float* pk, int num, double bpp) {
                if (lane < 0 || lane >= (int) p.lanes.size() || num <= 0) return;
                const float y0 = (float) laneY (lane), h = (float) laneHOf (lane);
                const float x0 = beatToX (startBeat), x1 = beatToX (startBeat + num * bpp);
                juce::Rectangle<float> r (x0, y0 + 2.0f, juce::jmax (2.0f, x1 - x0), h - 4.0f);
                g.setColour (col::rec.withAlpha (0.25f)); g.fillRoundedRectangle (r, 4);
                g.setColour (col::rec); g.fillRoundedRectangle (r.withHeight (14), 4);
                float mx = 0.02f;
                for (int i = 0; i < num; ++i) mx = std::max (mx, pk[i]);
                const float scale = juce::jlimit (1.0f, 10.0f, 0.8f / mx);
                auto body = r.withTrimmedTop (14).reduced (1, 2);
                const float mid = body.getCentreY(), half = body.getHeight() * 0.5f;
                g.setColour (col::rec.brighter (0.5f));
                for (int i = 0; i < num; ++i)
                {
                    const float a = std::min (1.0f, pk[i] * scale) * half;
                    g.drawVerticalLine ((int) beatToX (startBeat + i * bpp), mid - a, mid + a);
                }
            });
            const float px = beatToX (s.engine.positionBeats());
            const auto phc = s.engine.isRecording() ? col::rec : col::accent2;
            g.setColour (phc.withAlpha (0.22f)); g.fillRect (px - 2.5f, (float) area.getY(), 5.0f, (float) area.getHeight());
            g.setColour (phc); g.fillRect (px - 0.75f, (float) area.getY(), 1.5f, (float) area.getHeight());
        }
        if (! marquee.isEmpty())
        {
            g.setColour (col::accent2.withAlpha (0.14f)); g.fillRect (marquee);
            g.setColour (col::accent2); g.drawRect (marquee, 1.0f);
        }
        // ruler
        juce::Rectangle<int> ruler (headerW, toolbarH, getWidth() - headerW, rulerH);
        g.setColour (col::panel2); g.fillRect (ruler);
        g.setFont (juce::FontOptions (11.0f));
        const int every = rulerLabelStep (beatToX (p.beatsPerBar) - beatToX (0));
        for (int bar = (int) (scrollX / p.beatsPerBar); ; ++bar)
        {
            const float x = beatToX (bar * p.beatsPerBar);
            if (x > getWidth()) break;
            if (x < headerW) continue;
            g.setColour (col::dim);
            const bool label = bar % every == 0;
            if (label) g.drawText (juce::String (bar + 1), (int) x + 3, toolbarH, 40, rulerH, juce::Justification::centredLeft);
            g.drawVerticalLine ((int) x, (float) toolbarH + (label ? 12 : 18), (float) (toolbarH + rulerH));
        }
        if (p.hasPunch())
        {
            const float x0 = juce::jmax ((float) headerW, beatToX (p.punchIn)), x1 = beatToX (p.punchOut);
            g.setColour (col::rec.withAlpha (0.45f));
            g.fillRect (x0, (float) toolbarH + 2, x1 - x0, (float) rulerH - 4);
            g.setColour (col::text); g.drawText ("PUNCH", (int) x0 + 4, toolbarH, 60, rulerH, juce::Justification::centredLeft);
        }
        if (timeSelB > timeSelA)
        {
            const float x0 = juce::jmax ((float) headerW, beatToX (timeSelA)), x1 = beatToX (timeSelB);
            g.setColour (col::accent.withAlpha (0.45f));
            g.fillRect (x0, (float) toolbarH + 2, x1 - x0, (float) rulerH - 4);
        }
        const float px = beatToX (s.engine.positionBeats());
        if (px >= headerW)
        {
            juce::Path tri; tri.addTriangle (px - 7, (float) toolbarH + 1, px + 7, (float) toolbarH + 1, px, (float) (toolbarH + 13));
            g.setColour (col::accent2); g.fillPath (tri);
            g.fillRect (px - 0.75f, (float) toolbarH + 10, 1.5f, (float) (getHeight() - toolbarH - 10));   // line from the handle down through the tracks
        }
        g.setColour (col::panel); g.fillRect (0, toolbarH, headerW, rulerH);
        g.setColour (col::dim); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        drawHeadline (g, "PLAYLIST", { 8, toolbarH, headerW - 16, rulerH }, juce::Justification::centredLeft);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        lastMouse = e.getPosition();
        Part part; const int i = hitClip (e.position, part);
        if (tool != PTool::Select && e.position.y > toolbarH + rulerH && e.position.x > headerW)
        {
            setMouseCursor (tool == PTool::Paint ? toolCursor (CursorIcon::Brush) : tool == PTool::Delete ? toolCursor (CursorIcon::Bin)
                            : tool == PTool::Mute ? toolCursor (CursorIcon::Mute) : toolCursor (CursorIcon::Scissors));
            return;
        }
        setMouseCursor (i < 0 ? juce::MouseCursor::NormalCursor
                        : part == Part::Curve ? juce::MouseCursor::UpDownResizeCursor
                        : part == Part::FadeIn || part == Part::FadeOut ? juce::MouseCursor::PointingHandCursor
                        : part == Part::Right || part == Part::Left ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::DraggingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        auto& p = s.p();
        changed = false;
        dragStart = e.position;
        if (e.y >= toolbarH && e.y < toolbarH + rulerH && e.x > headerW)
        {
            // handle = drag the playhead; drag on the numbers = loop range (E: also selects clips in it); click = move playhead
            drag = std::abs (e.position.x - beatToX (s.engine.positionBeats())) < 10.0f ? Drag::Scrub
                 : tool == PTool::Select && ! e.mods.isShiftDown() ? Drag::TimeSel : Drag::Ruler;
            if (drag == Drag::Scrub) mouseDrag (e);
            return;
        }
        if (! clipArea().contains (e.getPosition())) return;
        Part part; const int i = hitClip (e.position, part);
        if (i >= 0 && ! p.clips[(size_t) i].selected && ! e.mods.isShiftDown()) timeSelA = timeSelB = -1;   // picking another clip drops the old ruler range
        if (i >= 0 && p.clips[(size_t) i].isAuto() && tool != PTool::Delete && tool != PTool::Slice && tool != PTool::Mute)
        {
            const int k = hitAutoPoint (p.clips[(size_t) i], e.position);
            if (k >= 0 && e.mods.isPopupMenu())   // right-click a point: delete it (FL)
            {
                const int uid = p.clips[(size_t) i].uid;
                s.edit ("Delete automation point", [uid, k] (Project& pr) {
                    for (auto& c : pr.clips) if (c.uid == uid && k < (int) c.autoPts.size() && c.autoPts.size() > 1) c.autoPts.erase (c.autoPts.begin() + k);
                });
                repaint(); return;
            }
            if (k >= 0 && ! e.mods.isCommandDown())
            {
                drag = Drag::AutoPt; dragClip = i; dragPt = k; dragOrig = p.clips;
                return;
            }
        }
        if (e.mods.isPopupMenu()) { if (i >= 0) clipMenu (i); return; }
        if ((i < 0 && tool != PTool::Paint) || e.mods.isCommandDown())   // FL: drag empty space (or Ctrl/Cmd+drag) to box-select
        {
            if (! e.mods.isShiftDown()) { timeSelA = timeSelB = -1; s.live ([] (Project& pr) { for (auto& c : pr.clips) c.selected = false; }); }
            drag = Drag::Marquee; marquee = {}; marqueeBase.clear();
            for (auto& c : p.clips) marqueeBase.push_back (c.selected);
            repaint(); return;
        }
        if (i < 0 && tool == PTool::Paint)
        {
            const int lane = laneAt (e.position.y);
            if (lane < 0 || lane >= (int) p.lanes.size()) return;
            const double at = juce::jmax (0.0, std::floor (xToBeat (e.position.x) / gridSnap()) * gridSnap());
            s.edit ("Paint clip", [&] (Project& pr) {
                Clip c;
                auto src = std::find_if (pr.clips.begin(), pr.clips.end(), [this] (const Clip& o) { return o.uid == lastClipUid; });
                if (src != pr.clips.end()) c = *src;
                else if (! pr.patterns.empty()) { c.patternIndex = juce::jlimit (0, (int) pr.patterns.size() - 1, pr.currentPattern); c.length = pr.patterns[(size_t) c.patternIndex].lengthBeats(); }
                else return;
                c.uid = pr.newUid(); c.lane = lane; c.start = at; c.selected = false; c.group = 0; c.takeGroup = 0; c.hidden = false;
                pr.clips.push_back (c);
            });
            return;
        }
        if (i >= 0 && tool == PTool::Delete)
        {
            const int g = p.clips[(size_t) i].group, uid = p.clips[(size_t) i].uid;
            s.edit ("Delete clip", [&] (Project& pr) {
                pr.clips.erase (std::remove_if (pr.clips.begin(), pr.clips.end(), [&] (const Clip& c) { return c.uid == uid || (g > 0 && c.group == g); }), pr.clips.end());
            });
            return;
        }
        if (i >= 0 && tool == PTool::Mute)
        {
            const int uid = p.clips[(size_t) i].uid;
            s.edit ("Mute clip", [&] (Project& pr) { for (auto& c : pr.clips) if (c.uid == uid) c.muted = ! c.muted; });
            return;
        }
        if (i >= 0 && tool == PTool::Slice)
        {
            const int uid = p.clips[(size_t) i].uid;
            const double at = snap (xToBeat (e.position.x));
            s.edit ("Slice clip", [&] (Project& pr) {
                auto it = std::find_if (pr.clips.begin(), pr.clips.end(), [uid] (const Clip& c) { return c.uid == uid; });
                if (it == pr.clips.end() || at <= it->start || at >= it->start + it->length) return;
                Clip b = *it; b.uid = pr.newUid();
                const double shortFade = secondsToBeats (0.005, pr.bpm);
                b.start = at; b.length = it->start + it->length - at; b.sourceOffset += beatsToSeconds (at - it->start, pr.bpm); b.patOffset += at - it->start;
                b.fadeIn = shortFade; b.selected = false;
                it->length = at - it->start; it->fadeOut = shortFade;
                pr.clips.push_back (b);
            });
            return;
        }
        if (i < 0)
        {
            timeSelA = timeSelB = -1;
            s.live ([] (Project& pr) { for (auto& c : pr.clips) c.selected = false; });
            s.engine.setPositionBeats (juce::jmax (0.0, snap (xToBeat (e.position.x))));
            repaint();
            return;
        }
        s.live ([&] (Project&) {
            if (! p.clips[(size_t) i].selected && ! e.mods.isShiftDown()) for (auto& c : p.clips) c.selected = false;
            p.clips[(size_t) i].selected = true;
            if (const int g = p.clips[(size_t) i].group; g > 0) for (auto& c : p.clips) if (c.group == g) c.selected = true;
        });
        lastClipUid = p.clips[(size_t) i].uid;
        dragClip = i; dragOrig = p.clips;
        drag = part == Part::Curve ? Drag::Curve : part == Part::FadeIn ? Drag::FadeIn : part == Part::FadeOut ? Drag::FadeOut : part == Part::Right ? Drag::Resize
             : part == Part::Left ? Drag::Trim : Drag::Move;
        cloneOnDrag = drag == Drag::Move && e.mods.isShiftDown(); cloned = false;   // FL: Shift+drag clones
        stretchDrag = drag == Drag::Resize && e.mods.isAltDown() && p.clips[(size_t) i].isAudio();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& p = s.p();
        lastMouse = e.getPosition();
        if (drag == Drag::Scrub)
        {
            const double b = xToBeat (e.position.x);
            s.engine.setPositionBeats (juce::jmax (0.0, e.mods.isAltDown() ? b : snap (b)));
            repaint(); return;
        }
        if (drag == Drag::AutoPt)
        {
            if (dragClip < 0 || dragClip >= (int) p.clips.size() || dragOrig.size() != p.clips.size()) return;
            const double abs = e.mods.isAltDown() ? xToBeat (e.position.x) : snap (xToBeat (e.position.x));
            s.live ([&] (Project&) {
                auto& c = p.clips[(size_t) dragClip];
                if (dragPt < 0 || dragPt >= (int) c.autoPts.size()) return;
                const double lo = dragPt > 0 ? c.autoPts[(size_t) dragPt - 1].beat : 0.0;
                const double hi = dragPt + 1 < (int) c.autoPts.size() ? c.autoPts[(size_t) dragPt + 1].beat : 1.0e9;
                auto& pt = c.autoPts[(size_t) dragPt];
                pt.beat = juce::jlimit (lo, hi, c.patOffset + abs - c.start);
                pt.value = autoYToValue (c, e.position.y);
            });
            changed = true; repaint(); return;
        }
        if (drag == Drag::TimeSel)
        {
            if (std::abs (e.position.x - dragStart.x) < 4) return;
            const double a = juce::jmax (0.0, snap (xToBeat (dragStart.x))), b = juce::jmax (0.0, snap (xToBeat (e.position.x)));
            timeSelA = juce::jmin (a, b); timeSelB = juce::jmax (a, b);
            s.live ([&] (Project& pr) { for (auto& c : pr.clips) c.selected = ! c.hidden && c.start < timeSelB && c.start + c.length > timeSelA; });
            changed = true; repaint(); return;
        }
        if (drag == Drag::Ruler)
        {
            if (std::abs (e.position.x - dragStart.x) < 4) return;
            const double a = juce::jmax (0.0, snap (xToBeat (dragStart.x))), b = juce::jmax (0.0, snap (xToBeat (e.position.x)));
            s.live ([&] (Project& pr) { pr.punchIn = juce::jmin (a, b); pr.punchOut = juce::jmax (a, b); });
            changed = true; repaint(); return;
        }
        if (drag == Drag::Marquee)
        {
            if (e.position.getDistanceFrom (dragStart) < 4) return;
            marquee = juce::Rectangle<float> (dragStart, e.position).getIntersection (clipArea().toFloat());
            s.live ([&] (Project& pr) {
                for (size_t i = 0; i < pr.clips.size(); ++i)
                    pr.clips[i].selected = (i < marqueeBase.size() && marqueeBase[i]) || clipRect (pr.clips[i]).intersects (marquee);
                for (auto& c : pr.clips) if (c.selected && c.group > 0) for (auto& o : pr.clips) if (o.group == c.group) o.selected = true;
            });
            changed = true; repaint(); return;
        }
        if (drag == Drag::Move && cloneOnDrag && ! cloned && e.position.getDistanceFrom (dragStart) > 4)
        {
            cloned = true;
            s.live ([&] (Project& pr) {
                const auto n = pr.clips.size();
                for (size_t i = 0; i < n; ++i)
                    if (pr.clips[i].selected) { auto c = pr.clips[i]; c.uid = pr.newUid(); c.selected = false; pr.clips.push_back (c); }
            });
            dragOrig = p.clips;
        }
        if (drag == Drag::None || dragClip < 0 || dragOrig.size() != p.clips.size()) return;
        const bool noSnap = e.mods.isAltDown();
        double d = xToBeat (e.position.x) - xToBeat (dragStart.x);
        if (! noSnap && plSnap > 0 && drag != Drag::FadeIn && drag != Drag::FadeOut && drag != Drag::Curve) d = std::round (d / plSnap) * plSnap;
        const int dLane = drag == Drag::Move ? laneAt (e.position.y) - laneAt (dragStart.y) : 0;
        s.live ([&] (Project&) {
            for (size_t i = 0; i < p.clips.size(); ++i)
            {
                auto o = dragOrig[i];
                if (drag == Drag::FadeIn || drag == Drag::FadeOut || drag == Drag::Curve)
                {
                    if ((int) i != dragClip) continue;
                    if (drag == Drag::Curve || e.mods.isAltDown()) o.fadeCurve = juce::jlimit (-2.0f, 2.0f, o.fadeCurve + (e.position.y - dragStart.y) / 50.0f);
                    else if (drag == Drag::FadeIn) o.fadeIn = juce::jlimit (0.0, o.length - o.fadeOut, o.fadeIn + d);
                    else o.fadeOut = juce::jlimit (0.0, o.length - o.fadeIn, o.fadeOut - d);
                }
                else if (! o.selected) continue;
                else if (drag == Drag::Move)
                {
                    o.start = juce::jmax (0.0, o.start + d);
                    o.lane = juce::jlimit (0, juce::jmax (0, (int) p.lanes.size() - 1), o.lane + dLane);
                }
                else if (drag == Drag::Resize) o.length = juce::jmax (gridSnap() * 0.25, o.length + d);
                else if (drag == Drag::Trim && o.isAudio())
                {
                    const double dd = juce::jlimit (-beatsFromSeconds (o.sourceOffset), o.length - 0.0625, d);
                    o.start += dd; o.length -= dd; o.sourceOffset += beatsToSeconds (dd, p.bpm);
                }
                else if (drag == Drag::Trim && (o.patternIndex >= 0 || o.isAuto()))
                {
                    const double dd = juce::jlimit (-o.patOffset, o.length - 0.0625, d);
                    o.start += dd; o.length -= dd; o.patOffset += dd;
                }
                p.clips[i] = o;
            }
        });
        changed = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        auto& p = s.p();
        if (drag == Drag::AutoPt)
        {
            if (changed) s.edit ("Move automation point", [] (Project&) {});
            drag = Drag::None; dragClip = dragPt = -1; repaint(); return;
        }
        if (drag == Drag::Marquee)
        {
            if (! changed) s.engine.setPositionBeats (juce::jmax (0.0, snap (xToBeat (e.position.x))));
            marquee = {}; drag = Drag::None; repaint(); return;
        }
        if (drag == Drag::TimeSel)
        {
            if (! changed) { timeSelA = timeSelB = -1; s.engine.setPositionBeats (juce::jmax (0.0, snap (xToBeat (e.position.x)))); }
            else if (timeSelB > timeSelA) s.edit ("Loop range", [a = timeSelA, b = timeSelB] (Project& pr) { pr.punchIn = a; pr.punchOut = b; });   // FL: playback loops the selection
            drag = Drag::None; repaint(); return;
        }
        if (drag == Drag::Ruler && ! changed) s.engine.setPositionBeats (juce::jmax (0.0, snap (xToBeat (e.position.x))));
        else if (changed)
        {
            if (drag == Drag::Move) timeSelA = timeSelB = -1;   // moved clips no longer match the ruler range
            if (stretchDrag && dragOrig.size() == p.clips.size()) { stretchClips(); drag = Drag::None; dragClip = -1; stretchDrag = false; repaint(); return; }
            s.edit (drag == Drag::Ruler ? "Punch range" : drag == Drag::FadeIn || drag == Drag::FadeOut || drag == Drag::Curve ? "Fade" : "Edit clips", [&] (Project&) {
                // routing follows the recording: a moved audio clip keeps playing through the insert it came from
                if (drag == Drag::Move && dragOrig.size() == p.clips.size())
                    for (size_t i = 0; i < p.clips.size(); ++i)
                    {
                        auto& c = p.clips[i];
                        if (c.isAudio() && c.lane != dragOrig[i].lane && c.mixerTrack < 0 && dragOrig[i].lane < (int) p.lanes.size())
                            c.mixerTrack = p.lanes[(size_t) dragOrig[i].lane].mixerTrack;
                    }
                autoCrossfade (p);
            });
        }
        drag = Drag::None; dragClip = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto& p = s.p();
        if (e.y >= toolbarH && e.y < toolbarH + rulerH) { timeSelA = timeSelB = -1; s.edit ("Clear punch", [] (Project& pr) { pr.punchIn = pr.punchOut = -1.0; }); return; }
        if (! clipArea().contains (e.getPosition())) return;
        Part part; const int i = hitClip (e.position, part);
        if (i >= 0 && p.clips[(size_t) i].isAuto())   // FL: double-click the envelope to add a point
        {
            const int uid = p.clips[(size_t) i].uid;
            const auto& c = p.clips[(size_t) i];
            const double at = c.patOffset + (e.mods.isAltDown() ? xToBeat (e.position.x) : snap (xToBeat (e.position.x))) - c.start;
            const float v = autoYToValue (c, e.position.y);
            s.edit ("Add automation point", [uid, at, v] (Project& pr) {
                for (auto& o : pr.clips)
                    if (o.uid == uid)
                    {
                        auto it = std::upper_bound (o.autoPts.begin(), o.autoPts.end(), at, [] (double b, const AutoPoint& q) { return b < q.beat; });
                        o.autoPts.insert (it, { at, v });
                    }
            });
            repaint(); return;
        }
        if (i >= 0)
        {
            if (p.clips[(size_t) i].isDrummer()) { openChannelEditor (s, p.clips[(size_t) i].drummer); return; }
           #if ! HHS_LICENSED
            if (p.clips[(size_t) i].isAudio())
            {
                const int uid = p.clips[(size_t) i].uid;
                juce::PopupMenu m;
                m.addItem (1, "Split stems (Drums, Bass, Vocals, Melodies)");
                m.showMenuAsync ({}, [this, uid] (int r) { if (r == 1) splitClipStems (uid); });
                return;
            }
           #endif
            if (! p.clips[(size_t) i].isAudio())
            {
                const int pat = p.clips[(size_t) i].patternIndex;
                s.edit ("Open pattern", [pat] (Project& pr) { pr.currentPattern = pat; });
                if (s.showView) s.showView (0);
            }
            return;
        }
        const int lane = laneAt (e.position.y);
        if (lane < 0 || p.patterns.empty()) return;
        const double at = juce::jmax (0.0, std::floor (xToBeat (e.position.x) / p.beatsPerBar) * p.beatsPerBar);
        s.edit ("Place pattern", [&] (Project& pr) {
            Clip c; c.uid = pr.newUid(); c.lane = lane; c.start = at; c.patternIndex = pr.currentPattern;
            c.length = pr.patterns[(size_t) pr.currentPattern].lengthBeats();
            pr.clips.push_back (c);
        });
    }

    void mouseMagnify (const juce::MouseEvent& e, float scale) override   // trackpad pinch (Mac / Windows precision touchpads)
    {
        if (e.position.x < headerW) return;
        const double at = xToBeat (e.position.x);
        pxPerBeat = juce::jlimit (2.0f, 1200.0f, pxPerBeat * scale);
        scrollX = juce::jmax (0.0, at - (e.position.x - headerW) / pxPerBeat);
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isAltDown())   // Alt+wheel: all tracks taller / thinner
        {
            zoomH = juce::jlimit (0.4f, 4.0f, zoomH * (w.deltaY > 0 ? 1.1f : 1.0f / 1.1f));
            vZoom.setValue (zoomH, juce::dontSendNotification);
            layoutHeaders(); repaint();
            return;
        }
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            const double at = xToBeat (e.position.x);
            pxPerBeat = juce::jlimit (2.0f, 1200.0f, pxPerBeat * std::pow (1.15f, juce::jlimit (-4.0f, 4.0f, w.deltaY * 8.0f)));
            scrollX = juce::jmax (0.0, at - (e.position.x - headerW) / pxPerBeat);
        }
        else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
            scrollX = juce::jmax (0.0, scrollX - (w.deltaX != 0 ? w.deltaX : w.deltaY) * 600.0 / pxPerBeat);
        else
        {
            scrollY = juce::jlimit (0, juce::jmax (0, totalLaneH() - clipArea().getHeight() + 40), scrollY - (int) (w.deltaY * 200));
            layoutHeaders();
        }
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (isTypingNoteKey (k)) return false;
        const int cmd = juce::ModifierKeys::commandModifier;
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        {
            const bool anyClip = std::any_of (s.p().clips.begin(), s.p().clips.end(), [] (const Clip& c) { return c.selected; });
            if (! anyClip && ! s.selectedLanes.isEmpty())
            {
                const auto t = s.selectedLanes; s.selectedLanes.clear();
                s.edit ("Delete tracks", [t] (Project& p) { deleteLanes (p, t); });
                return true;
            }
            s.edit ("Delete clips", [] (Project& p) { p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [] (const Clip& c) { return c.selected; }), p.clips.end()); });
            return true;
        }
        if (k == juce::KeyPress ('g', cmd, 0)) { groupSelected (true); return true; }
        if (k == juce::KeyPress ('g', cmd | juce::ModifierKeys::shiftModifier, 0)) { groupSelected (false); return true; }
        if (k.getModifiers().isAltDown() && (k.getKeyCode() == juce::KeyPress::leftKey || k.getKeyCode() == juce::KeyPress::rightKey))
        {
            // Alt+Left/Right: nudge the selected clips 1 ms off the beat (Shift = 10 ms); nothing selected = the whole track
            const double ms = (k.getModifiers().isShiftDown() ? 10.0 : 1.0) * (k.getKeyCode() == juce::KeyPress::rightKey ? 1.0 : -1.0);
            const bool any = std::any_of (s.p().clips.begin(), s.p().clips.end(), [] (const Clip& c) { return c.selected; });
            auto lanes = s.selectedLanes;
            if (! any && lanes.isEmpty())
                for (auto& c : s.p().clips) if (c.uid == lastClipUid) lanes.add (c.lane);
            if (! any && lanes.isEmpty()) return true;
            s.edit ("Nudge ms", [&] (Project& pr) {
                const double db = secondsToBeats (ms / 1000.0, pr.bpm);
                for (auto& c : pr.clips) if (any ? c.selected : lanes.contains (c.lane)) c.start = juce::jmax (0.0, c.start + db);
            });
            nudgeTotalMs = (lastNudgeAny == any ? nudgeTotalMs : 0.0) + ms; lastNudgeAny = any;
            nudgeText = juce::String (nudgeTotalMs >= 0 ? "+" : "") + juce::String (nudgeTotalMs, 0) + " ms " + (any ? "(selected clips)" : "(whole track)");
            nudgeShown = juce::Time::getMillisecondCounter();
            repaint();
            return true;
        }
        if (k.getKeyCode() == juce::KeyPress::upKey || k.getKeyCode() == juce::KeyPress::downKey)   // Up/Down: slide the selected clips to the track above/below
        {
            const int dir = k.getKeyCode() == juce::KeyPress::upKey ? -1 : 1;
            int lo = 1 << 30, hi = -1;
            for (auto& c : s.p().clips) if (c.selected) { lo = juce::jmin (lo, c.lane); hi = juce::jmax (hi, c.lane); }
            if (hi < 0) return false;
            if (lo + dir < 0 || hi + dir >= (int) s.p().lanes.size()) return true;
            s.edit ("Move clips", [dir] (Project& p) { for (auto& c : p.clips) if (c.selected) c.lane += dir; });
            return true;
        }
       #if ! HHS_LICENSED
        if (k.getModifiers().isAltDown() && (k.getKeyCode() == 'a' || k.getKeyCode() == 'A'))   // Alt+A: hi-hat style roll on the selected audio clips
        {
            std::vector<int> u;
            for (auto& c : s.p().clips) if (c.selected && c.isAudio()) u.push_back (c.uid);
            if (! u.empty()) rollMenu (u);
            return true;
        }
       #endif
        if (k.getModifiers().isAnyModifierKeyDown() == false)
        {
            const std::pair<int, PTool> keys[] { { 'e', PTool::Select }, { 'p', PTool::Paint }, { 'b', PTool::Paint }, { 'd', PTool::Delete }, { 't', PTool::Mute }, { 'c', PTool::Slice } };
            for (auto& [key, tl] : keys)
                if (k.getKeyCode() == key || k.getKeyCode() == juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key))
                { tool = tl; toolBtns[(int) tl]->setToggleState (true, juce::dontSendNotification); return true; }
        }
        const auto lower = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) k.getKeyCode());
        if (k.getModifiers() == juce::ModifierKeys (cmd) && (lower == 'c' || lower == 'x'))
        {
            clipboard.clear(); clipLo = 1e9;
            for (auto& c : s.p().clips) if (c.selected) { clipboard.push_back (c); clipLo = std::min (clipLo, c.start); }
            if (timeSelB > timeSelA) clipLo = std::min (clipLo, timeSelA);   // time selection: paste keeps the empty space before the first clip
            if (lower == 'x' && ! clipboard.empty())
                s.edit ("Cut clips", [] (Project& p) { p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [] (const Clip& c) { return c.selected; }), p.clips.end()); });
            return true;
        }
        if (k.getModifiers() == juce::ModifierKeys (cmd) && lower == 'v')
        {
            if (clipboard.empty()) return true;
            const double at = juce::jmax (0.0, snap (s.engine.positionBeats()));
            const auto cb = clipboard; const double lo = clipLo;
            s.edit ("Paste clips", [cb, lo, at] (Project& p) {
                for (auto& c : p.clips) c.selected = false;
                for (auto c : cb)
                {
                    c.uid = p.newUid(); c.start = c.start - lo + at; c.selected = true; c.group = 0;
                    c.lane = juce::jlimit (0, juce::jmax (0, (int) p.lanes.size() - 1), c.lane);
                    p.clips.push_back (c);
                }
            });
            return true;
        }
        if (k.getModifiers() == juce::ModifierKeys (cmd) && lower == 'd') { timeSelA = timeSelB = -1; s.live ([] (Project& p) { for (auto& c : p.clips) c.selected = false; }); repaint(); return true; }
        if (k == juce::KeyPress ('b', cmd, 0))
        {
            const double ta = timeSelA, tb = timeSelB;
            if (tb > ta) { timeSelA = tb; timeSelB = tb + (tb - ta); }   // the selection moves onto the copy, so Ctrl+B again keeps stacking
            s.edit ("Duplicate clips", [ta, tb] (Project& p) {
                double lo = 1e9, hi = 0;
                for (auto& c : p.clips) if (c.selected) { lo = std::min (lo, c.start); hi = std::max (hi, c.start + c.length); }
                if (tb > ta) { lo = ta; hi = tb; }   // time selection: duplicate by the whole range (full bars), not the clip ends
                const auto n = p.clips.size();
                for (size_t i = 0; i < n; ++i)
                    if (p.clips[i].selected) { auto c = p.clips[i]; c.uid = p.newUid(); c.start += hi - lo; p.clips[i].selected = false; p.clips.push_back (c); }
            });
            return true;
        }
        if (k == juce::KeyPress ('a', cmd, 0)) { s.live ([] (Project& p) { for (auto& c : p.clips) c.selected = true; }); repaint(); return true; }
        return false;
    }

    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        const auto t = d.description.toString();
        return t == "sample" || t.startsWith ("pattern:") || t.startsWith ("audio:");
    }
    void itemDropped (const SourceDetails& d) override   // drag from the Samples browser / PATTERNS tab
    {
        const auto desc = d.description.toString();
        if (desc.startsWith ("audio:")) { filesDropped ({ desc.fromFirstOccurrenceOf ("audio:", false, false) }, d.localPosition.x, d.localPosition.y); return; }
        if (desc.startsWith ("pattern:"))
        {
            const int pat = desc.fromFirstOccurrenceOf ("pattern:", false, false).getIntValue();
            const double at = juce::jmax (0.0, snap (xToBeat ((float) d.localPosition.x)));
            int lane = laneAt ((float) d.localPosition.y);
            s.edit ("Place pattern", [&] (Project& p) {
                if (pat < 0 || pat >= (int) p.patterns.size()) return;
                if (lane < 0 || lane >= (int) p.lanes.size())
                {
                    Lane l; l.name = p.patterns[(size_t) pat].name; l.mixerTrack = p.addMixerTrack (l.name);
                    p.lanes.push_back (l); lane = (int) p.lanes.size() - 1;
                }
                Clip c; c.uid = p.newUid(); c.lane = lane; c.start = at; c.patternIndex = pat;
                c.length = p.patterns[(size_t) pat].lengthBeats();
                p.clips.push_back (c);
            });
            return;
        }
        if (auto* t = dynamic_cast<juce::FileTreeComponent*> (d.sourceComponent.get()))
        {
            juce::StringArray files;
            for (int i = 0; i < t->getNumSelectedFiles(); ++i) files.add (t->getSelectedFile (i).getFullPathName());
            if (! files.isEmpty()) filesDropped (files, d.localPosition.x, d.localPosition.y);
        }
    }
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! allHumHouseShareFiles (files); }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        const double at = juce::jmax (0.0, snap (xToBeat ((float) x)));
        int lane = laneAt ((float) y);
        s.edit ("Add audio", [&] (Project& p) {
            for (auto& path : files)
            {
                juce::File f (path);
                const double secs = audioSeconds (f);
                if (secs <= 0) continue;
                if (lane < 0 || lane >= (int) p.lanes.size())
                {
                    Lane l; l.name = f.getFileNameWithoutExtension(); l.mixerTrack = p.addMixerTrack (l.name);
                    p.lanes.push_back (l); lane = (int) p.lanes.size() - 1;
                }
                Clip c; c.uid = p.newUid(); c.lane = lane; c.start = at; c.audioFile = f; c.patternIndex = -1;
                c.length = secondsToBeats (secs, p.bpm);
                p.clips.push_back (c);
                ++lane;
            }
        });
    }

private:
    enum class Drag { None, Ruler, TimeSel, Scrub, Move, Resize, Trim, FadeIn, FadeOut, Curve, Marquee, AutoPt };
    int dragPt = -1;
    enum class PTool { Select, Paint, Delete, Mute, Slice };
    PTool tool = PTool::Select;
    juce::OwnedArray<juce::TextButton> toolBtns;
    juce::TextButton groupBtn;
    int lastClipUid = 0;
    void groupSelected (bool on)
    {
        s.edit (on ? "Group clips" : "Ungroup clips", [on] (Project& pr) {
            int g = 0;
            if (on) { for (auto& c : pr.clips) g = juce::jmax (g, c.group); ++g; }
            int n = 0;
            for (auto& c : pr.clips) if (c.selected) { c.group = on ? g : 0; ++n; }
            if (on && n < 2) for (auto& c : pr.clips) if (c.group == g) c.group = 0;
        });
    }
    enum class Part { Body, Left, Right, FadeIn, FadeOut, Curve };
    juce::Point<float> fadeHandle (const Clip& c, juce::Rectangle<float> r, bool in) const
    {
        const float len = (float) ((in ? c.fadeIn : c.fadeOut) * pxPerBeat);
        const float gn = std::pow (0.5f, std::exp (c.fadeCurve));
        return { in ? r.getX() + len * 0.5f : r.getRight() - len * 0.5f, r.getBottom() - gn * r.getHeight() };
    }
    Studio& s;
    std::vector<std::unique_ptr<LaneHeader>> headers;
    juce::TextButton addTrack, addDrummer, loopRec, rts, preroll, clearPunch;
    juce::ComboBox snapBox;
    double plSnap = 1.0;
    float lastPx = -1.0f;
    bool wasPlaying = false;
    std::unique_ptr<juce::AudioThumbnailCache> thumbCache;
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbs;
    double scrollX = 0; int scrollY = 0; float pxPerBeat = 24.0f;
    int laneH = 60;   // even height: tracks fill the view, never thin lines
    Drag drag = Drag::None; int dragClip = -1; bool changed = false;
    bool stretchDrag = false; int stretchMode = 0;   // Alt+drag a clip's right edge: 0 Resample, 1 Stretch (keep pitch)
    std::vector<Clip> dragOrig;
    juce::Point<float> dragStart; juce::Point<int> lastMouse;
    juce::Rectangle<float> marquee; std::vector<bool> marqueeBase;
    double timeSelA = -1, timeSelB = -1;   // ruler time selection (beats)
    std::vector<Clip> clipboard; double clipLo = 0; bool cloneOnDrag = false, cloned = false;

    juce::Rectangle<int> clipArea() const { return getLocalBounds().withTrimmedTop (toolbarH + rulerH).withTrimmedLeft (headerW); }
    float beatToX (double b) const { return (float) headerW + (float) ((b - scrollX) * pxPerBeat); }
    double xToBeat (float x) const { return scrollX + (x - (float) headerW) / pxPerBeat; }
    std::map<int, float> laneMul;   // per-track height (drag a header's bottom edge)
    float zoomH = 1.0f;             // all tracks (Alt+wheel, Make all tracks, side bar)
    juce::Slider vZoom { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    int numLanes() const { return (int) const_cast<Studio&> (s).p().lanes.size(); }
    int laneHOf (int lane) const
    {
        auto it = laneMul.find (lane);
        return juce::jlimit (22, 800, (int) ((float) laneH * zoomH * (it != laneMul.end() ? it->second : 1.0f)));
    }
    int totalLaneH() const { int t = 0; for (int i = 0; i < numLanes(); ++i) t += laneHOf (i); return t; }
    int laneY (int lane) const
    {
        int y = toolbarH + rulerH - scrollY;
        for (int i = 0; i < lane; ++i) y += laneHOf (i);
        return y;
    }
    int laneAt (float y) const
    {
        int top = toolbarH + rulerH - scrollY;
        for (int i = 0; i < numLanes(); ++i) { const int h = laneHOf (i); if (y >= (float) top && y < (float) (top + h)) return i; top += h; }
        return -1;
    }
    double gridSnap() const
    {
        auto& p = const_cast<Studio&> (s).p();
        double g = 1.0;
        while (g * pxPerBeat < 12) g *= 2;
        while (g * pxPerBeat > 80 && g > p.snap && g > 0.0625) g *= 0.5;
        return g;
    }
    double snap (double b) const { return plSnap > 0 ? std::round (b / plSnap) * plSnap : b; }
    void paintOverChildren (juce::Graphics& g) override
    {
        if (nudgeText.isEmpty() || juce::Time::getMillisecondCounter() - nudgeShown > 2500) return;
        auto r = juce::Rectangle<int> (0, toolbarH + 6, 240, 22).withX (getWidth() - 250);
        g.setColour (col::panel.withAlpha (0.92f)); g.fillRoundedRectangle (r.toFloat(), 5.0f);
        g.setColour (col::accent); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("Nudge " + nudgeText, r, juce::Justification::centred);
        juce::Timer::callAfterDelay (2600, [sp = juce::Component::SafePointer<juce::Component> (this)] { if (sp) sp->repaint(); });
    }
    double nudgeTotalMs = 0.0; bool lastNudgeAny = false; juce::String nudgeText; juce::uint32 nudgeShown = 0;
    double beatsFromSeconds (double secs) const { return secondsToBeats (secs, const_cast<Studio&> (s).p().bpm); }
    juce::Rectangle<float> clipRect (const Clip& c) const
    {
        return { beatToX (c.start), (float) laneY (c.lane) + 2.0f, juce::jmax (4.0f, (float) (c.length * pxPerBeat)), (float) laneHOf (c.lane) - 4.0f };
    }
    int hitClip (juce::Point<float> pt, Part& part) const
    {
        auto& p = const_cast<Studio&> (s).p();
        part = Part::Body;
        for (int i = (int) p.clips.size() - 1; i >= 0; --i)
        {
            auto& c = p.clips[(size_t) i];
            if (c.hidden) continue;
            auto r = clipRect (c);
            if (! r.contains (pt)) continue;
            if (c.isAudio())
            {
                if (c.fadeIn * pxPerBeat > 10 && fadeHandle (c, r, true).getDistanceFrom (pt) < 8) { part = Part::Curve; return i; }
                if (c.fadeOut * pxPerBeat > 10 && fadeHandle (c, r, false).getDistanceFrom (pt) < 8) { part = Part::Curve; return i; }
            }
            if (c.isAudio() && pt.y < r.getY() + 12)
            {
                const float fi = r.getX() + (float) (c.fadeIn * pxPerBeat), fo = r.getRight() - (float) (c.fadeOut * pxPerBeat);
                if (std::abs (pt.x - fi) < 8) { part = Part::FadeIn; return i; }
                if (std::abs (pt.x - fo) < 8) { part = Part::FadeOut; return i; }
            }
            if (pt.x > r.getRight() - 6) part = Part::Right;
            else if ((c.isAudio() || c.isAuto()) && pt.x < r.getX() + 6) part = Part::Left;
            return i;
        }
        return -1;
    }
    juce::AudioThumbnail* thumb (const juce::File& f)
    {
        auto key = f.getFullPathName();
        auto it = thumbs.find (key);
        if (it != thumbs.end()) return it->second.get();
        if (! f.existsAsFile()) return nullptr;
        auto t = std::make_unique<juce::AudioThumbnail> (512, formats(), *thumbCache);
        t->setSource (new juce::FileInputSource (f));
        t->addChangeListener (this);
        return (thumbs[key] = std::move (t)).get();
    }
    juce::Rectangle<float> autoBody (const Clip& c) const { return clipRect (c).withTrimmedTop (14).reduced (1, 4); }
    float autoValueToY (const Clip& c, float v) const { auto b = autoBody (c); return b.getBottom() - v * b.getHeight(); }
    float autoYToValue (const Clip& c, float y) const { auto b = autoBody (c); return juce::jlimit (0.0f, 1.0f, (b.getBottom() - y) / juce::jmax (1.0f, b.getHeight())); }
    float autoBeatToX (const Clip& c, double b) const { return beatToX (c.start + b - c.patOffset); }
    int hitAutoPoint (const Clip& c, juce::Point<float> pt) const
    {
        int best = -1; float bd = 8.0f;
        for (int k = 0; k < (int) c.autoPts.size(); ++k)
        {
            const float d = pt.getDistanceFrom ({ autoBeatToX (c, c.autoPts[(size_t) k].beat), autoValueToY (c, c.autoPts[(size_t) k].value) });
            if (d < bd) { bd = d; best = k; }
        }
        return best;
    }
    void drawAutomation (juce::Graphics& g, const Clip& c, juce::Rectangle<float> r, juce::Colour colour)
    {
        if (c.autoPts.empty()) return;
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (r.toNearestInt());
        auto body = autoBody (c);
        juce::Path line, fill;
        const auto& pts = c.autoPts;
        const float x0 = r.getX(), x1 = r.getRight();
        line.startNewSubPath (x0, autoValueToY (c, autoValueAt (pts, c.patOffset)));
        for (auto& q : pts) { const float x = autoBeatToX (c, q.beat); if (x > x0 && x < x1) line.lineTo (x, autoValueToY (c, q.value)); }
        line.lineTo (x1, autoValueToY (c, autoValueAt (pts, c.patOffset + c.length)));
        fill = line; fill.lineTo (x1, body.getBottom()); fill.lineTo (x0, body.getBottom()); fill.closeSubPath();
        g.setColour (colour.withAlpha (0.22f)); g.fillPath (fill);
        g.setColour (colour.brighter (0.4f)); g.strokePath (line, juce::PathStrokeType (1.6f));
        for (auto& q : pts)
        {
            const float x = autoBeatToX (c, q.beat), y = autoValueToY (c, q.value);
            g.setColour (juce::Colours::white); g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
            g.setColour (colour.darker (0.3f)); g.drawEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f, 1.2f);
        }
    }
    void drawClip (juce::Graphics& g, const Clip& c)
    {
        auto& p = s.p();
        auto r = clipRect (c);
        if (r.getRight() < headerW || r.getX() > getWidth()) return;
        juce::Colour colour = c.isAuto() ? juce::Colour (0xff4fd18b) : c.isDrummer() ? juce::Colour (0xffffc93d) : c.isAudio() ? juce::Colour (0xff3d9bff) : (c.patternIndex >= 0 && c.patternIndex < (int) p.patterns.size() ? p.patterns[(size_t) c.patternIndex].colour : col::accent);
        if (c.lane >= 0 && c.lane < (int) p.lanes.size() && ! p.lanes[(size_t) c.lane].colour.isTransparent()) colour = p.lanes[(size_t) c.lane].colour;
        if (! c.colour.isTransparent()) colour = c.colour;
        if (c.muted) colour = col::dim;
        g.setColour (colour.withAlpha (0.28f)); g.fillRoundedRectangle (r, 4);
        g.setColour (colour.withAlpha (0.9f)); g.fillRoundedRectangle (r.withHeight (14), 4);
        auto body = r.withTrimmedTop (14).reduced (1, 2);
        if (c.isAudio())
        {
            if (auto* t = thumb (c.audioFile))
            {
                const double secs = beatsToSeconds (c.length, p.bpm);
                g.setColour (colour.brighter (0.4f));
                t->drawChannels (g, body.toNearestInt(), c.sourceOffset, c.sourceOffset + secs, c.gain * juce::jlimit (1.0f, 8.0f, 0.6f / juce::jmax (0.01f, t->getApproximatePeak())));   // quiet takes stay visible
            }
            // fades
            const float fi = (float) (c.fadeIn * pxPerBeat), fo = (float) (c.fadeOut * pxPerBeat);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            const float ex = std::exp (c.fadeCurve);
            auto fadeShape = [&] (bool in, float len, juce::Path& line) {
                juce::Path pth; const float x0 = in ? r.getX() : r.getRight();
                pth.startNewSubPath (x0, r.getY());
                for (int k = 0; k <= 24; ++k)
                {
                    const float t = (float) k / 24.0f, x = in ? x0 + t * len : x0 - t * len, y = r.getBottom() - std::pow (t, ex) * r.getHeight();
                    pth.lineTo (x, y);
                    if (k == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
                }
                pth.closeSubPath(); return pth; };
            for (int side = 0; side < 2; ++side)
            {
                const bool in = side == 0; const float len = in ? fi : fo;
                if (len <= 1) continue;
                juce::Path line; auto shape = fadeShape (in, len, line);
                g.setColour (juce::Colours::black.withAlpha (0.45f)); g.fillPath (shape);
                g.setColour (col::text.withAlpha (0.7f)); g.strokePath (line, juce::PathStrokeType (1.2f));
                if (len > 10) { auto h = fadeHandle (c, r, in); g.setColour (col::accent2); g.fillEllipse (h.x - 4, h.y - 4, 8, 8); g.setColour (col::text); g.drawEllipse (h.x - 4, h.y - 4, 8, 8, 1.0f); }
            }
            g.setColour (col::text);
            g.fillRect (r.getX() + fi - 3, r.getY(), 6.0f, 6.0f);
            g.fillRect (r.getRight() - fo - 3, r.getY(), 6.0f, 6.0f);
        }
        else if (c.isDrummer())
        {   // Logic-style drummer region: cymbals on top, snare in the middle, kick at the bottom
            const Channel* dc = nullptr; for (auto& ch : p.channels) if (ch.id == c.drummer) dc = &ch;
            if (dc != nullptr)
                if (auto hits = drummerHits (dc->drum, p.bpm, c.length))
                {
                    const float rowH = (body.getHeight() - 6) / 3.0f;
                    for (auto& h : *hits)
                    {
                        if (h.beat >= c.length) break;
                        const int row = h.lane == 0 ? 2 : (h.lane <= 6 || (h.lane >= 25 && h.lane <= 28)) ? 1 : 0;
                        g.setColour (colour.brighter (0.6f).withAlpha (0.35f + 0.65f * h.vel));
                        g.fillRect (body.getX() + (float) (h.beat * pxPerBeat), body.getY() + 3 + row * rowH + rowH * 0.2f, 2.0f, rowH * 0.6f);
                    }
                }
            g.setColour (colour.brighter (0.4f).withAlpha (0.5f));
            for (double bar = 4; bar < c.length; bar += 4) g.fillRect (body.getX() + (float) (bar * pxPerBeat), body.getY(), 1.0f, body.getHeight());
        }
        else if (c.patternIndex >= 0 && c.patternIndex < (int) p.patterns.size())
        {
            auto& pat = p.patterns[(size_t) c.patternIndex];
            const double len = pat.lengthBeats();
            int lo = 127, hi = 0;
            for (auto& [id, list] : pat.notes) for (auto& n : list) { lo = std::min (lo, n.key); hi = std::max (hi, n.key); }
            if (hi >= lo)
            {
                g.setColour (colour.brighter (0.5f));
                for (double off = -std::fmod (juce::jmax (0.0, c.patOffset), len); off < c.length; off += len)
                    for (auto& [id, list] : pat.notes)
                        for (auto& n : list)
                        {
                            if (off + n.start >= c.length || off + n.start < 0.0) continue;
                            const float y = body.getBottom() - 3 - (body.getHeight() - 6) * (float) (n.key - lo) / (float) juce::jmax (1, hi - lo);
                            g.fillRect (body.getX() + (float) ((off + n.start) * pxPerBeat), y, juce::jmax (1.5f, (float) (n.length * pxPerBeat)), 2.0f);
                        }
            }
        }
        if (c.isAuto()) drawAutomation (g, c, r, colour);
        juce::String label = c.isAuto() ? c.autoTarget.name : c.isAudio() ? c.audioFile.getFileNameWithoutExtension()
                                         : c.isDrummer() ? juce::String ("Drummer") : (c.patternIndex >= 0 && c.patternIndex < (int) p.patterns.size() ? p.patterns[(size_t) c.patternIndex].name : juce::String ("?"));
        if (c.isAudio())
        {
            const int ins = p.clipInsert (c);
            if (ins >= 0 && ins < (int) p.mixer.size()) label << "  > " << p.mixer[(size_t) ins].name;
            if (c.takeGroup > 0)
            {
                int n = 0, k = 0;
                for (auto& o : p.clips) if (o.takeGroup == c.takeGroup) { ++n; if (o.uid == c.uid) k = n; }
                if (n > 1) label << "  [take " << k << "/" << n << "]";
            }
        }
        const Lane* ln = c.lane >= 0 && c.lane < (int) p.lanes.size() ? &p.lanes[(size_t) c.lane] : nullptr;
        g.setColour (ln != nullptr && ! ln->nameColour.isTransparent() ? ln->nameColour : col::contrastOn (colour));   // never the same color as the clip
        g.setFont (ln != nullptr && ln->nameFont.isNotEmpty() ? col::laneNameFont (ln->nameFont, 11.0f) : juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (label, r.withHeight (14).reduced (5, 0), juce::Justification::centredLeft, true);
        if (c.selected) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (r, 4, 1.5f); }
        else if (c.patternIndex >= 0 && c.patternIndex == p.currentPattern) { g.setColour (col::accent2); g.drawRoundedRectangle (r.reduced (0.5f), 4, 1.5f); }   // where the open pattern is used
    }
    static void addRollItems (juce::PopupMenu& m)
    {
        const char* names[] { "1/4", "1/8", "1/16", "1/32", "1/8 triplet", "1/16 triplet" };
        for (int i = 0; i < 6; ++i) m.addItem (300 + i, juce::String ("Roll ") + names[i]);
    }
    void rollMenu (std::vector<int> uids)
    {
        juce::PopupMenu m; addRollItems (m);
        m.showMenuAsync ({}, [this, uids] (int r) { applyRoll (uids, r); });
    }
    // ruler range set: only that part rolls; otherwise the whole clip. Audio outside the range is kept.
    void applyRoll (const std::vector<int>& uids, int r)
    {
        static const double divs[] { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
        if (r < 300 || r > 305) return;
        const double div = divs[r - 300], ra = timeSelA, rb = timeSelB;
        s.edit ("Roll clip", [&] (Project& p) { rollAudioClips (p, uids, div, ra, rb); });
    }
    // audio quantize: cut the clip at each hit and snap every hit to the grid (the part before the first hit stays put)
    void quantizeAudioClip (int uid, double div)
    {
        const Clip* cp = nullptr; for (auto& o : s.p().clips) if (o.uid == uid) cp = &o;
        if (cp == nullptr || ! cp->isAudio() || cp->reversed || ! cp->audioFile.existsAsFile()) return;
        const Clip c = *cp; const double bpm = s.p().bpm;
        const double secs = beatsToSeconds (c.length, bpm), gap = juce::jmax (0.03, beatsToSeconds (div, bpm) * 0.5);
        juce::Component::SafePointer<Playlist> safe (this);
        juce::Thread::launch ([safe, c, div, bpm, secs, gap] {
            std::vector<double> on;
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (c.audioFile) })
            {
                const double sr = rd->sampleRate;
                const auto a = (juce::int64) (c.sourceOffset * sr);
                const int len = (int) juce::jmin ((juce::int64) (secs * sr), rd->lengthInSamples - a);
                if (len > 0)
                {
                    juce::AudioBuffer<float> b (2, len);
                    rd->read (&b, 0, len, a, true, true);
                    b.addFrom (0, 0, b, 1, 0, len); b.applyGain (0, 0, len, 0.5f);
                    on = detectOnsets (b.getReadPointer (0), len, sr, gap);
                }
            }
            juce::MessageManager::callAsync ([safe, c, on, div, bpm, secs] {
                auto* pl = safe.getComponent();
                if (pl == nullptr || on.empty()) return;
                pl->s.edit ("Quantize audio", [&] (Project& p) {
                    auto it = std::find_if (p.clips.begin(), p.clips.end(), [&] (const Clip& o) { return o.uid == c.uid; });
                    if (it == p.clips.end()) return;
                    const Clip src = *it;
                    p.clips.erase (it);
                    const bool firstHit = on.front() <= 0.01;
                    std::vector<double> cuts { 0.0 };
                    for (double t : on) if (t > 0.01 && t < secs) cuts.push_back (t);
                    std::vector<double> starts;
                    for (size_t k = 0; k < cuts.size(); ++k)
                    {
                        const double orig = src.start + secondsToBeats (cuts[k], bpm);
                        double q = (k == 0 && ! firstHit) ? orig : std::round (orig / div) * div;
                        if (! starts.empty()) q = std::max (q, starts.back() + 1.0 / 64.0);
                        starts.push_back (std::max (0.0, q));
                    }
                    const double fade = secondsToBeats (0.004, bpm);
                    for (size_t k = 0; k < cuts.size(); ++k)
                    {
                        Clip n = src; n.uid = p.newUid(); n.selected = false;
                        n.start = starts[k]; n.sourceOffset = src.sourceOffset + cuts[k];
                        const double stop = k + 1 < cuts.size() ? starts[k + 1] : starts[k] + secondsToBeats (secs - cuts[k], bpm);
                        n.length = stop - n.start;
                        if (n.length <= 0) continue;
                        n.fadeIn = k == 0 ? src.fadeIn : fade;
                        n.fadeOut = k + 1 == cuts.size() ? src.fadeOut : fade;
                        p.clips.push_back (n);
                    }
                });
            });
        });
    }
    // live "Splitting stems: NN%" badge (top-right of the Playlist) while a split runs
    struct SplitProgress : juce::Component, juce::Timer
    {
        SplitProgress (std::shared_ptr<std::atomic<float>> pr, juce::String n) : prog (std::move (pr)), name (std::move (n)) { setInterceptsMouseClicks (false, false); startTimerHz (15); }
        void timerCallback() override { repaint(); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            const float v = juce::jlimit (0.0f, 1.0f, prog->load());
            g.setColour (juce::Colour (0xe0101418)); g.fillRoundedRectangle (r, 6.0f);
            auto bar = r.reduced (6.0f).removeFromBottom (5.0f);
            g.setColour (juce::Colours::white.withAlpha (0.15f)); g.fillRoundedRectangle (bar, 2.5f);
            g.setColour (juce::Colour (0xff4ad991)); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * v), 2.5f);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Splitting stems: " + juce::String (juce::roundToInt (v * 100.0f)) + "%  " + name,
                        r.reduced (8.0f, 3.0f).withTrimmedBottom (8.0f), juce::Justification::centredLeft, true);
        }
        std::shared_ptr<std::atomic<float>> prog; juce::String name;
    };
    juce::OwnedArray<SplitProgress> splitBars;
    void placeSplitBars()
    {
        for (int i = 0; i < splitBars.size(); ++i)
        {
            splitBars[i]->setBounds (getWidth() - 330, 46 + i * 38, 320, 34);
            splitBars[i]->toFront (false);
        }
    }
    // HumHouse Stem Splitter: runs in the background; the stems land on 4 new tracks under the clip, the original is muted (kept)
    // FL-style consolidate: the selected clips (audio, patterns, Drummer) render to one audio clip on a free
    // track below; the originals are muted, not deleted, so the edit stays undoable and re-editable
    void consolidate (int uid)
    {
        std::vector<int> u; int maxLane = 0;
        for (auto& o : s.p().clips)
            if ((o.selected || o.uid == uid) && ! o.isAuto() && ! o.hidden && ! o.muted) { u.push_back (o.uid); maxLane = juce::jmax (maxLane, o.lane); }
        if (u.empty()) return;
        s.engine.stop();
        const auto f = s.p().recordingsFolder().getChildFile ("Consolidated").getNonexistentChildFile ("Consolidated", ".wav");
        double from = 0, len = 0;
        if (! s.engine.consolidateClips (u, f, from, len))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Consolidate", "Couldn't render the selected clips.");
            return;
        }
        s.edit ("Consolidate", [u, f, from, len, maxLane] (Project& pr) {
            const std::set<int> us (u.begin(), u.end());
            int lane = -1;
            for (int li = maxLane + 1; li < (int) pr.lanes.size() && lane < 0; ++li)
            {
                if (pr.lanes[(size_t) li].muted || pr.lanes[(size_t) li].armed) continue;
                const bool busy = std::any_of (pr.clips.begin(), pr.clips.end(), [&] (const Clip& o) { return o.lane == li && o.start < from + len && o.start + o.length > from; });
                if (! busy) lane = li;
            }
            if (lane < 0) { Lane l; l.name = "Consolidated"; l.mixerTrack = 0; pr.lanes.push_back (l); lane = (int) pr.lanes.size() - 1; }
            for (auto& o : pr.clips) { o.selected = false; if (us.count (o.uid)) o.muted = true; }
            Clip c; c.uid = pr.newUid(); c.lane = lane; c.start = from; c.length = len; c.audioFile = f;
            c.mixerTrack = 0; c.selected = true;   // rendered through its inserts already: plays straight on the master
            pr.clips.push_back (c);
        });
    }

    void humTime (int uid, double grid, float strength)
    {
        const Clip* cp = nullptr; for (auto& o : s.p().clips) if (o.uid == uid) cp = &o;
        if (cp == nullptr || ! cp->isAudio()) return;
        const Clip c = *cp;
        s.engine.stop();
        juce::AudioBuffer<float> in; double sr = 0;
        if (! readClipAudio (c, s.p().bpm, in, sr)) return;
        const auto out = timeRender (in, sr, s.p().bpm, c.start, grid, strength);
        const auto f = s.p().recordingsFolder().getChildFile ("HumTime").getNonexistentChildFile (c.audioFile.getFileNameWithoutExtension() + "_humtime", ".wav");
        if (writeClipAudio (f, out, sr)) useRendered (s, uid, f, "HumTime");
    }

    void splitClipStems (int uid)
    {
        const Clip* cp = nullptr; for (auto& o : s.p().clips) if (o.uid == uid) cp = &o;
        if (cp == nullptr || ! cp->isAudio() || ! cp->audioFile.existsAsFile()) return;
        const Clip c = *cp;
        auto& pr = s.p();
        const auto dir = (pr.projectFile.existsAsFile() ? pr.projectFile.getParentDirectory() : pr.recordingsFolder().getParentDirectory()).getChildFile ("Stems");
        const double secs = beatsToSeconds (c.length, pr.bpm);
        auto prog = std::make_shared<std::atomic<float>> (0.0f);
        addAndMakeVisible (splitBars.add (new SplitProgress (prog, c.audioFile.getFileNameWithoutExtension())));
        placeSplitBars();
        juce::Component::SafePointer<Playlist> safe (this);
        juce::Thread::launch ([safe, c, dir, secs, prog] {
            const auto files = splitStemsToFiles (c.audioFile, c.sourceOffset, secs, dir, prog.get());
            juce::MessageManager::callAsync ([safe, c, files, prog] {
                auto* pl = safe.getComponent();
                if (pl == nullptr) return;
                for (int i = pl->splitBars.size(); --i >= 0;)
                    if (pl->splitBars[i]->prog == prog) pl->splitBars.remove (i);
                pl->placeSplitBars();
                if (files[0] == juce::File()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Stem Splitter", "Couldn't split this clip. The original clip is unchanged."); return; }
                pl->s.edit ("Split stems", [c, files] (Project& p) {
                    const juce::uint32 cols[] { 0xffff5d5d, 0xff3d9bff, 0xffffc93d, 0xff4ad991 };
                    for (int k = 0; k < NumStems; ++k)
                    {
                        const int at = juce::jmin ((int) p.lanes.size(), c.lane + 1 + k);
                        insertLane (p, at);
                        auto& l = p.lanes[(size_t) at]; l.name = stemName (k); l.colour = juce::Colour (cols[k]);
                        Clip n = c; n.uid = p.newUid(); n.lane = at; n.audioFile = files[(size_t) k]; n.sourceOffset = 0;
                        n.selected = false; n.group = 0; n.takeGroup = 0; n.colour = {}; n.mixerTrack = -1; n.muted = false;
                        p.clips.push_back (n);
                    }
                    for (auto& o : p.clips) if (o.uid == c.uid) o.muted = true;   // the original stays, muted
                });
            });
        });
    }
    // Key Finder: key, scale, BPM and tuning offset of an audio clip (analysed off the audio thread)
    void findKeyForClip (const Clip& c)
    {
        juce::Component::SafePointer<Playlist> safe (this);
        const auto file = c.audioFile; const double off = c.sourceOffset, secs = juce::jmin (120.0, beatsToSeconds (c.length, s.p().bpm));
        juce::Thread::launch ([safe, file, off, secs] {
            KeyGuess g; bool ok = false;
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (file) })
            {
                const double sr = rd->sampleRate;
                const auto a = (juce::int64) (off * sr);
                const int len = (int) juce::jmin ((juce::int64) (secs * sr), rd->lengthInSamples - a);
                if (len > (int) sr)
                {
                    juce::AudioBuffer<float> b (2, len);
                    rd->read (&b, 0, len, a, true, true);
                    b.addFrom (0, 0, b, 1, 0, len); b.applyGain (0, 0, len, 0.5f);
                    g = findKey (b.getReadPointer (0), len, sr); ok = true;
                }
            }
            juce::MessageManager::callAsync ([safe, g, ok] { if (auto* pl = safe.getComponent()) pl->showKeyResult (g, ok); });
        });
    }
    void showKeyResult (const KeyGuess& g, bool ok)
    {
        if (! ok) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Key Finder", "Couldn't read this clip (it needs at least 1 second of audio)."); return; }
        const float a4 = 440.0f * std::pow (2.0f, g.cents / 1200.0f);
        const juce::String msg = g.describe() + "\n\nAutoPitch: set Tuning A4 to " + juce::String (a4, 1) + " Hz to tune to this audio.";
        // closest key + its relative only; the title bar X (or Esc) closes it
        struct KeyView : juce::Component
        {
            juce::Label text; juce::TextButton keyBpm { "Set key + BPM" }, keyOnly { "Set key only" }, rel;
            std::function<void (int, int, bool)> apply;
            KeyView (const KeyGuess& g, const juce::String& m)
            {
                text.setText (m, juce::dontSendNotification); text.setJustificationType (juce::Justification::topLeft);
                text.setFont (juce::FontOptions (15.0f));
                const int rr = g.scaleIndex == 1 ? (g.root + 3) % 12 : (g.root + 9) % 12, rs = g.scaleIndex == 1 ? 0 : 1;
                rel.setButtonText ("Set relative: " + KeyGuess::keyName (rr, rs));
                keyBpm.onClick  = [this, g] { done (g.root, g.scaleIndex, true); };
                keyOnly.onClick = [this, g] { done (g.root, g.scaleIndex, false); };
                rel.onClick     = [this, rr, rs] { done (rr, rs, false); };
                for (auto* c : std::initializer_list<juce::Component*> { &text, &keyBpm, &keyOnly, &rel }) addAndMakeVisible (c);
                setSize (520, 190);
            }
            void done (int root, int scale, bool bpm)
            {
                if (apply) apply (root, scale, bpm);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
            }
            void resized() override
            {
                auto b = getLocalBounds().reduced (12);
                auto row = b.removeFromBottom (32);
                const int w = (row.getWidth() - 16) / 3;
                keyBpm.setBounds (row.removeFromLeft (w)); row.removeFromLeft (8);
                keyOnly.setBounds (row.removeFromLeft (w)); row.removeFromLeft (8);
                rel.setBounds (row);
                text.setBounds (b);
            }
        };
        juce::Component::SafePointer<Playlist> safe (this);
        auto* view = new KeyView (g, msg);
        view->apply = [safe, g] (int root, int scale, bool bpm) {
            if (auto* pl = safe.getComponent())
                pl->s.edit ("Key Finder", [g, root, scale, bpm] (Project& p) { p.keyRoot = root; p.scaleIndex = scale; if (bpm && g.bpm > 0) p.bpm = g.bpm; });
        };
        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "Key Finder"; o.content.setOwned (view); o.componentToCentreAround = this;
        o.dialogBackgroundColour = col::panel; o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = false; o.resizable = false;
        o.launchAsync();
    }
    void stretchClips()
    {
        auto& p = s.p();
        const auto dir = p.recordingsFolder();
        std::vector<std::pair<int, juce::File>> baked;
        for (size_t i = 0; i < p.clips.size(); ++i)
        {
            const auto& c = p.clips[i]; const auto& o = dragOrig[i];
            if (! c.isAudio() || c.uid != o.uid || std::abs (c.length - o.length) < 1.0e-6 || o.length <= 0) continue;
            const auto f = bakeClipStretch (o.audioFile, o.sourceOffset, beatsToSeconds (o.length, p.bpm), c.length / o.length, stretchMode == 1, dir);
            if (f != juce::File()) baked.push_back ({ c.uid, f });
        }
        auto orig = dragOrig;
        s.edit (stretchMode == 1 ? "Stretch clip" : "Resample clip", [orig, baked] (Project& pr) {
            for (size_t i = 0; i < pr.clips.size() && i < orig.size(); ++i)
            {
                auto& c = pr.clips[i];
                auto it = std::find_if (baked.begin(), baked.end(), [&] (const auto& b) { return b.first == c.uid; });
                if (it == baked.end()) { if (c.uid == orig[i].uid && c.isAudio()) c.length = orig[i].length; continue; }   // couldn't render: unchanged
                const double k = c.length / orig[i].length;
                c.audioFile = it->second; c.sourceOffset = 0; c.fadeIn = orig[i].fadeIn * k; c.fadeOut = orig[i].fadeOut * k;
            }
        });
    }
    void clipMenu (int index)
    {
        auto& p = s.p();
        const auto clip = p.clips[(size_t) index];
        juce::PopupMenu m, route, takes, fades;
        int nSel = 0; for (auto& o : p.clips) if (o.selected || o.uid == clip.uid) ++nSel;
        const juce::String sel = nSel > 1 ? " (" + juce::String (nSel) + " clips)" : juce::String();
        m.addItem (1, "Delete" + sel);
        m.addItem (2, "Split at playhead" + sel, clip.isAudio());
        m.addItem (3, (clip.muted ? "Unmute" : "Mute") + sel);
        fades.addItem (70, "Short fades (click-free)"); fades.addItem (71, "Fade in 1 beat"); fades.addItem (72, "Fade out 1 beat");
        fades.addItem (73, "Fade in + out 1 bar"); fades.addItem (74, "Clear fades");
        m.addSubMenu ("Fades (selected clips)", fades, clip.isAudio());
        for (size_t i = 0; i < p.mixer.size(); ++i)
            route.addItem (100 + (int) i, i == 0 ? juce::String ("Master") : juce::String ((int) i) + " " + p.mixer[i].name, true, p.clipInsert (clip) == (int) i);
        route.addSeparator(); route.addItem (99, "Follow track insert");
        m.addSubMenu ("Route to insert", route, clip.isAudio());
        std::vector<int> group;
        if (clip.takeGroup > 0)
            for (auto& o : p.clips) if (o.takeGroup == clip.takeGroup) group.push_back (o.uid);
        for (size_t k = 0; k < group.size(); ++k) takes.addItem (1000 + group[k], "Take " + juce::String ((int) k + 1), true, group[k] == clip.uid);
        m.addSubMenu ("Takes", takes, group.size() > 1);
        m.addItem (4, "Reverse" + sel, clip.isAudio(), clip.reversed);
       #if ! HHS_LICENSED
        juce::PopupMenu roll; addRollItems (roll);
        m.addSubMenu ("Roll (hi-hat style)   Alt+A", roll, clip.isAudio());
       #endif
        {
            juce::PopupMenu qm;
            const char* qn[] { "1/4", "1/8", "1/16", "1/32", "1/8 triplet", "1/16 triplet" };
            for (int i = 0; i < 6; ++i) qm.addItem (320 + i, juce::String ("Quantize ") + qn[i]);
            m.addSubMenu ("Quantize hits to grid (cuts at each hit)", qm, clip.isAudio() && ! clip.reversed);
        }
        m.addItem (5, "Split by channel (one clip per instrument)", clip.patternIndex >= 0);
        m.addItem (8, "Select all uses of this pattern", clip.patternIndex >= 0);
        m.addItem (15, "Consolidate (render selected clips to one audio clip)", ! clip.isAuto());
        m.addItem (17, "HumTone: edit the pitch of each note...", clip.isAudio());
        {
            juce::PopupMenu tm;
            tm.addItem (340, "Warp hits to 1/16"); tm.addItem (341, "Warp hits to 1/8"); tm.addItem (343, "Warp hits to 1/4");
            tm.addItem (342, "Warp hits to 1/16 at 50% (keeps the feel)");
            m.addSubMenu ("HumTime: warp timing to the grid (stretch, no cuts)", tm, clip.isAudio() && ! clip.reversed);
        }
        if (clip.lane >= 0 && clip.lane < (int) p.lanes.size() && p.lanes[(size_t) clip.lane].compLane >= 0)
            m.addItem (16, "Use in comp (this clip, or the ruler range on it)", clip.isAudio());
        m.addItem (6, "Group selected clips"); m.addItem (7, "Ungroup", clip.group > 0);
        {
            auto cm = colourMenu (30);
            cm.addItem (43, "Use track color", ! clip.colour.isTransparent());
            m.addSubMenu ("Color (selected clips)", cm);
        }
        if (clip.lane >= 0 && clip.lane < (int) p.lanes.size()) m.addSubMenu ("Track color", colourMenu (50));
       #if ! HHS_LICENSED
        m.addItem (9, "Find key / BPM / tune", clip.isAudio());
        {
            juce::PopupMenu sm;
            sm.addItem (12, "Resample (speed + pitch, like tape)", true, stretchMode == 0);
            sm.addItem (13, "Stretch (keep pitch)", true, stretchMode == 1);
            m.addSubMenu ("Stretch mode (Alt+drag clip edge)", sm, clip.isAudio());
        }
        if (clip.isDrummer()) m.addItem (10, "Drummer settings (kit, style, mix)...");
       #if ! HHS_LICENSED
        m.addItem (11, "Split stems (Drums, Bass, Vocals, Melodies)", clip.isAudio());
       #endif
       #endif
        std::vector<AutoTarget> autoT;
        if (! clip.isAuto())
        {
            juce::PopupMenu am;
            if (clip.isAudio()) am = automateMenu (s, autoT, 2000, p.clipInsert (clip), 0, -1);
            else
            {
                std::vector<int> chans;
                if (clip.isDrummer()) chans.push_back (clip.drummer);
                else if (clip.patternIndex >= 0 && clip.patternIndex < (int) p.patterns.size())
                    for (auto& ch : p.channels)
                        if (auto it = p.patterns[(size_t) clip.patternIndex].notes.find (ch.id); it != p.patterns[(size_t) clip.patternIndex].notes.end() && ! it->second.empty())
                            chans.push_back (ch.id);
                for (int id : chans)
                    if (auto* ch = p.findChannel (id))
                        am.addSubMenu (ch->name, automateMenu (s, autoT, 2000, ch->mixerTrack, 0, id));
            }
            m.addSeparator();
            m.addSubMenu ("Automate (new track below)", am, autoT.size() > 0);
        }
        m.showMenuAsync ({}, [this, uid = clip.uid, autoT, autoLane = clip.lane] (int r) {
            if (r == 0) return;
            if (r >= 2000) { if (r - 2000 < (int) autoT.size()) createAutomationClip (s, autoT[(size_t) (r - 2000)], autoLane); return; }
            if (r >= 300 && r <= 305)
            {
                std::vector<int> u;
                for (auto& o : s.p().clips) if ((o.selected || o.uid == uid) && o.isAudio()) u.push_back (o.uid);
                applyRoll (u, r);
                return;
            }
            if (r >= 320 && r <= 325)
            {
                std::vector<int> u;
                for (auto& o : s.p().clips) if ((o.selected || o.uid == uid) && o.isAudio() && ! o.reversed) u.push_back (o.uid);
                static const double divs[] { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
                for (int q : u) quantizeAudioClip (q, divs[r - 320]);
                return;
            }
            if (r == 15) { consolidate (uid); return; }
            if (r == 17) { new ToolWindow ("HumTone - pitch of each note", new HumToneView (s, uid)); return; }
            if (r >= 340 && r <= 343) { static const double g[] { 0.25, 0.5, 0.25, 1.0 }; humTime (uid, g[r - 340], r == 342 ? 0.5f : 1.0f); return; }
            if (r == 16)
            {
                const double a = timeSelA, b = timeSelB;
                s.edit ("Use in comp", [uid, a, b] (Project& p) {
                    const Clip* src = nullptr;
                    for (auto& c : p.clips) if (c.uid == uid) src = &c;
                    if (src == nullptr || src->lane < 0 || src->lane >= (int) p.lanes.size()) return;
                    const int comp = p.lanes[(size_t) src->lane].compLane;
                    if (comp < 0 || comp >= (int) p.lanes.size()) return;
                    double r0 = src->start, r1 = src->start + src->length;
                    if (b > a && a < r1 && b > r0) { r0 = juce::jmax (r0, a); r1 = juce::jmin (r1, b); }
                    Clip piece = *src; piece.uid = p.newUid(); piece.sourceOffset += (r0 - src->start) * 60.0 / p.bpm;
                    piece.start = r0; piece.length = r1 - r0; piece.fadeIn = piece.fadeOut = 0.01;
                    placeOver (p, piece, comp);
                });
                return;
            }
            if (r == 11) { splitClipStems (uid); return; }
            if (r == 12 || r == 13) { stretchMode = r - 12; return; }
            if (r == 10) { for (auto& o : s.p().clips) if (o.uid == uid && o.isDrummer()) { openChannelEditor (s, o.drummer); break; } return; }
            if (r == 9) { for (auto& o : s.p().clips) if (o.uid == uid) { findKeyForClip (o); break; } return; }
            if (r >= 30 && r <= 43)
            {
                const auto c = r == 43 ? juce::Colour() : pickedColour (r - 30);
                s.edit ("Clip color", [uid, c] (Project& pr) { for (auto& o : pr.clips) if (o.selected || o.uid == uid) o.colour = c; });
                return;
            }
            if (r >= 50 && r <= 62)
            {
                const auto c = pickedColour (r - 50);
                s.edit ("Track color", [uid, c] (Project& pr) {
                    std::set<int> ls; for (auto& o : pr.clips) if (o.selected || o.uid == uid) ls.insert (o.lane);
                    for (int li : ls) if (li >= 0 && li < (int) pr.lanes.size()) pr.lanes[(size_t) li].colour = c; });
                return;
            }
            s.edit ("Clip edit", [&] (Project& pr) {
                auto it = std::find_if (pr.clips.begin(), pr.clips.end(), [uid] (const Clip& c) { return c.uid == uid; });
                if (it == pr.clips.end()) return;
                auto& c = *it;
                auto forSel = [&] (std::function<void (Clip&)> f) { for (auto& o : pr.clips) if ((o.selected || o.uid == uid) && o.isAudio()) f (o); };
                const double shortFade = secondsToBeats (0.005, pr.bpm);
                if (r == 1) { pr.clips.erase (std::remove_if (pr.clips.begin(), pr.clips.end(), [uid] (const Clip& o) { return o.selected || o.uid == uid; }), pr.clips.end()); return; }
                else if (r == 2)
                {
                    const double at = s.engine.positionBeats();
                    std::vector<Clip> made;
                    forSel ([&] (Clip& o) {
                        if (! (at > o.start && at < o.start + o.length)) return;
                        Clip b = o; b.uid = pr.newUid();
                        b.start = at; b.length = o.start + o.length - at; b.sourceOffset += beatsToSeconds (at - o.start, pr.bpm); b.patOffset += at - o.start;
                        b.fadeIn = shortFade; o.length = at - o.start; o.fadeOut = shortFade; b.selected = false;
                        made.push_back (b);
                    });
                    for (auto& b : made) pr.clips.push_back (b);
                }
                else if (r == 3) { const bool m = ! c.muted; for (auto& o : pr.clips) if (o.selected || o.uid == uid) o.muted = m; }
                else if (r == 8)
                {
                    const int pat = c.patternIndex;
                    for (auto& o : pr.clips) o.selected = ! o.isAudio() && o.patternIndex == pat;
                    pr.currentPattern = pat;
                }
                else if (r == 4) { const bool rv = ! c.reversed; forSel ([rv] (Clip& o) { o.reversed = rv; }); }
                else if (r == 6 || r == 7)
                {
                    int g = 0;
                    if (r == 6) { for (auto& o : pr.clips) g = juce::jmax (g, o.group); ++g; }
                    const int old = c.group;
                    for (auto& o : pr.clips) if (o.selected || o.uid == uid || (r == 7 && old > 0 && o.group == old)) o.group = g;
                }
                else if (r == 5 && c.patternIndex >= 0 && c.patternIndex < (int) pr.patterns.size())
                {
                    const Clip orig = c;
                    const Pattern src = pr.patterns[(size_t) orig.patternIndex];
                    std::vector<Clip> made;
                    int lane = orig.lane;
                    for (auto& ch : pr.channels)
                    {
                        auto nit = src.notes.find (ch.id);
                        if (nit == src.notes.end() || nit->second.empty()) continue;
                        Pattern one; one.name = src.name + " - " + ch.name; one.colour = ch.colour;
                        one.notes[ch.id] = nit->second;
                        pr.patterns.push_back (one);
                        while (lane >= (int) pr.lanes.size())
                        {
                            Lane l; l.name = "Track " + juce::String (pr.lanes.size() + 1); l.mixerTrack = pr.addMixerTrack (l.name); pr.lanes.push_back (l);
                        }
                        Clip nc = orig; nc.uid = pr.newUid(); nc.lane = lane++; nc.patternIndex = (int) pr.patterns.size() - 1; nc.group = 0; nc.selected = true;
                        made.push_back (nc);
                    }
                    if (made.size() > 1)
                    {
                        pr.clips.erase (std::remove_if (pr.clips.begin(), pr.clips.end(), [uid] (const Clip& o) { return o.uid == uid; }), pr.clips.end());
                        for (auto& o : pr.clips) o.selected = false;
                        for (auto& nc : made) pr.clips.push_back (nc);
                    }
                    else if (! made.empty()) pr.patterns.pop_back();
                    return;
                }
                else if (r == 70) forSel ([&] (Clip& o) { o.fadeIn = o.fadeOut = shortFade; });
                else if (r == 71) forSel ([] (Clip& o) { o.fadeIn = juce::jmin (1.0, o.length * 0.5); });
                else if (r == 72) forSel ([] (Clip& o) { o.fadeOut = juce::jmin (1.0, o.length * 0.5); });
                else if (r == 73) forSel ([&] (Clip& o) { o.fadeIn = o.fadeOut = juce::jmin ((double) pr.beatsPerBar, o.length * 0.5); });
                else if (r == 74) forSel ([] (Clip& o) { o.fadeIn = o.fadeOut = 0; });
                else if (r == 99) c.mixerTrack = -1;
                else if (r >= 100 && r < 1000) forSel ([r] (Clip& o) { o.mixerTrack = r - 100; });
                else if (r >= 1000) { const int keep = r - 1000; for (auto& o : pr.clips) if (o.takeGroup == c.takeGroup) o.hidden = o.uid != keep; }
            });
            if (r == 8)   // jump the view to the first use
            {
                double first = 1.0e9; int lane = 0;
                for (auto& o : s.p().clips) if (o.selected && o.start < first) { first = o.start; lane = o.lane; }
                if (first < 1.0e9)
                {
                    scrollX = juce::jmax (0.0, first - (double) s.p().beatsPerBar);
                    scrollY = juce::jmax (0, laneY (lane) + scrollY - (toolbarH + rulerH));
                    resized();
                }
                repaint();
            }
        });
    }
    void layoutHeaders()
    {
        for (size_t i = 0; i < headers.size(); ++i)
        {
            const int y = laneY ((int) i);
            headers[i]->setBounds (0, y, headerW, laneHOf ((int) i));
            headers[i]->setVisible (y + laneHOf ((int) i) > toolbarH + rulerH);
        }
    }
    void refresh()
    {
        auto& p = s.p();
        if (headers.size() != p.lanes.size())
        {
            headers.clear();
            for (size_t i = 0; i < p.lanes.size(); ++i) 
            {
                headers.push_back (std::make_unique<LaneHeader> (s, (int) i));
                headers.back()->onHeight = [this] (int lane, int h)
                {
                    laneMul[lane] = juce::jlimit (0.3f, 8.0f, (float) h / (float) juce::jmax (1.0f, (float) laneH * zoomH));
                    layoutHeaders(); repaint();
                };
                headers.back()->onAllHeights = [this] (int size)
                {
                    laneMul.clear();
                    zoomH = size == 0 ? 0.6f : size == 1 ? 1.0f : 1.8f;
                    vZoom.setValue (zoomH, juce::dontSendNotification);
                    layoutHeaders(); repaint();
                };
                addAndMakeVisible (*headers.back());
            }
            layoutHeaders();
        }
        for (auto& h : headers) h->refresh();
        loopRec.setToggleState (p.loopRecord, juce::dontSendNotification);
        rts.setToggleState (p.returnToStart, juce::dontSendNotification);
        preroll.setToggleState (p.preRollBars > 0, juce::dontSendNotification);
        repaint();
    }
    void changeListenerCallback (juce::ChangeBroadcaster* src) override
    {
        if (src == &s) refresh(); else repaint();
    }
    void timerCallback() override
    {
        const bool playing = s.engine.isPlaying();
        if (! playing) { if (wasPlaying) repaint(); wasPlaying = false; lastPx = -1.0f; return; }
        wasPlaying = true;
        const double pos = s.engine.positionBeats();
        float px = beatToX (pos);
        if (px > getWidth() - 40 || px < headerW) { scrollX = juce::jmax (0.0, pos - 2.0); repaint(); lastPx = beatToX (pos); }
        else
        {
            // only redraw what moves: the old + new playhead strips, and the growing take while recording
            auto strip = [this] (float x) { if (x >= 0) repaint ((int) x - 8, toolbarH, 16, getHeight() - toolbarH); };
            strip (lastPx); strip (px);
            if (s.engine.isRecording())
            {
                double b0 = pos;
                s.engine.forEachLiveTake ([&] (int, double sb, const float*, int, double) { b0 = std::min (b0, sb); });
                const int x0 = juce::jmax (headerW, (int) beatToX (b0) - 2);
                if (px + 8 > x0) repaint (x0, toolbarH + rulerH, (int) px + 8 - x0, getHeight() - toolbarH - rulerH);
            }
            lastPx = px;
        }
        for (auto& h : headers) if (h->arm.getToggleState()) h->repaint();
    }
};
}

std::unique_ptr<juce::Component> createPlaylist (Studio& s) { return std::make_unique<Playlist> (s); }
}

namespace hhs
{
// ---------------------------------------------------------------- automation (FL): menus + new automation track
static juce::String fxSlotName (Studio& s, int track, const FxSlot& f)
{
    if (auto* pl = s.engine.fxPlugin (track, f.uid)) return pl->getName();
    for (auto& e : builtinEffects()) if (f.type == e.id) return e.name;
    return f.type.isNotEmpty() ? f.type : juce::String ("Effect");
}
static void addFxAutoItems (Studio& s, juce::PopupMenu& m, std::vector<AutoTarget>& out, int base, int track, const FxSlot& f)
{
    const auto fxName = fxSlotName (s, track, f);
    auto add = [&] (const juce::String& param, const juce::String& label)
    {
        AutoTarget t; t.kind = AutoFxParam; t.track = track; t.fxUid = f.uid; t.param = param; t.name = fxName + " - " + label;
        m.addItem (base + (int) out.size(), label); out.push_back (t);
    };
    if (auto* b = s.engine.builtinFx (track, f.uid)) { for (auto& d : b->params()) add (d.id, d.name); }
    else if (auto* pl = s.engine.fxPlugin (track, f.uid))
    {
        const auto& ps = pl->getParameters();
        for (int k = 0; k < juce::jmin (128, ps.size()); ++k) add ("#" + juce::String (k), ps[k]->getName (32));
    }
}
juce::PopupMenu automateMenu (Studio& s, std::vector<AutoTarget>& out, int base, int track, int fxUid, int channelId)
{
    juce::PopupMenu m;
    auto& p = s.p();
    auto add = [&] (juce::PopupMenu& mm, int kind, const juce::String& name, const juce::String& label)
    {
        AutoTarget t; t.kind = kind; t.track = track; t.channel = channelId; t.name = name;
        mm.addItem (base + (int) out.size(), label); out.push_back (t);
    };
    if (auto* ch = channelId >= 0 ? p.findChannel (channelId) : nullptr)
    {
        add (m, AutoChanVol, ch->name + " - Volume", "Volume");
        add (m, AutoChanPan, ch->name + " - Pan", "Pan");
        add (m, AutoChanPitch, ch->name + " - Pitch", "Pitch");
        m.addSeparator();
    }
    if (track >= 0 && track < (int) p.mixer.size())
    {
        auto& mt = p.mixer[(size_t) track];
        const auto ins = mt.name.isNotEmpty() ? mt.name : juce::String ("Insert ") + juce::String (track);
        add (m, AutoInsertVol, ins + " - Volume", ins + " volume");
        add (m, AutoInsertPan, ins + " - Pan", ins + " pan");
        for (auto& f : mt.fx)
        {
            if (fxUid > 0 && f.uid != fxUid) continue;
            if (fxUid > 0) { m.addSeparator(); addFxAutoItems (s, m, out, base, track, f); }
            else { juce::PopupMenu sub; addFxAutoItems (s, sub, out, base, track, f); m.addSubMenu (fxSlotName (s, track, f), sub); }
        }
    }
    return m;
}
void createAutomationClip (Studio& s, const AutoTarget& t, int belowLane)
{
    auto& p = s.p();
    float v = 0.5f;   // the envelope starts flat at the knob's current value
    if (t.kind == AutoInsertVol || t.kind == AutoInsertPan)
    {
        if (t.track >= 0 && t.track < (int) p.mixer.size()) v = autoFromValue (t.kind, t.kind == AutoInsertVol ? p.mixer[(size_t) t.track].volume : p.mixer[(size_t) t.track].pan);
    }
    else if (t.kind == AutoFxParam)
    {
        if (auto* b = s.engine.builtinFx (t.track, t.fxUid))
        {
            const int i = b->paramIndex (t.param);
            if (i >= 0) { const auto& d = b->params()[(size_t) i]; v = (b->p (i) - d.min) / juce::jmax (1.0e-6f, d.max - d.min); }
        }
        else if (auto* pl = s.engine.fxPlugin (t.track, t.fxUid))
        {
            const int i = t.param.substring (1).getIntValue();
            if (i >= 0 && i < pl->getParameters().size()) v = pl->getParameters()[i]->getValue();
        }
    }
    else if (auto* ch = p.findChannel (t.channel))
        v = autoFromValue (t.kind, t.kind == AutoChanVol ? ch->volume : t.kind == AutoChanPan ? ch->pan : ch->pitch);
    v = juce::jlimit (0.0f, 1.0f, v);
    s.edit ("Automate " + t.name, [&] (Project& pr) {
        const int at = juce::jlimit (0, (int) pr.lanes.size(), belowLane + 1);
        Lane l;
        l.name = t.name;
        l.mixerTrack = belowLane >= 0 && belowLane < (int) pr.lanes.size() ? pr.lanes[(size_t) belowLane].mixerTrack : 0;
        for (auto& c : pr.clips) if (c.lane >= at) ++c.lane;
        pr.lanes.insert (pr.lanes.begin() + at, l);
        Clip c;
        c.uid = pr.newUid(); c.lane = at; c.start = 0.0;
        c.length = juce::jmax (pr.songLengthBeats(), (double) pr.beatsPerBar * 4.0);
        c.autoTarget = t;
        c.autoPts = { { 0.0, v }, { c.length, v } };
        pr.clips.push_back (c);
    });
}
} // namespace hhs
