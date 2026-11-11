#include <set>
#include "UI.h"
#include <juce_dsp/juce_dsp.h>

namespace hhs
{
namespace
{
// Hum Verse (HumHouse DJ): two decks + crossfader on their own audio callback beside the engine (never inside the engine's block)
struct Deck : public juce::Component, private juce::Timer
{
    Deck (juce::AudioFormatManager& f, juce::TimeSliceThread& t, juce::AudioThumbnailCache& tc, juce::String nm, bool right)
        : fm (f), io (t), title (std::move (nm)), mirror (right), thumb (256, f, tc)
    {
        for (auto* b : { &load, &play, &cue, &sync, &push }) addAndMakeVisible (b);
        push.setTooltip ("PUSH: drop this song on the Record page as an audio clip at the playhead (right-click: Beat Channel Rack / end of song)");
        push.onClick = [this] { if (onPush && file.existsAsFile()) onPush(); };
        for (int i = 0; i < 4; ++i)
        {
            auto* p = pads.add (new juce::TextButton (juce::String (i + 1)));
            p->setColour (juce::TextButton::buttonColourId, padColour (i).withAlpha (0.25f));
            p->onClick = [this, i] { hotCue (i); };
            p->setTooltip ("Hot cue " + juce::String (i + 1) + ": click to set / jump, shift-click to clear");
            addAndMakeVisible (p);
        }
        load.onClick = [this] { pick(); };
        play.onClick = [this] { if (tp.isPlaying()) tp.stop(); else tp.start(); };
        cue.onClick = [this] { if (tp.isPlaying()) { tp.stop(); tp.setPosition (cuePos); } else { cuePos = tp.getCurrentPosition(); tp.setPosition (cuePos); } };
        sync.onClick = [this] { if (onSync) onSync(); };
        for (auto* lb : { &loop4, &loop8 })
        {
            lb->setClickingTogglesState (true);
            lb->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2fb36b));
            addAndMakeVisible (lb);
        }
        loop4.onClick = [this] { setLoop (loop4, loop8, 4); };
        loop8.onClick = [this] { setLoop (loop8, loop4, 8); };
        pitch.setSliderStyle (juce::Slider::LinearVertical); pitch.setRange (-16.0, 16.0, 0.01); pitch.setValue (0.0);
        pitch.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pitch.setDoubleClickReturnValue (true, 0.0);
        pitch.onValueChange = [this] { rs.setResamplingRatio (1.0 + pitch.getValue() / 100.0); if (onBpm) onBpm(); };
        addAndMakeVisible (pitch);
        startTimerHz (phoneUI ? 30 : 60);
    }
    ~Deck() override { tp.setSource (nullptr); }
    static juce::Colour padColour (int i) { const juce::Colour c[] { juce::Colours::red, juce::Colours::orange, juce::Colours::yellow, juce::Colours::limegreen }; return c[i & 3]; }
    void hotCue (int i)
    {
        if (juce::ModifierKeys::currentModifiers.isShiftDown()) cues[(size_t) i] = -1.0;
        else if (cues[(size_t) i] < 0) cues[(size_t) i] = tp.getCurrentPosition();
        else tp.setPosition (cues[(size_t) i]);
        pads[i]->setColour (juce::TextButton::buttonColourId, padColour (i).withAlpha (cues[(size_t) i] >= 0 ? 0.85f : 0.25f));
    }
    void pick()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a song onto " + title, juce::File(), fm.getWildcardForAllFormats());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
            if (auto f = fc.getResult(); f.existsAsFile()) loadFile (f); });
    }
    void loadFile (const juce::File& f)
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) return;
        const double sr = r->sampleRate;
        bpm = guessBpm (*r, f.getFileNameWithoutExtension());
        analyse (*r);
        tp.stop(); tp.setSource (nullptr);
        src = std::make_unique<juce::AudioFormatReaderSource> (r.release(), true);
        tp.setSource (src.get(), 32768, &io, sr, 2);   // read-ahead on its own thread: the audio callback never touches the disk
        thumb.setSource (new juce::FileInputSource (f));   // waveform builds on the thumbnail thread
        name = f.getFileNameWithoutExtension(); cuePos = 0.0; file = f;
        for (int i = 0; i < 4; ++i) { cues[(size_t) i] = -1.0; pads[i]->setColour (juce::TextButton::buttonColourId, padColour (i).withAlpha (0.25f)); }
        repaint();
    }
    static double guessBpm (juce::AudioFormatReader& r, const juce::String& n)
    {
        const auto low = n.toLowerCase();
        if (const int k = low.indexOf ("bpm"); k > 0)
        {
            int a = k; while (a > 0 && (juce::CharacterFunctions::isDigit (low[a - 1]) || low[a - 1] == ' ')) --a;
            if (const double v = low.substring (a, k).trim().getDoubleValue(); v >= 60 && v <= 200) return v;
        }
        // onset-energy autocorrelation over 30 s from the middle of the song (hop = 512 samples)
        const int hop = 512; const juce::int64 len = juce::jmin ((juce::int64) (r.sampleRate * 30.0), r.lengthInSamples);
        const juce::int64 start = juce::jmax ((juce::int64) 0, r.lengthInSamples / 2 - len / 2);
        juce::AudioBuffer<float> b (2, (int) len);
        r.read (&b, 0, (int) len, start, true, true);
        std::vector<float> env ((size_t) (len / hop), 0.0f);
        float prev = 0.0f;
        for (size_t i = 0; i < env.size(); ++i)
        {
            float e = 0.0f;
            for (int c = 0; c < b.getNumChannels(); ++c) e += b.getRMSLevel (c, (int) i * hop, hop);
            env[i] = juce::jmax (0.0f, e - prev); prev = e;
        }
        double best = 0.0, bestBpm = 120.0;
        for (double t = 70.0; t <= 180.0; t += 0.5)
        {
            const double lag = 60.0 / t * r.sampleRate / hop;
            double sum = 0.0;
            for (size_t i = 0; i + (size_t) (lag * 4) + 1 < env.size(); ++i)
                for (int m = 1; m <= 4; ++m) sum += env[i] * env[i + (size_t) (lag * m)];
            if (sum > best) { best = sum; bestBpm = t; }
        }
        return bestBpm;
    }
    // key (Krumhansl profiles on a 30 s chroma) + first strong beat for the grid
    void analyse (juce::AudioFormatReader& r)
    {
        const double sr = r.sampleRate;
        {
            juce::AudioBuffer<float> head (1, (int) juce::jmin ((juce::int64) (sr * 15.0), r.lengthInSamples));
            r.read (&head, 0, head.getNumSamples(), 0, true, false);
            const float pk = head.getMagnitude (0, head.getNumSamples());
            gridStart = 0.0;
            for (int i = 0; i < head.getNumSamples(); ++i) if (std::abs (head.getSample (0, i)) > pk * 0.5f) { gridStart = i / sr; break; }
        }
        constexpr int order = 13, N = 1 << order;
        juce::dsp::FFT fft (order);
        const juce::int64 len = juce::jmin ((juce::int64) (sr * 30.0), r.lengthInSamples);
        const juce::int64 start = juce::jmax ((juce::int64) 0, r.lengthInSamples / 2 - len / 2);
        juce::AudioBuffer<float> b (1, (int) len);
        r.read (&b, 0, (int) len, start, true, false);
        std::array<double, 12> chroma {};
        std::vector<float> w ((size_t) N * 2);
        for (juce::int64 pos = 0; pos + N <= len; pos += N / 2)
        {
            std::fill (w.begin(), w.end(), 0.0f);
            for (int i = 0; i < N; ++i) w[(size_t) i] = b.getSample (0, (int) pos + i) * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N));
            fft.performFrequencyOnlyForwardTransform (w.data());
            for (int k = 1; k < N / 2; ++k)
            {
                const double hz = k * sr / N;
                if (hz < 55.0 || hz > 2000.0) continue;
                const int pc = ((int) std::lround (12.0 * std::log2 (hz / 440.0)) + 9 + 1200) % 12;
                chroma[(size_t) pc] += w[(size_t) k];
            }
        }
        static const double maj[] { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double mnr[] { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        double best = -1e9; int bestPc = 0; bool minor = false;
        for (int root = 0; root < 12; ++root)
            for (int m = 0; m < 2; ++m)
            {
                double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
                for (int i = 0; i < 12; ++i)
                {
                    const double x = chroma[(size_t) ((root + i) % 12)], y = m ? mnr[i] : maj[i];
                    sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
                }
                const double c = (12 * sxy - sx * sy) / std::sqrt (juce::jmax (1e-12, (12 * sxx - sx * sx) * (12 * syy - sy * sy)));
                if (c > best) { best = c; bestPc = root; minor = m == 1; }
            }
        static const int cam[] { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
        static const char* names[] { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        key = juce::String (cam[minor ? (bestPc + 3) % 12 : bestPc]) + (minor ? "A" : "B") + "  " + names[bestPc] + (minor ? "m" : "");
    }
    double liveBpm() const { return bpm * (1.0 + pitch.getValue() / 100.0); }
    // drag the BPM number up / down (Shift = fine), wheel nudges, double-click = match the other deck
    juce::Rectangle<int> bpmRect() const { return { getWidth() - 86, 2, 80, 36 }; }
    void setLiveBpm (double v) { if (bpm > 0) pitch.setValue (juce::jlimit (-16.0, 16.0, (v / bpm - 1.0) * 100.0)); }
    void mouseMove (const juce::MouseEvent& e) override { setMouseCursor (bpmRect().contains (e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor); }
    void mouseDown (const juce::MouseEvent& e) override { draggingBpm = bpmRect().contains (e.getPosition()) && bpm > 0; dragFrom = liveBpm(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! draggingBpm) return;
        const double step = e.mods.isShiftDown() ? 0.01 : 0.1;
        setLiveBpm (std::round ((dragFrom - e.getDistanceFromDragStartY() * step) / step) * step);
        repaint (bpmRect());
    }
    void mouseUp (const juce::MouseEvent&) override { draggingBpm = false; }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (bpmRect().contains (e.getPosition()) && onSync) onSync(); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! bpmRect().contains (e.getPosition()) || bpm <= 0 || w.deltaY == 0.0f) return;
        const double step = e.mods.isShiftDown() ? 0.01 : 0.1;
        setLiveBpm (std::round (liveBpm() / step) * step + (w.deltaY > 0 ? step : -step));
        repaint (bpmRect());
    }
    bool draggingBpm = false; double dragFrom = 0.0;
    void matchTo (const Deck& o) { if (bpm > 0 && o.bpm > 0) pitch.setValue (juce::jlimit (-16.0, 16.0, (o.liveBpm() / bpm - 1.0) * 100.0)); }
    static juce::String clock (double s) { s = juce::jmax (0.0, s); return juce::String::formatted ("%02d:%04.1f", (int) s / 60, std::fmod (s, 60.0)); }
    juce::Rectangle<int> platter() const
    {
        auto r = getLocalBounds().reduced (8); r.removeFromTop (40); r.removeFromBottom (78);
        r.removeFromLeft (mirror ? 0 : 30); r.removeFromRight (mirror ? 30 : 0);
        const int d = juce::jmin (r.getWidth(), r.getHeight());
        return r.withSizeKeepingCentre (d, d);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        g.setColour (col::accent.withAlpha (0.6f)); g.drawRect (getLocalBounds(), 1);
        g.setColour (col::accent); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        fitText (g, title, 8, 4, 60, 16, juce::Justification::centredLeft);
        g.setColour (col::text); g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        fitText (g, name.isEmpty() ? juce::String ("Load a track") : name, 8, 20, getWidth() - 90, 18, juce::Justification::centredLeft, true);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        fitText (g, juce::String (liveBpm(), 1), getWidth() - 86, 4, 78, 22, juce::Justification::centredRight);
        g.setColour (col::dim); g.setFont (juce::FontOptions (10.0f)); fitText (g, "BPM", getWidth() - 86, 24, 78, 12, juce::Justification::centredRight);
        g.setColour (juce::Colour (0xff6ad1ff)); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        fitText (g, key, getWidth() - 160, 4, 70, 18, juce::Justification::centredRight);
        // platter: spinning marker, pitch, elapsed / remaining
        const auto pr = platter().toFloat().reduced (4.0f);
        const double len = tp.getLengthInSeconds(), pos = tp.getCurrentPosition();
        g.setColour (col::bg); g.fillEllipse (pr);
        g.setColour (col::dim.withAlpha (0.5f)); g.drawEllipse (pr, 2.0f);
        if (len > 0)
        {
            juce::Path arc; arc.addCentredArc (pr.getCentreX(), pr.getCentreY(), pr.getWidth() * 0.5f - 3, pr.getHeight() * 0.5f - 3, 0.0f, 0.0f,
                                               juce::MathConstants<float>::twoPi * (float) (pos / len), true);
            g.setColour (col::accent); g.strokePath (arc, juce::PathStrokeType (4.0f));
            const float ang = (float) std::fmod (pos / 1.8, 1.0) * juce::MathConstants<float>::twoPi;   // 33 1/3 rpm
            const auto c = pr.getCentre();
            g.setColour (col::text);
            g.drawLine (c.x, c.y, c.x + std::sin (ang) * pr.getWidth() * 0.45f, c.y - std::cos (ang) * pr.getHeight() * 0.45f, 2.0f);
        }
        g.setColour (col::text); g.setFont (juce::FontOptions (juce::jmax (12.0f, pr.getHeight() * 0.13f), juce::Font::bold));
        const auto ir = pr.reduced (pr.getWidth() * 0.18f).toNearestInt();
        fitText (g, juce::String (pitch.getValue() >= 0 ? "+" : "") + juce::String (pitch.getValue(), 1) + "%", ir.withHeight (ir.getHeight() / 3), juce::Justification::centred);
        fitText (g, clock (pos), ir.withTrimmedTop (ir.getHeight() / 3).withHeight (ir.getHeight() / 3), juce::Justification::centred);
        g.setColour (col::dim); fitText (g, "-" + clock (len - pos), ir.withTrimmedTop (2 * ir.getHeight() / 3), juce::Justification::centred);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bottom = r.removeFromBottom (74);
        auto padRow = bottom.removeFromTop (34);
        const int pw = padRow.getWidth() / 4;
        for (auto* p : pads) p->setBounds (padRow.removeFromLeft (pw).reduced (2));
        const int w = bottom.getWidth() / 7;
        load.setBounds (bottom.removeFromLeft (w).reduced (2)); cue.setBounds (bottom.removeFromLeft (w).reduced (2));
        play.setBounds (bottom.removeFromLeft (w).reduced (2)); sync.setBounds (bottom.removeFromLeft (w).reduced (2));
        loop4.setBounds (bottom.removeFromLeft (w).reduced (2)); loop8.setBounds (bottom.removeFromLeft (w).reduced (2)); push.setBounds (bottom.reduced (2));
        r.removeFromTop (40);
        pitch.setBounds (mirror ? r.removeFromRight (28) : r.removeFromLeft (28));
    }
    // beat loop from the grid beat at the press; checked every 1/60 s
    void setLoop (juce::TextButton& on, juce::TextButton& other, int beats)
    {
        other.setToggleState (false, juce::dontSendNotification);
        if (! on.getToggleState() || bpm <= 0.0) { on.setToggleState (false, juce::dontSendNotification); loopLen = 0.0; return; }
        const double beat = 60.0 / bpm, pos = tp.getCurrentPosition();
        loopStart = gridStart + std::floor ((pos - gridStart) / beat) * beat;
        loopLen = beats * beat;
    }
    // live stutter: loops the last 1/4 or 1/8 beat while a hotkey is held
    void stutter (double beats)
    {
        if (beats <= 0.0 || bpm <= 0.0) { if (stuttering) { loopLen = 0.0; stuttering = false; } return; }
        if (stuttering) return;
        loop4.setToggleState (false, juce::dontSendNotification); loop8.setToggleState (false, juce::dontSendNotification);
        loopStart = tp.getCurrentPosition(); loopLen = beats * 60.0 / bpm; stuttering = true;
    }
    bool stuttering = false;
    void timerCallback() override
    {
        play.setButtonText (tp.isPlaying() ? "PAUSE" : "PLAY");
        if (loopLen > 0.0 && tp.getCurrentPosition() >= loopStart + loopLen)
            tp.setPosition (loopStart + std::fmod (tp.getCurrentPosition() - loopStart, loopLen));
        if (isShowing()) repaint (platter());
    }

    juce::AudioFormatManager& fm; juce::TimeSliceThread& io;
    juce::String title, name;
    bool mirror = false;
    juce::AudioThumbnail thumb;
    juce::AudioTransportSource tp;
    juce::ResamplingAudioSource rs { &tp, false, 2 };
    std::unique_ptr<juce::AudioFormatReaderSource> src;
    std::unique_ptr<juce::FileChooser> chooser;
    double bpm = 0.0, cuePos = 0.0, gridStart = 0.0;
    juce::String key;
    std::array<double, 4> cues { -1.0, -1.0, -1.0, -1.0 };
    std::function<void()> onSync, onPush, onBpm;
    juce::File file;
    juce::TextButton push { "PUSH" };
    juce::TextButton load { "LOAD" }, play { "PLAY" }, cue { "CUE" }, sync { "SYNC" }, loop4 { "LOOP 4" }, loop8 { "LOOP 8" };
    double loopStart = 0.0, loopLen = 0.0;
    juce::OwnedArray<juce::TextButton> pads;
    juce::Slider pitch;
};

// Serato-style centre: both tracks' zoomed waveforms stacked (playhead in the middle) + full-song overviews
struct Waves : public juce::Component, private juce::Timer
{
    Waves (Deck& x, Deck& y) : a (x), b (y) { startTimerHz (phoneUI ? 30 : 60); }
    void timerCallback() override { if (isShowing()) repaint(); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        auto r = getLocalBounds();
        auto over = r.removeFromBottom (36);
        const int h = r.getHeight() / 2;
        zoomed (g, a, r.removeFromTop (h), juce::Colour (0xff3da8ff));
        zoomed (g, b, r, juce::Colour (0xffff7a3d));
        overview (g, a, over.removeFromTop (18), juce::Colour (0xff3da8ff));
        overview (g, b, over, juce::Colour (0xffff7a3d));
        g.setColour (juce::Colours::white); g.drawVerticalLine (getWidth() / 2, 0.0f, (float) getHeight() - 36);
    }
    static void zoomed (juce::Graphics& g, Deck& d, juce::Rectangle<int> r, juce::Colour c)
    {
        if (d.thumb.getTotalLength() <= 0) return;
        const double pos = d.tp.getCurrentPosition(), span = 4.0;   // seconds each side
        g.setColour (c);
        d.thumb.drawChannels (g, r.reduced (0, 2), pos - span, pos + span, 1.0f);
        if (d.bpm > 0)
        {   // beat grid from the song start (rough, BPM-based)
            const double beat = 60.0 / d.bpm;
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            for (double t = d.gridStart + std::ceil ((pos - span - d.gridStart) / beat) * beat; t < pos + span; t += beat)
                g.drawVerticalLine (r.getX() + (int) ((t - pos + span) / (2 * span) * r.getWidth()), (float) r.getY(), (float) r.getBottom());
        }
        for (size_t i = 0; i < d.cues.size(); ++i)
            if (const double q = d.cues[i]; q >= pos - span && q <= pos + span)
            { g.setColour (Deck::padColour ((int) i)); g.fillRect (r.getX() + (int) ((q - pos + span) / (2 * span) * r.getWidth()) - 1, r.getY(), 3, r.getHeight()); }
    }
    static void overview (juce::Graphics& g, Deck& d, juce::Rectangle<int> r, juce::Colour c)
    {
        g.setColour (juce::Colours::white.withAlpha (0.06f)); g.fillRect (r.reduced (0, 1));
        const double len = d.thumb.getTotalLength();
        if (len <= 0) return;
        g.setColour (c.withAlpha (0.8f)); d.thumb.drawChannels (g, r.reduced (0, 1), 0.0, len, 1.0f);
        g.setColour (juce::Colours::white);
        g.fillRect (r.getX() + (int) (d.tp.getCurrentPosition() / len * r.getWidth()), r.getY(), 2, r.getHeight());
    }
    void mouseDown (const juce::MouseEvent& e) override   // click an overview to jump
    {
        if (e.y < getHeight() - 36) return;
        auto& d = e.y < getHeight() - 18 ? a : b;
        if (d.tp.getLengthInSeconds() > 0) d.tp.setPosition (d.tp.getLengthInSeconds() * e.x / juce::jmax (1, getWidth()));
    }
    Deck& a; Deck& b;
};

// library: songs in a folder, double-click = load on the deck that isn't playing
struct Library : public juce::Component, private juce::ListBoxModel
{
    Library (juce::AudioFormatManager& f, Deck& x, Deck& y) : fm (f), a (x), b (y)
    {
        list.setModel (this); list.setRowHeight (phoneUI ? 34 : 22);
        list.setColour (juce::ListBox::backgroundColourId, col::bg);
        for (auto* c : { &downloads, &folder, &toA, &toB }) addAndMakeVisible (c);
        downloads.onClick = [this] { scan (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads")); };
        addAndMakeVisible (list);
        folder.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser> ("Music folder for Hum Verse", dir);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc) { if (auto d = fc.getResult(); d.isDirectory()) scan (d); }); };
        toA.onClick = [this] { if (auto f = picked(); f.existsAsFile()) a.loadFile (f); };
        toB.onClick = [this] { if (auto f = picked(); f.existsAsFile()) b.loadFile (f); };
        scan (juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    }
    void scan (const juce::File& d)
    {
        dir = d; files.clear();
        for (const auto& e : juce::RangedDirectoryIterator (d, true, fm.getWildcardForAllFormats(), juce::File::findFiles))
            if (files.size() < 2000) files.add (e.getFile());
        files.sort(); list.updateContent(); repaint();
    }
    juce::File picked() const { const int r = list.getSelectedRow(); return r >= 0 && r < files.size() ? files[r] : juce::File(); }
    int getNumRows() override { return files.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (row >= files.size()) return;
        if (sel) g.fillAll (col::accent.withAlpha (0.45f)); else if (row % 2) g.fillAll (juce::Colours::white.withAlpha (0.03f));
        const auto& f = files.getReference (row);
        g.setColour (col::text); g.setFont (juce::FontOptions (phoneUI ? 15.0f : 13.0f));
        fitText (g, juce::String (row + 1), 4, 0, 34, h, juce::Justification::centredRight);
        fitText (g, f.getFileNameWithoutExtension(), 46, 0, w - 200, h, juce::Justification::centredLeft, true);
        g.setColour (col::dim);
        fitText (g, f.getFileExtension().substring (1).toUpperCase() + "  " + juce::File::descriptionOfSizeInBytes (f.getSize()), w - 150, 0, 144, h, juce::Justification::centredRight);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row < files.size()) (a.tp.isPlaying() && ! b.tp.isPlaying() ? b : a).loadFile (files[row]);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        fitText (g, "#    SONG  -  " + dir.getFullPathName() + "  (" + juce::String (files.size()) + ")", 8, 4, getWidth() - 400, 18, juce::Justification::centredLeft, true);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (26).reduced (2);
        toB.setBounds (top.removeFromRight (80)); toA.setBounds (top.removeFromRight (80)); folder.setBounds (top.removeFromRight (90)); downloads.setBounds (top.removeFromRight (100));
        list.setBounds (r);
    }
    juce::AudioFormatManager& fm; Deck& a; Deck& b;
    juce::File dir; juce::Array<juce::File> files;
    juce::ListBox list;
    juce::TextButton downloads { "DOWNLOADS" }, folder { "FOLDER..." }, toA { "LOAD A" }, toB { "LOAD B" };
    std::unique_ptr<juce::FileChooser> chooser;
};

struct DjMix : public juce::AudioSource
{
    DjMix (Deck& x, Deck& y) : a (x), b (y) {}
    void prepareToPlay (int n, double sr) override { a.rs.prepareToPlay (n, sr); b.rs.prepareToPlay (n, sr); tmp.setSize (2, n); }
    void releaseResources() override { a.rs.releaseResources(); b.rs.releaseResources(); }
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        if (tmp.getNumSamples() < info.numSamples) return;   // never allocate here
        const float x = fade.load(), m = master.load();
        const int cv = curve.load();   // 0 smooth (constant power), 1 linear, 2 cut (scratch: both full until the edges)
        const float ga = (cv == 1 ? 1.0f - x : cv == 2 ? juce::jlimit (0.0f, 1.0f, (1.0f - x) * 8.0f) : std::cos (x * juce::MathConstants<float>::halfPi)) * m;
        const float gb = (cv == 1 ? x : cv == 2 ? juce::jlimit (0.0f, 1.0f, x * 8.0f) : std::sin (x * juce::MathConstants<float>::halfPi)) * m;
        for (auto [d, gain] : { std::pair<Deck*, float> { &a, ga }, { &b, gb } })
        {
            juce::AudioSourceChannelInfo t (&tmp, 0, info.numSamples);
            d->rs.getNextAudioBlock (t);
            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                info.buffer->addFrom (c, info.startSample, tmp, juce::jmin (c, 1), 0, info.numSamples, gain);
        }
        // REC: the mix goes to a background disk writer (lock-free FIFO); never waits if the UI is swapping writers
        const juce::SpinLock::ScopedTryLockType lock (recLock);
        if (lock.isLocked() && rec != nullptr && info.buffer->getNumChannels() > 0)
        {
            const float* ch[2] { info.buffer->getReadPointer (0, info.startSample), info.buffer->getReadPointer (juce::jmin (1, info.buffer->getNumChannels() - 1), info.startSample) };
            rec->write (ch, info.numSamples);
        }
    }
    juce::SpinLock recLock;
    juce::AudioFormatWriter::ThreadedWriter* rec = nullptr;
    std::atomic<int> curve { 0 };
    Deck& a; Deck& b; juce::AudioBuffer<float> tmp;
    std::atomic<float> fade { 0.5f }, master { 0.8f };
};

// TRANSITIONS: pre-rendered A->B presets on a worker thread (own readers, own buffers; never touches the audio or UI thread)
struct TransitionPreset { const char* name; double bars; int kind; };   // kind: 0 blend, 1 cut, 2 bass swap, 3 filter sweep, 4 echo out
static constexpr TransitionPreset kTransitions[] {
    { "Blend 8 bars", 8, 0 }, { "Blend 16 bars", 16, 0 }, { "Bass Swap 8 bars", 8, 2 },
    { "Filter Sweep 8 bars", 8, 3 }, { "Echo Out 4 bars", 4, 4 }, { "Cut on the 1", 2, 1 } };
struct TransitionJob { juce::File a, b, out; double aPos = 0, aRate = 1, bPos = 0, bRate = 1, bpm = 120; int preset = 0; };

static bool readStretched (juce::AudioFormatReader& r, double startSec, double rate, double outSr, juce::AudioBuffer<float>& dst)
{
    const int n = dst.getNumSamples();
    const double step = rate * r.sampleRate / outSr;   // input samples per output sample
    const int need = (int) std::ceil (n * step) + 16;
    juce::AudioBuffer<float> in (2, need);
    in.clear();
    r.read (&in, 0, need, (juce::int64) (juce::jmax (0.0, startSec) * r.sampleRate), true, true);
    for (int ch = 0; ch < 2; ++ch)
    {
        juce::LagrangeInterpolator li;
        li.process (step, in.getReadPointer (juce::jmin (ch, (int) r.numChannels - 1)), dst.getWritePointer (ch), n, need, 0);
    }
    return true;
}

static bool renderTransition (const TransitionJob& j)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> ra (fm.createReaderFor (j.a)), rb (fm.createReaderFor (j.b));
    if (ra == nullptr || rb == nullptr || ra->sampleRate <= 0 || rb->sampleRate <= 0) return false;
    const auto& pr = kTransitions[j.preset];
    const double sr = ra->sampleRate, beat = 60.0 / juce::jmax (40.0, j.bpm);
    const double pre = 8 * beat, len = pr.bars * 4 * beat, post = 8 * beat;
    const int nPre = (int) (pre * sr), nLen = (int) (len * sr), nTot = (int) ((pre + len + post) * sr);
    juce::AudioBuffer<float> A (2, nPre + nLen), B (2, nTot - nPre), out (2, nTot);
    readStretched (*ra, j.aPos, j.aRate, sr, A);
    readStretched (*rb, j.bPos, j.bRate, sr, B);
    out.clear();
    juce::IIRFilter hpA[2], hpB[2];
    juce::AudioBuffer<float> echo (2, (int) (beat * sr * 0.75) + 1); echo.clear(); int ep = 0;
    const int blk = 256;
    for (int s0 = 0; s0 < nTot; s0 += blk)
    {
        const int n = juce::jmin (blk, nTot - s0);
        const float t = (float) juce::jlimit (0.0, 1.0, (s0 - nPre) / (double) juce::jmax (1, nLen));   // 0..1 across the transition
        float ga = 1, gb = 0, hpa = 0, hpb = 0; bool echoOnly = false;
        switch (pr.kind)
        {
            case 0: ga = std::cos (t * juce::MathConstants<float>::halfPi); gb = std::sin (t * juce::MathConstants<float>::halfPi); break;
            case 1: ga = t < 0.5f ? 1.0f : 0.0f; gb = 1.0f - ga; break;
            case 2: gb = juce::jmin (1.0f, t * 2.0f); ga = t < 0.5f ? 1.0f : 1.0f - (t - 0.5f) * 2.0f; hpa = t >= 0.5f ? 220.0f : 0.0f; hpb = t < 0.5f ? 220.0f : 0.0f; break;
            case 3: hpa = 20.0f * std::pow (150.0f, t); ga = t < 0.75f ? 1.0f : 1.0f - (t - 0.75f) * 4.0f; gb = juce::jlimit (0.0f, 1.0f, (t - 0.5f) * 2.0f); break;
            case 4: echoOnly = t >= 0.25f; ga = echoOnly ? 0.0f : 1.0f; gb = t >= 0.5f ? 1.0f : 0.0f; break;
        }
        if (s0 >= nPre + nLen) { ga = 0; gb = 1; hpb = 0; }
        for (int ch = 0; ch < 2; ++ch)
        {
            if (hpa > 0) hpA[ch].setCoefficients (juce::IIRCoefficients::makeHighPass (sr, hpa)); else hpA[ch].reset();
            if (hpb > 0) hpB[ch].setCoefficients (juce::IIRCoefficients::makeHighPass (sr, hpb)); else hpB[ch].reset();
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            float* o = out.getWritePointer (ch, s0);
            if (s0 < A.getNumSamples())
            {
                const int na = juce::jmin (n, A.getNumSamples() - s0);
                juce::HeapBlock<float> tmp (na); std::memcpy (tmp, A.getReadPointer (ch, s0), sizeof (float) * (size_t) na);
                if (hpa > 0) hpA[ch].processSamples (tmp, na);
                for (int i = 0; i < na; ++i) o[i] += tmp[i] * ga;
                if (pr.kind == 4)   // echo send taken from A while it still plays, then left to ring out
                    for (int i = 0; i < na; ++i) { auto* e = echo.getWritePointer (ch); const int k = (ep + i) % echo.getNumSamples(); const float d = e[k]; e[k] = tmp[i] * (echoOnly ? 0.0f : 0.5f) + d * 0.55f; o[i] += d * 0.8f; }
            }
            else if (pr.kind == 4)
                for (int i = 0; i < n; ++i) { auto* e = echo.getWritePointer (ch); const int k = (ep + i) % echo.getNumSamples(); const float d = e[k]; e[k] = d * 0.55f; o[i] += d * 0.8f; }
            if (s0 >= nPre)
            {
                const int bo = s0 - nPre, nb = juce::jmin (n, B.getNumSamples() - bo);
                juce::HeapBlock<float> tmp (juce::jmax (1, nb)); std::memcpy (tmp, B.getReadPointer (ch, bo), sizeof (float) * (size_t) juce::jmax (0, nb));
                if (hpb > 0) hpB[ch].processSamples (tmp, nb);
                for (int i = 0; i < nb; ++i) o[i] += tmp[i] * gb;
            }
        }
        ep = (ep + n) % echo.getNumSamples();
    }
    out.applyGainRamp (0, juce::jmin (256, nTot), 0.0f, 1.0f);
    out.applyGainRamp (nTot - juce::jmin (2048, nTot), juce::jmin (2048, nTot), 1.0f, 0.0f);
    for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::clip (out.getWritePointer (ch), out.getReadPointer (ch), -1.0f, 1.0f, nTot);
    j.out.deleteFile();
    std::unique_ptr<juce::OutputStream> os (j.out.createOutputStream());
    if (os == nullptr) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (w == nullptr) return false;
    os.release();
    return w->writeFromAudioSampleBuffer (out, 0, nTot);
}

class DjView : public juce::Component, private juce::Timer, private juce::MidiInputCallback
{
public:
    explicit DjView (Studio& st) : s (st)
    {
        fm.registerBasicFormats();
        io.startThread (juce::Thread::Priority::normal);
        addAndMakeVisible (a); addAndMakeVisible (b); addAndMakeVisible (waves); addAndMakeVisible (library);
        a.onSync = [this] { a.matchTo (b); }; b.onSync = [this] { b.matchTo (a); };
        // LINK: the decks keep their own BPM until linked; then moving either one moves the other
        linkBtn.setClickingTogglesState (true);
        linkBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2fb36b));
        linkBtn.setTooltip ("LINK the two decks' BPM: drag either BPM number and the other follows (off = separate)");
        linkBtn.onClick = [this] { if (linkBtn.getToggleState()) b.matchTo (a); };
        addAndMakeVisible (linkBtn);
        const auto follow = [this] (Deck& from, Deck& to) {
            if (! linkBtn.getToggleState() || linking) return;
            const juce::ScopedValueSetter<bool> guard (linking, true);
            to.matchTo (from); to.repaint();
        };
        a.onBpm = [this, follow] { follow (a, b); }; b.onBpm = [this, follow] { follow (b, a); };
        a.onPush = [this] { pushOrMenu (a.file); }; b.onPush = [this] { pushOrMenu (b.file); };
        xf.setSliderStyle (juce::Slider::LinearHorizontal); xf.setRange (0.0, 1.0); xf.setValue (0.5);
        xf.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0); xf.setDoubleClickReturnValue (true, 0.5);
        xf.onValueChange = [this] { mix.fade = (float) xf.getValue(); };
        vol.setSliderStyle (juce::Slider::LinearHorizontal); vol.setRange (0.0, 1.0); vol.setValue (0.8);
        vol.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        vol.onValueChange = [this] { mix.master = (float) vol.getValue(); };
        for (auto* c : { (juce::Component*) &xf, (juce::Component*) &vol, (juce::Component*) &autoMix, (juce::Component*) &fadeBtn }) addAndMakeVisible (c);
        autoMix.setClickingTogglesState (true);
        autoMix.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2fb36b));
        fadeBtn.onClick = [this] { startFade(); };
        for (auto* c : { &recBtn, &curveBtn, &transBtn }) addAndMakeVisible (c);
        transBtn.setTooltip ("Transition presets between deck 1 and deck 2: rendered in the background, then audition, PUSH or add to the rack");
        transBtn.onClick = [this] { transMenu(); };
        recBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd23c3c));
        recBtn.setTooltip ("Record the Hum Verse mix; when you stop it's saved to Downloads and you can PUSH it to the Record timeline");
        recBtn.onClick = [this] { toggleRec(); };
        curveBtn.setTooltip ("Crossfade curve and FADE length (manual or automatic)");
        curveBtn.onClick = [this] { curveMenu(); };
        setWantsKeyboardFocus (true);
        addAndMakeVisible (learn);
        learn.setTooltip ("CDJ / controller in MIDI mode (enable it in Options > MIDI): pick a control, then move it on the hardware");
        learn.onClick = [this] {
            DjView* self = this;   // MSVC can't name `this` in a nested lambda's init-capture
            juce::PopupMenu m;
            for (int i = 0; i < (int) std::size (kTargets); ++i) m.addItem (i + 1, kTargets[i]);
            m.addSeparator(); m.addItem (100, "Clear all mappings");
            m.showMenuAsync ({}, [safe = juce::Component::SafePointer<DjView> (self)] (int r) {
                if (safe == nullptr || r <= 0) return;
                if (r == 100) { safe->maps.clear(); safe->saveMaps(); safe->learn.setButtonText ("LEARN"); return; }
                safe->armed = r - 1; safe->learn.setButtonText ("MOVE IT...");
            });
        };
        loadMaps();
        s.engine.deviceManager.addMidiInputDeviceCallback ({}, this);
        player.setSource (&mix);
        startTimerHz (30);
        s.engine.deviceManager.addAudioCallback (&player);
        setSize (phoneUI ? 380 : 1200, phoneUI ? 760 : 760);
    }
    ~DjView() override
    {
        stopRec (false);
        s.engine.deviceManager.removeMidiInputDeviceCallback ({}, this);
        s.engine.deviceManager.removeAudioCallback (&player);
        player.setSource (nullptr);
        io.stopThread (2000);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        fitText (g, "A   <   CROSSFADER   >   B", xf.getBounds().translated (0, -16).withHeight (14), juce::Justification::centred);
        fitText (g, "DJ OUT", vol.getBounds().translated (0, -16).withHeight (14), juce::Justification::centred);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        if (phoneUI)
        {
            a.setBounds (r.removeFromTop (230)); waves.setBounds (r.removeFromTop (120).reduced (0, 4)); b.setBounds (r.removeFromTop (230));
            auto mix = r.removeFromTop (44); mix.removeFromTop (16);
            vol.setBounds (mix.removeFromRight (64)); mix.removeFromRight (4);
            autoMix.setBounds (mix.removeFromLeft (56)); transBtn.setBounds (mix.removeFromLeft (48)); learn.setBounds (mix.removeFromRight (56)); fadeBtn.setBounds (mix.removeFromRight (48));
            recBtn.setBounds (mix.removeFromLeft (44)); curveBtn.setBounds (mix.removeFromRight (56)); linkBtn.setBounds (mix.removeFromLeft (44)); xf.setBounds (mix.reduced (6, 0));
            library.setBounds (r);
            return;
        }
        auto top = r.removeFromTop (juce::jmax (300, r.getHeight() * 5 / 10));
        const int dw = juce::jmin (330, top.getWidth() / 4);
        a.setBounds (top.removeFromLeft (dw)); b.setBounds (top.removeFromRight (dw));
        waves.setBounds (top.reduced (6, 0));
        auto mix = r.removeFromTop (44); mix.removeFromTop (16);
        vol.setBounds (mix.removeFromRight (180)); mix.removeFromRight (16);
        learn.setBounds (mix.removeFromRight (100)); mix.removeFromRight (6);
        autoMix.setBounds (mix.removeFromLeft (90)); mix.removeFromLeft (6); fadeBtn.setBounds (mix.removeFromLeft (64));
        mix.removeFromLeft (6); curveBtn.setBounds (mix.removeFromLeft (70)); mix.removeFromLeft (6); recBtn.setBounds (mix.removeFromLeft (64));
        mix.removeFromLeft (6); transBtn.setBounds (mix.removeFromLeft (64));
        mix.removeFromLeft (6); linkBtn.setBounds (mix.removeFromLeft (60));
        xf.setBounds (mix.withSizeKeepingCentre (juce::jmin (420, mix.getWidth()), mix.getHeight()));
        library.setBounds (r.withTrimmedTop (4));
    }
private:
    // FADE: 8 s crossfade to the other deck (synced + started); AUTO MIX fires it 16 beats before the playing deck ends
    void startFade()
    {
        const bool toB = xf.getValue() < 0.5;
        Deck& from = toB ? a : b; Deck& to = toB ? b : a;
        if (to.tp.getLengthInSeconds() <= 0.0) return;
        to.matchTo (from);
        if (! to.tp.isPlaying()) to.tp.start();
        fadeFrom = xf.getValue(); fadeTo = toB ? 1.0 : 0.0; fadeT0 = juce::Time::getMillisecondCounterHiRes(); fading = true;
    }
    void timerCallback() override
    {
        if (fading)
        {
            const double k = juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - fadeT0) / fadeMs);
            xf.setValue (fadeFrom + (fadeTo - fadeFrom) * k);
            if (k >= 1.0)
            {
                fading = false;
                Deck& old = fadeTo > 0.5 ? a : b;
                if (autoMix.getToggleState()) old.tp.stop();
            }
            return;
        }
        if (! autoMix.getToggleState()) return;
        Deck& cur = xf.getValue() < 0.5 ? a : b;
        const double len = cur.tp.getLengthInSeconds();
        const double lead = cur.liveBpm() > 0.0 ? 16.0 * 60.0 / cur.liveBpm() : 20.0;
        if (cur.tp.isPlaying() && len > lead && cur.tp.getCurrentPosition() >= len - lead) startFade();
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)   // space: play / stop the deck the crossfader is on (both stop if both play)
        {
            if (a.tp.isPlaying() && b.tp.isPlaying()) { a.tp.stop(); b.tp.stop(); return true; }
            Deck& d = xf.getValue() < 0.5 ? a : b;
            if (d.tp.isPlaying()) d.tp.stop(); else d.tp.start();
            return true;
        }
        // keyboard-as-controller: deck 1 = 1-4 cues, Q play, W cue, E sync, R/T loop 4/8; deck 2 = 7-0 cues, U play, I cue, O sync, P/[ loop 4/8
        // hold A/S (deck 1) or J/K (deck 2) to stutter 1/4 / 1/8 beat; Z/X nudge the crossfader, C centers it
        const auto ch = (juce::juce_wchar) juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
        const juce::String d1 ("1234"), d2 ("7890");
        if (d1.containsChar (ch)) { a.hotCue (d1.indexOfChar (ch)); return true; }
        if (d2.containsChar (ch)) { b.hotCue (d2.indexOfChar (ch)); return true; }
        switch (ch)
        {
            case 'q': a.play.triggerClick(); return true;  case 'w': a.cue.triggerClick(); return true;  case 'e': a.sync.triggerClick(); return true;
            case 'r': a.loop4.triggerClick(); return true; case 't': a.loop8.triggerClick(); return true;
            case 'u': b.play.triggerClick(); return true;  case 'i': b.cue.triggerClick(); return true;  case 'o': b.sync.triggerClick(); return true;
            case 'p': b.loop4.triggerClick(); return true; case '[': b.loop8.triggerClick(); return true;
            case 'a': a.stutter (0.25); return true; case 's': a.stutter (0.125); return true;
            case 'j': b.stutter (0.25); return true; case 'k': b.stutter (0.125); return true;
            case 'z': fading = false; xf.setValue (juce::jmax (0.0, xf.getValue() - 0.1)); return true;
            case 'x': fading = false; xf.setValue (juce::jmin (1.0, xf.getValue() + 0.1)); return true;
            case 'c': fading = false; xf.setValue (0.5); return true;
            default: break;
        }
        return false;
    }
    bool keyStateChanged (bool) override
    {   // stutter keys released -> back to normal playback
        auto down = [] (char c) { return juce::KeyPress::isKeyCurrentlyDown (c) || juce::KeyPress::isKeyCurrentlyDown (juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) c)); };
        if (! down ('a') && ! down ('s')) a.stutter (0.0);
        if (! down ('j') && ! down ('k')) b.stutter (0.0);
        return false;
    }
    void curveMenu()
    {
        juce::PopupMenu m, len;
        const char* names[] { "Smooth (constant power)", "Linear", "Cut (scratch)" };
        for (int i = 0; i < 3; ++i) m.addItem (1 + i, names[i], true, mix.curve.load() == i);
        for (double sec : { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 }) len.addItem (10 + (int) sec, juce::String ((int) sec) + " s", true, std::abs (fadeMs - sec * 1000.0) < 1.0);
        m.addSubMenu ("FADE / AUTO MIX length", len);
        m.addItem (5, "FADE now"); m.addItem (6, "Center crossfader");
        m.showMenuAsync ({}, [safe = juce::Component::SafePointer<DjView> (this)] (int r) {
            if (safe == nullptr || r <= 0) return;
            if (r <= 3) safe->mix.curve = r - 1;
            else if (r == 5) safe->startFade();
            else if (r == 6) { safe->fading = false; safe->xf.setValue (0.5); }
            else if (r >= 10) safe->fadeMs = (r - 10) * 1000.0;
        });
    }
    void toggleRec() { if (recWriter != nullptr) stopRec (true); else startRec(); }
    void startRec()
    {
        auto* dev = s.engine.deviceManager.getCurrentAudioDevice();
        const double sr = dev != nullptr ? dev->getCurrentSampleRate() : 44100.0;
        auto dl = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
        if (! dl.isDirectory() && ! dl.createDirectory()) dl = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        recFile = dl.getNonexistentChildFile ("Hum Verse Mix " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M"), ".wav");
        auto out = recFile.createOutputStream();
        if (out == nullptr) return;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (out.release());
        auto* w = wav.createWriterFor (os.get(), sr, 2, 24, {}, 0);
        if (w == nullptr) return;
        os.release();
        recWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w, io, 65536);
        { const juce::SpinLock::ScopedLockType l (mix.recLock); mix.rec = recWriter.get(); }
        recBtn.setToggleState (true, juce::dontSendNotification); recBtn.setButtonText ("STOP");
    }
    void stopRec (bool ask)
    {
        if (recWriter == nullptr) return;
        { const juce::SpinLock::ScopedLockType l (mix.recLock); mix.rec = nullptr; }
        recWriter = nullptr;   // flushes the FIFO and closes the WAV
        recBtn.setToggleState (false, juce::dontSendNotification); recBtn.setButtonText ("REC");
        if (! ask) return;
        juce::PopupMenu m;
        m.addSectionHeader ("Saved: Downloads/" + recFile.getFileName());
        m.addItem (1, "PUSH to Record timeline (end of song)"); m.addItem (2, "PUSH to Record timeline at playhead");
        m.addItem (3, "Add to Beat Channel Rack"); m.addItem (4, "Show in folder");
        m.showMenuAsync ({}, [safe = juce::Component::SafePointer<DjView> (this), f = recFile] (int r) {
            if (safe == nullptr || r <= 0) return;
            if (r <= 2) safe->pushToRecord (f, r == 2);
            else if (r == 3) addSampleChannels (safe->s, { f }, {});
            else f.revealToUser();
        });
    }
    // TRANS: A (from its playhead) into B (from its cue), at deck 1's tempo; each preset renders once per pair
    void transMenu()
    {
        juce::PopupMenu m;
        if (! a.file.existsAsFile() || ! b.file.existsAsFile()) { m.addItem (-1, "Load a song on both decks first", false); m.showMenuAsync ({}); return; }
        m.addItem (1000, "Render all presets (background)");
        m.addSeparator();
        for (int i = 0; i < (int) std::size (kTransitions); ++i)
        {
            const auto f = transFile (i);
            juce::PopupMenu sub;
            const bool ready = f.existsAsFile() && rendering.count (i) == 0;
            sub.addItem (i * 10 + 1, ready ? "Audition" : "Render + audition");
            sub.addItem (i * 10 + 2, "PUSH to Record timeline at playhead", ready);
            sub.addItem (i * 10 + 3, "PUSH to Record timeline (end of song)", ready);
            sub.addItem (i * 10 + 4, "Add to Beat Channel Rack", ready);
            sub.addItem (i * 10 + 5, "Show in folder", ready);
            m.addSubMenu (juce::String (kTransitions[i].name) + (rendering.count (i) ? "  (rendering...)" : ready ? "  (ready)" : ""), sub);
        }
        m.addSeparator(); m.addItem (2000, "Stop audition");
        m.showMenuAsync ({}, [safe = juce::Component::SafePointer<DjView> (this)] (int r) {
            if (safe == nullptr || r <= 0) return;
            if (r == 2000) { safe->s.engine.stopPreview(); return; }
            if (r == 1000) { for (int i = 0; i < (int) std::size (kTransitions); ++i) safe->renderTrans (i, false); return; }
            const int i = r / 10, act = r % 10;
            const auto f = safe->transFile (i);
            if (act == 1) { if (f.existsAsFile() && safe->rendering.count (i) == 0) safe->s.engine.previewFile (f); else safe->renderTrans (i, true); }
            else if (act == 2 || act == 3) safe->pushToRecord (f, act == 2);
            else if (act == 4) addSampleChannels (safe->s, { f }, {});
            else if (act == 5) f.revealToUser();
        });
    }
    juce::File transFile (int i) const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads").getChildFile ("Hum Verse Transitions");
        const auto key = juce::String::toHexString ((a.file.getFullPathName() + b.file.getFullPathName() + juce::String (a.tp.getCurrentPosition(), 0)
                                                     + juce::String (b.cuePos, 1) + juce::String (a.liveBpm(), 1)).hashCode64());
        return dir.getChildFile (a.file.getFileNameWithoutExtension().substring (0, 24) + " to " + b.file.getFileNameWithoutExtension().substring (0, 24)
                                 + " - " + kTransitions[i].name + " " + key.substring (0, 6) + ".wav");
    }
    void renderTrans (int i, bool audition)
    {
        const auto f = transFile (i);
        if (rendering.count (i) || (f.existsAsFile() && ! audition)) return;
        if (f.existsAsFile()) { s.engine.previewFile (f); return; }
        f.getParentDirectory().createDirectory();
        TransitionJob j; j.a = a.file; j.b = b.file; j.out = f; j.preset = i;
        j.aPos = a.tp.getCurrentPosition(); j.bPos = b.cuePos;
        j.bpm = a.liveBpm() > 0 ? a.liveBpm() : 120.0;
        j.aRate = a.bpm > 0 ? j.bpm / a.bpm : 1.0;
        j.bRate = b.bpm > 0 ? j.bpm / b.bpm : 1.0;
        rendering.insert (i);
        transBtn.setButtonText ("TRANS...");
        juce::Thread::launch ([j, i, audition, safe = juce::Component::SafePointer<DjView> (this)] {
            const bool ok = renderTransition (j);
            juce::MessageManager::callAsync ([safe, i, ok, audition, f = j.out] {
                if (safe == nullptr) return;
                safe->rendering.erase (i);
                if (safe->rendering.empty()) safe->transBtn.setButtonText ("TRANS");
                if (ok && audition) safe->s.engine.previewFile (f);
            });
        });
    }
    std::set<int> rendering;
    juce::TextButton transBtn { "TRANS" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recWriter;
    juce::File recFile;
    juce::TextButton recBtn { "REC" }, curveBtn { "CURVE" };
    double fadeMs = 8000.0;
    // MIDI LEARN: CDJ / controller messages (note or CC per channel; pitch bend for pitch) -> deck controls
    static constexpr const char* kTargets[] { "Deck 1 Play", "Deck 1 Cue", "Deck 1 Sync", "Deck 1 Pitch", "Deck 2 Play", "Deck 2 Cue", "Deck 2 Sync", "Deck 2 Pitch", "Crossfader", "DJ Out volume" };
    static int midiKey (const juce::MidiMessage& m)
    {
        if (m.isNoteOnOrOff()) return 0x10000 | (m.getChannel() << 8) | m.getNoteNumber();
        if (m.isController()) return 0x20000 | (m.getChannel() << 8) | m.getControllerNumber();
        if (m.isPitchWheel()) return 0x30000 | (m.getChannel() << 8);
        return 0;
    }
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        const int k = midiKey (m);
        if (k == 0) return;
        const double v = m.isPitchWheel() ? m.getPitchWheelValue() / 16383.0 : m.isController() ? m.getControllerValue() / 127.0 : (m.getVelocity() > 0 ? 1.0 : 0.0);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DjView> (this), k, v] { if (safe != nullptr) safe->onMidi (k, v); });
    }
    void onMidi (int k, double v)
    {
        if (armed >= 0) { maps[k] = armed; armed = -1; learn.setButtonText ("LEARN"); saveMaps(); return; }
        const auto it = maps.find (k);
        if (it == maps.end()) return;
        const int t = it->second;
        Deck& d = t < 4 ? a : b;
        const bool button = t < 8 && t % 4 != 3, press = v >= 0.5 && last[k] < 0.5;
        last[k] = v;
        if (button) { if (press) (t % 4 == 0 ? d.play : t % 4 == 1 ? d.cue : d.sync).triggerClick(); return; }
        if (t < 8) d.pitch.setValue (-16.0 + 32.0 * v);
        else if (t == 8) { fading = false; xf.setValue (v); }
        else vol.setValue (v);
    }
    void saveMaps()
    {
        if (s.settings == nullptr) return;
        juce::StringArray rows;
        for (const auto& [k, t] : maps) rows.add (juce::String (k) + ":" + juce::String (t));
        s.settings->setValue ("humVerseMidi", rows.joinIntoString (";")); s.settings->saveIfNeeded();
    }
    // PUSH: a deck's track goes to the Record timeline as a normal audio clip (stacked after the song with a
    // 16-beat crossfade), to the Beat Channel Rack as one long one-shot, or both
    // PUSH: lands the song on the Record page as an audio clip at the playhead, like a recorded vocal take;
    // right-click (double-tap on phone) for the Beat rack / end-of-song choices
    void pushOrMenu (const juce::File& f)
    {
        if (juce::ModifierKeys::getCurrentModifiers().isPopupMenu()) { pushMenu (f); return; }
        pushToRecord (f, true);
        if (s.showView) s.showView (1);
    }
    void pushMenu (const juce::File& f)
    {
        juce::PopupMenu m;
        m.addItem (1, "Record timeline (audio clip)"); m.addItem (2, "Beat Channel Rack (long one-shot)"); m.addItem (3, "Both");
        m.addSeparator(); m.addItem (4, "Record timeline at playhead");
        m.showMenuAsync ({}, [safe = juce::Component::SafePointer<DjView> (this), f] (int r) {
            if (safe == nullptr || r <= 0) return;
            if (r != 2) safe->pushToRecord (f, r == 4);
            if (r == 2 || r == 3) addSampleChannels (safe->s, { f }, {});
        });
    }
    void pushToRecord (const juce::File& f, bool atPlayhead)
    {
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
        const double secs = rd && rd->sampleRate > 0 ? (double) rd->lengthInSamples / rd->sampleRate : 0.0;
        if (secs <= 0) return;
        const double play = s.engine.positionBeats();
        s.edit ("Push to song", [&] (Project& p) {
            const double len = secondsToBeats (secs, p.bpm), xfade = juce::jmin (16.0, len * 0.25);
            double end = 0.0;
            for (auto& c : p.clips) end = juce::jmax (end, c.start + c.length);
            Lane l; l.name = f.getFileNameWithoutExtension(); l.mixerTrack = p.addMixerTrack (l.name);
            p.lanes.push_back (l);
            Clip c; c.uid = p.newUid(); c.lane = (int) p.lanes.size() - 1; c.audioFile = f; c.patternIndex = -1; c.length = len;
            c.start = atPlayhead ? play : juce::jmax (0.0, end - (end > 0 ? xfade : 0.0));
            c.fadeIn = c.start > 0 ? xfade : 0.0; c.fadeOut = xfade;
            p.clips.push_back (c);
        });
    }
    void loadMaps()
    {
        if (s.settings == nullptr) return;
        for (const auto& row : juce::StringArray::fromTokens (s.settings->getValue ("humVerseMidi"), ";", ""))
            if (row.contains (":")) maps[row.upToFirstOccurrenceOf (":", false, false).getIntValue()] = juce::jlimit (0, 9, row.fromFirstOccurrenceOf (":", false, false).getIntValue());
    }
    juce::TextButton learn { "LEARN" };
    std::map<int, int> maps;
    std::map<int, double> last;
    int armed = -1;
    juce::TextButton autoMix { "AUTO MIX" }, fadeBtn { "FADE" }, linkBtn { "LINK" };
    bool linking = false;
    bool fading = false;
    double fadeFrom = 0.5, fadeTo = 0.5, fadeT0 = 0.0;
    Studio& s;
    juce::AudioFormatManager fm;
    juce::TimeSliceThread io { "Hum Verse read-ahead" };
    juce::AudioThumbnailCache thumbs { 8 };
    Deck a { fm, io, thumbs, "DECK 1", false }, b { fm, io, thumbs, "DECK 2", true };
    DjMix mix { a, b };
    Waves waves { a, b };
    Library library { fm, a, b };
    juce::AudioSourcePlayer player;
    juce::Slider xf, vol;
};

struct DjWindow : public juce::DocumentWindow
{
    explicit DjWindow (Studio& s) : juce::DocumentWindow ("Hum Verse", col::panel, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (! phoneUI); if (phoneUI) setTitleBarHeight (44);
        setContentOwned (new DjView (s), true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }   // decks keep playing; DJ opens it again
};
std::unique_ptr<DjWindow>& djWindow() { static std::unique_ptr<DjWindow> w; return w; }
}

void showDjDecks (Studio& s)
{
    auto& w = djWindow();
    if (w == nullptr) w = std::make_unique<DjWindow> (s);
    w->setVisible (true); w->toFront (true);
}
void closeDjDecks() { djWindow().reset(); }
}
