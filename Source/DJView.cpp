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
        for (auto* b : { &load, &play, &cue, &sync }) addAndMakeVisible (b);
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
        pitch.onValueChange = [this] { rs.setResamplingRatio (1.0 + pitch.getValue() / 100.0); };
        addAndMakeVisible (pitch);
        startTimerHz (60);
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
        name = f.getFileNameWithoutExtension(); cuePos = 0.0;
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
        g.drawText (title, 8, 4, 60, 16, juce::Justification::centredLeft);
        g.setColour (col::text); g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (name.isEmpty() ? juce::String ("Load a track") : name, 8, 20, getWidth() - 90, 18, juce::Justification::centredLeft, true);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText (juce::String (liveBpm(), 1), getWidth() - 86, 4, 78, 22, juce::Justification::centredRight);
        g.setColour (col::dim); g.setFont (juce::FontOptions (10.0f)); g.drawText ("BPM", getWidth() - 86, 24, 78, 12, juce::Justification::centredRight);
        g.setColour (juce::Colour (0xff6ad1ff)); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (key, getWidth() - 160, 4, 70, 18, juce::Justification::centredRight);
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
        g.drawText (juce::String (pitch.getValue() >= 0 ? "+" : "") + juce::String (pitch.getValue(), 1) + "%", ir.withHeight (ir.getHeight() / 3), juce::Justification::centred);
        g.drawText (clock (pos), ir.withTrimmedTop (ir.getHeight() / 3).withHeight (ir.getHeight() / 3), juce::Justification::centred);
        g.setColour (col::dim); g.drawText ("-" + clock (len - pos), ir.withTrimmedTop (2 * ir.getHeight() / 3), juce::Justification::centred);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bottom = r.removeFromBottom (74);
        auto padRow = bottom.removeFromTop (34);
        const int pw = padRow.getWidth() / 4;
        for (auto* p : pads) p->setBounds (padRow.removeFromLeft (pw).reduced (2));
        const int w = bottom.getWidth() / 6;
        load.setBounds (bottom.removeFromLeft (w).reduced (2)); cue.setBounds (bottom.removeFromLeft (w).reduced (2));
        play.setBounds (bottom.removeFromLeft (w).reduced (2)); sync.setBounds (bottom.removeFromLeft (w).reduced (2));
        loop4.setBounds (bottom.removeFromLeft (w).reduced (2)); loop8.setBounds (bottom.reduced (2));
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
    void timerCallback() override
    {
        play.setButtonText (tp.isPlaying() ? "PAUSE" : "PLAY");
        if (loopLen > 0.0 && tp.getCurrentPosition() >= loopStart + loopLen)
            tp.setPosition (loopStart + std::fmod (tp.getCurrentPosition() - loopStart, loopLen));
        repaint (platter());
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
    std::function<void()> onSync;
    juce::TextButton load { "LOAD" }, play { "PLAY" }, cue { "CUE" }, sync { "SYNC" }, loop4 { "LOOP 4" }, loop8 { "LOOP 8" };
    double loopStart = 0.0, loopLen = 0.0;
    juce::OwnedArray<juce::TextButton> pads;
    juce::Slider pitch;
};

// Serato-style centre: both tracks' zoomed waveforms stacked (playhead in the middle) + full-song overviews
struct Waves : public juce::Component, private juce::Timer
{
    Waves (Deck& x, Deck& y) : a (x), b (y) { startTimerHz (60); }
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
        g.drawText (juce::String (row + 1), 4, 0, 34, h, juce::Justification::centredRight);
        g.drawText (f.getFileNameWithoutExtension(), 46, 0, w - 200, h, juce::Justification::centredLeft, true);
        g.setColour (col::dim);
        g.drawText (f.getFileExtension().substring (1).toUpperCase() + "  " + juce::File::descriptionOfSizeInBytes (f.getSize()), w - 150, 0, 144, h, juce::Justification::centredRight);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row < files.size()) (a.tp.isPlaying() && ! b.tp.isPlaying() ? b : a).loadFile (files[row]);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        g.drawText ("#    SONG  -  " + dir.getFullPathName() + "  (" + juce::String (files.size()) + ")", 8, 4, getWidth() - 400, 18, juce::Justification::centredLeft, true);
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
        const float ga = std::cos (x * juce::MathConstants<float>::halfPi) * m, gb = std::sin (x * juce::MathConstants<float>::halfPi) * m;
        for (auto [d, gain] : { std::pair<Deck*, float> { &a, ga }, { &b, gb } })
        {
            juce::AudioSourceChannelInfo t (&tmp, 0, info.numSamples);
            d->rs.getNextAudioBlock (t);
            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                info.buffer->addFrom (c, info.startSample, tmp, juce::jmin (c, 1), 0, info.numSamples, gain);
        }
    }
    Deck& a; Deck& b; juce::AudioBuffer<float> tmp;
    std::atomic<float> fade { 0.5f }, master { 0.8f };
};

class DjView : public juce::Component, private juce::Timer
{
public:
    explicit DjView (Studio& st) : s (st)
    {
        fm.registerBasicFormats();
        io.startThread (juce::Thread::Priority::normal);
        addAndMakeVisible (a); addAndMakeVisible (b); addAndMakeVisible (waves); addAndMakeVisible (library);
        a.onSync = [this] { a.matchTo (b); }; b.onSync = [this] { b.matchTo (a); };
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
        player.setSource (&mix);
        startTimerHz (30);
        s.engine.deviceManager.addAudioCallback (&player);
        setSize (phoneUI ? 380 : 1200, phoneUI ? 760 : 760);
    }
    ~DjView() override
    {
        s.engine.deviceManager.removeAudioCallback (&player);
        player.setSource (nullptr);
        io.stopThread (2000);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        g.drawText ("A   <   CROSSFADER   >   B", xf.getBounds().translated (0, -16).withHeight (14), juce::Justification::centred);
        g.drawText ("DJ OUT", vol.getBounds().translated (0, -16).withHeight (14), juce::Justification::centred);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        if (phoneUI)
        {
            a.setBounds (r.removeFromTop (230)); waves.setBounds (r.removeFromTop (120).reduced (0, 4)); b.setBounds (r.removeFromTop (230));
            auto mix = r.removeFromTop (44); mix.removeFromTop (16);
            vol.setBounds (mix.removeFromRight (100)); mix.removeFromRight (6);
            autoMix.setBounds (mix.removeFromLeft (64)); fadeBtn.setBounds (mix.removeFromRight (48)); xf.setBounds (mix.reduced (6, 0));
            library.setBounds (r);
            return;
        }
        auto top = r.removeFromTop (juce::jmax (300, r.getHeight() * 5 / 10));
        const int dw = juce::jmin (330, top.getWidth() / 4);
        a.setBounds (top.removeFromLeft (dw)); b.setBounds (top.removeFromRight (dw));
        waves.setBounds (top.reduced (6, 0));
        auto mix = r.removeFromTop (44); mix.removeFromTop (16);
        vol.setBounds (mix.removeFromRight (180)); mix.removeFromRight (16);
        autoMix.setBounds (mix.removeFromLeft (90)); mix.removeFromLeft (6); fadeBtn.setBounds (mix.removeFromLeft (64));
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
            const double k = juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - fadeT0) / 8000.0);
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
    juce::TextButton autoMix { "AUTO MIX" }, fadeBtn { "FADE" };
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
