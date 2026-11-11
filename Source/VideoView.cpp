#include "UI.h"
#if JUCE_MAC || JUCE_WINDOWS || JUCE_IOS
 #include <juce_video/juce_video.h>
 #define HHS_HAS_VIDEO 1
#else
 #define HHS_HAS_VIDEO 0
#endif

// VIDEO tab: score to picture. The video follows the song playhead; decoding runs on the OS player's own threads
// and the sync check is a 30 Hz message-thread timer, so nothing touches the audio thread or blocks the UI.
namespace hhs
{
namespace
{
class VideoView : public juce::Component, private juce::Timer
{
public:
    explicit VideoView (Studio& st) : s (st)
    {
        fm.registerBasicFormats();
        for (auto* b : { &load, &toRecord, &sound }) addAndMakeVisible (b);
        addAndMakeVisible (offset); addAndMakeVisible (info);
        offset.setRange (-600.0, 600.0, 0.01); offset.setValue (0.0); offset.setTextValueSuffix (" s");
        offset.setSliderStyle (juce::Slider::LinearHorizontal); offset.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        offset.setTooltip ("Where the video starts on the song timeline (seconds)");
        sound.setClickingTogglesState (true); sound.setTooltip ("Hear the video's own audio");
        info.setColour (juce::Label::textColourId, col::dim);
        info.setText ("IMPORT a video, then score it with your tracks and mixer. The picture follows the playhead.", juce::dontSendNotification);
        load.onClick = [this] { pick(); };
        toRecord.onClick = [this] { audioToRecord(); };
        sound.onClick = [this] {
           #if HHS_HAS_VIDEO
            video.setAudioVolume (sound.getToggleState() ? 1.0f : 0.0f);
           #endif
        };
       #if HHS_HAS_VIDEO
        addAndMakeVisible (video);
       #endif
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        if (! HHS_HAS_VIDEO || file == juce::File())
        {
            g.setColour (col::dim); g.setFont (juce::FontOptions (15.0f));
            g.drawText (HHS_HAS_VIDEO ? "No video loaded" : "Video playback needs the Mac, Windows or iPhone build", screen, juce::Justification::centred);
        }
    }
    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (phoneUI ? 44 : 34).reduced (4);
        load.setBounds (bar.removeFromLeft (90)); bar.removeFromLeft (4);
        toRecord.setBounds (bar.removeFromLeft (150)); bar.removeFromLeft (4);
        sound.setBounds (bar.removeFromLeft (110)); bar.removeFromLeft (8);
        offset.setBounds (bar.removeFromLeft (juce::jmin (320, bar.getWidth() / 2)));
        info.setBounds (bar);
        screen = r;
       #if HHS_HAS_VIDEO
        video.setBounds (r);
       #endif
    }
private:
    void pick()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import video", juce::File(), "*.mp4;*.mov;*.m4v;*.avi;*.wmv");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe = juce::Component::SafePointer<VideoView> (this)] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
            safe->file = fc.getResult();
            safe->info.setText (safe->file.getFileName(), juce::dontSendNotification);
           #if HHS_HAS_VIDEO
            safe->video.loadAsync (juce::URL (safe->file), [safe] (const juce::URL&, juce::Result r) {
                if (safe == nullptr) return;
                if (r.failed()) safe->info.setText ("Can't open this video: " + r.getErrorMessage(), juce::dontSendNotification);
                else safe->video.setAudioVolume (safe->sound.getToggleState() ? 1.0f : 0.0f);
            });
           #endif
            safe->repaint();
        });
    }
    // the video's audio lands on a new Record track as a normal clip at the video's start: move it anywhere after
    void audioToRecord()
    {
        if (! file.existsAsFile()) { info.setText ("Import a video first", juce::dontSendNotification); return; }
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (file));
        if (rd == nullptr || rd->sampleRate <= 0) { info.setText ("This system can't read the video's audio; export it as WAV and drag it in", juce::dontSendNotification); return; }
        const double secs = (double) rd->lengthInSamples / rd->sampleRate, at = juce::jmax (0.0, offset.getValue());
        const auto f = file;
        s.edit ("Video audio to Record", [&] (Project& p) {
            Lane l; l.name = f.getFileNameWithoutExtension() + " (video)"; l.mixerTrack = p.addMixerTrack (l.name); p.lanes.push_back (l);
            Clip c; c.uid = p.newUid(); c.lane = (int) p.lanes.size() - 1; c.audioFile = f; c.patternIndex = -1;
            c.start = secondsToBeats (at, p.bpm); c.length = secondsToBeats (secs, p.bpm);
            p.clips.push_back (c);
        });
        info.setText ("Video audio is on the Record timeline", juce::dontSendNotification);
    }
    void timerCallback() override
    {
       #if HHS_HAS_VIDEO
        if (! isShowing() || ! video.isVideoOpen()) { if (video.isVideoOpen() && video.isPlaying()) video.stop(); return; }
        const double t = beatsToSeconds (s.engine.positionBeats(), s.p().bpm) - offset.getValue();
        const double len = video.getVideoDuration();
        const bool inside = t >= 0.0 && t < len;
        if (s.engine.isPlaying() && inside)
        {
            if (! video.isPlaying()) { video.setPlayPosition (t); video.play(); }
            else if (std::abs (video.getPlayPosition() - t) > 0.08) video.setPlayPosition (t);   // drift > 2 frames: resync
        }
        else
        {
            if (video.isPlaying()) video.stop();
            const double want = juce::jlimit (0.0, juce::jmax (0.0, len - 0.01), t);
            if (std::abs (video.getPlayPosition() - want) > 0.02) video.setPlayPosition (want);   // scrub with the playhead
        }
       #endif
    }
    Studio& s;
    juce::AudioFormatManager fm;
    juce::File file;
    juce::Rectangle<int> screen;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TextButton load { "IMPORT" }, toRecord { "AUDIO TO RECORD" }, sound { "VIDEO SOUND" };
    juce::Slider offset;
    juce::Label info;
   #if HHS_HAS_VIDEO
    juce::VideoComponent video { false };
   #endif
};
}
std::unique_ptr<juce::Component> createVideoView (Studio& s) { return std::make_unique<VideoView> (s); }
}
