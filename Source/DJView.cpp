#include "UI.h"

namespace hhs
{
namespace
{
// HumHouse DJ: two decks + crossfader on their own audio callback beside the engine (never inside the engine's block)
struct Deck : public juce::Component, private juce::Timer
{
    explicit Deck (juce::AudioFormatManager& f, juce::TimeSliceThread& t, juce::String nm) : fm (f), io (t), title (std::move (nm))
    {
        for (auto* b : { &load, &play, &cue, &sync }) addAndMakeVisible (b);
        load.onClick = [this] { pick(); };
        play.onClick = [this] { if (tp.isPlaying()) tp.stop(); else tp.start(); };
        cue.onClick = [this] { if (tp.isPlaying()) { tp.stop(); tp.setPosition (cuePos); } else { cuePos = tp.getCurrentPosition(); tp.setPosition (cuePos); } };
        sync.onClick = [this] { if (onSync) onSync(); };
        pitch.setSliderStyle (juce::Slider::LinearVertical); pitch.setRange (-16.0, 16.0, 0.01); pitch.setValue (0.0);
        pitch.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20); pitch.setTextValueSuffix (" %");
        pitch.setDoubleClickReturnValue (true, 0.0);
        pitch.onValueChange = [this] { rs.setResamplingRatio (1.0 + pitch.getValue() / 100.0); };
        addAndMakeVisible (pitch);
        startTimerHz (30);
    }
    ~Deck() override { tp.setSource (nullptr); }
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
        tp.stop(); tp.setSource (nullptr);
        src = std::make_unique<juce::AudioFormatReaderSource> (r.release(), true);
        tp.setSource (src.get(), 32768, &io, sr, 2);   // read-ahead on its own thread: the audio callback never touches the disk
        name = f.getFileNameWithoutExtension(); cuePos = 0.0;
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
    double liveBpm() const { return bpm * (1.0 + pitch.getValue() / 100.0); }
    void matchTo (const Deck& o) { if (bpm > 0 && o.bpm > 0) pitch.setValue (juce::jlimit (-16.0, 16.0, (o.liveBpm() / bpm - 1.0) * 100.0)); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        g.setColour (col::accent); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (title, 10, 6, getWidth() - 20, 18, juce::Justification::centredLeft);
        g.setColour (col::text); g.setFont (juce::FontOptions (12.0f));
        g.drawText (name.isEmpty() ? juce::String ("LOAD a song") : name, 10, 26, getWidth() - 100, 18, juce::Justification::centredLeft, true);
        const double len = tp.getLengthInSeconds(), pos = tp.getCurrentPosition();
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText (juce::String (liveBpm(), 1) + " BPM", 10, 48, 160, 26, juce::Justification::centredLeft);
        auto bar = juce::Rectangle<float> (10.0f, 80.0f, (float) getWidth() - 100.0f, 10.0f);
        g.setColour (col::bg); g.fillRoundedRectangle (bar, 3.0f);
        if (len > 0) { g.setColour (col::accent); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) (pos / len)), 3.0f);
                       g.setColour (col::text); g.drawVerticalLine ((int) (bar.getX() + bar.getWidth() * (float) (cuePos / len)), bar.getY() - 3, bar.getBottom() + 3); }
    }
    void mouseDown (const juce::MouseEvent& e) override   // click the bar to jump
    {
        const auto bar = juce::Rectangle<int> (10, 74, getWidth() - 100, 22);
        if (bar.contains (e.getPosition()) && tp.getLengthInSeconds() > 0)
            tp.setPosition (tp.getLengthInSeconds() * (e.x - bar.getX()) / juce::jmax (1, bar.getWidth()));
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        pitch.setBounds (r.removeFromRight (74));
        auto row = r.removeFromBottom (34);
        const int w = row.getWidth() / 4;
        load.setBounds (row.removeFromLeft (w).reduced (2)); play.setBounds (row.removeFromLeft (w).reduced (2));
        cue.setBounds (row.removeFromLeft (w).reduced (2)); sync.setBounds (row.reduced (2));
    }
    void timerCallback() override { play.setButtonText (tp.isPlaying() ? "PAUSE" : "PLAY"); repaint (0, 40, getWidth() - 90, 60); }

    juce::AudioFormatManager& fm; juce::TimeSliceThread& io;
    juce::String title, name;
    juce::AudioTransportSource tp;
    juce::ResamplingAudioSource rs { &tp, false, 2 };
    std::unique_ptr<juce::AudioFormatReaderSource> src;
    std::unique_ptr<juce::FileChooser> chooser;
    double bpm = 0.0, cuePos = 0.0;
    std::function<void()> onSync;
    juce::TextButton load { "LOAD" }, play { "PLAY" }, cue { "CUE" }, sync { "SYNC" };
    juce::Slider pitch;
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

class DjView : public juce::Component
{
public:
    explicit DjView (Studio& st) : s (st)
    {
        fm.registerBasicFormats();
        io.startThread (juce::Thread::Priority::normal);
        addAndMakeVisible (a); addAndMakeVisible (b);
        a.onSync = [this] { a.matchTo (b); }; b.onSync = [this] { b.matchTo (a); };
        xf.setSliderStyle (juce::Slider::LinearHorizontal); xf.setRange (0.0, 1.0); xf.setValue (0.5);
        xf.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0); xf.setDoubleClickReturnValue (true, 0.5);
        xf.onValueChange = [this] { mix.fade = (float) xf.getValue(); };
        vol.setSliderStyle (juce::Slider::LinearHorizontal); vol.setRange (0.0, 1.0); vol.setValue (0.8);
        vol.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        vol.onValueChange = [this] { mix.master = (float) vol.getValue(); };
        for (auto* c : { (juce::Component*) &xf, (juce::Component*) &vol }) addAndMakeVisible (c);
        player.setSource (&mix);
        s.engine.deviceManager.addAudioCallback (&player);
        setSize (phoneUI ? 380 : 760, phoneUI ? 620 : 300);
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
        auto r = getLocalBounds().reduced (8);
        auto bottom = r.removeFromBottom (54);
        bottom.removeFromTop (18);
        vol.setBounds (bottom.removeFromRight (160)); bottom.removeFromRight (16);
        xf.setBounds (bottom);
        if (phoneUI) { a.setBounds (r.removeFromTop (r.getHeight() / 2).reduced (0, 4)); b.setBounds (r.reduced (0, 4)); }
        else { a.setBounds (r.removeFromLeft (r.getWidth() / 2).reduced (4, 0)); b.setBounds (r.reduced (4, 0)); }
    }
private:
    Studio& s;
    juce::AudioFormatManager fm;
    juce::TimeSliceThread io { "HumHouse DJ read-ahead" };
    Deck a { fm, io, "DECK A" }, b { fm, io, "DECK B" };
    DjMix mix { a, b };
    juce::AudioSourcePlayer player;
    juce::Slider xf, vol;
};

struct DjWindow : public juce::DocumentWindow
{
    explicit DjWindow (Studio& s) : juce::DocumentWindow ("HumHouse DJ", col::panel, juce::DocumentWindow::closeButton)
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
