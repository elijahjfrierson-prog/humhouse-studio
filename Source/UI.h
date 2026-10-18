#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "ProjectIO.h"
#include "NoteTools.h"

namespace hhs
{
namespace col
{
    // mutable so the theme color wheel can recolor the whole DAW
    inline juce::Colour bg (0xff0e1013), panel (0xff16191e), panel2 (0xff1d2127), grid (0xff22262d), gridBeat (0xff2c3139),
        gridBar (0xff3c434e), text (0xffdfe4ec), dim (0xff7f8896), accent (0xffff8a3d), accent2 (0xff38d1ff),
        rec (0xffff3b57), good (0xff3ddc84), scale (0xff1a2721), root (0xff21392d);
}

class HHLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HHLookAndFeel();
    void refreshColours();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
};

// Rotary knob; hovering shows its name, what it does and its value (tooltip + hint bar + popup).
class Knob : public juce::Slider
{
public:
    Knob (const juce::String& name, const juce::String& help, double mn, double mx, double def, const juce::String& suffix = {});
};

void setupButton (juce::Button&, const juce::String& text, const juce::String& tooltip, bool toggle = false);
void askText (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> done);

// Shared app state: every edit goes through edit() so it is locked against the audio thread,
// pushed to the 999-level undo history and broadcast to all views.
struct Studio : public juce::ChangeBroadcaster
{
    explicit Studio (Engine& e) : engine (e) {}
    Engine& engine;
    UndoHistory undo;
    juce::PropertiesFile* settings = nullptr;
    int selectedChannel = -1, selectedTrack = 1;
    juce::SortedSet<int> selectedLanes;   // highlighted playlist tracks (click / Shift / Ctrl-Cmd)
    int laneAnchor = -1;
    bool dirty = false;
    std::function<void (int view)> showView;   // 0 beat, 1 record, 2 mix

    Project& p() { return engine.project(); }
    template <typename F> void live (F&& f) { const juce::ScopedLock sl (engine.getLock()); f (p()); }
    template <typename F> void edit (const juce::String& what, F&& f) { live (std::forward<F> (f)); commit (what); }
    void commit (const juce::String& what);
    void undoStep (int delta);
    void jumpHistory (int index);
    NoteList& notes (int channelId);
    Channel* channel (int id);
};

std::unique_ptr<juce::Component> createMainComponent();
std::unique_ptr<juce::Component> createChannelRack (Studio&);
std::unique_ptr<juce::Component> createPianoRoll (Studio&);
std::unique_ptr<juce::Component> createPlaylist (Studio&);
std::unique_ptr<juce::Component> createMixer (Studio&);
std::unique_ptr<juce::Component> createBrowser (Studio&);

void addSampleChannels (Studio&, const juce::Array<juce::File>&, const juce::String& insertName);
void importKit (Studio&, const juce::File& folder);
void addPluginChannel (Studio&, const juce::PluginDescription&);

void showPluginHouseMenu (Studio&, int track, int replaceSlotUid = -1);
void addEffect (Studio&, int track, const juce::String& type, const juce::String& pluginId, int replaceSlotUid = -1);
void showPluginScanner (Studio&);

void openEffectEditor (Studio&, int track, int slotUid);
void openChannelEditor (Studio&, int channelId);
void closeAllEditorWindows();
void closeEditorsForPlugin (juce::AudioProcessor*);

void applyTheme (juce::Colour accent, float tint);   // tint 0..1 = how much the accent colors the background
void resetTheme();
void showThemeDialog (Studio&);
void drawLogo (juce::Graphics&, juce::Rectangle<float> area, bool withText);

enum class Tool { Arp, Strum, Chop, Flam, Quantize, Randomize, Limit, Articulate, ScaleLevels, Flip, Claw, Lfo };
void showToolDialog (Studio&, Tool, int channelId);
}
