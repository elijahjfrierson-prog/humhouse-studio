#pragma once
#include "../Model.h"
#include "../Instruments.h"
#include <memory>
#include <vector>

namespace hhs
{
struct DrumHit { double beat; int lane; float vel; int variant; };
enum DrumGroup { GroupKick = 0, GroupSnare, GroupHats, GroupToms, GroupCymbals, NumDrumGroups };
const char* drumGroupName (int g);
int drumGroupOf (int lane);
float drummerDynamics (const DrummerParams&, double songBeat);   // velocity scale from the per-section dynamics %, ramped over the last 2 bars   // kit lane -> DrumGroup

// the hits a Drummer region of `lengthBeats` plays (fill into the end of the region); cached, message thread
std::shared_ptr<const std::vector<DrumHit>> drummerHits (const DrummerParams&, double bpm, double lengthBeats);
juce::Array<juce::File> drumKits();               // the bundled rock kits (Sounds/Rock Drums)
juce::File defaultDrumKit();
// adds a Drummer channel + its own Playlist track with an 8-bar region at `startBeat`; returns the channel id
int addDrummerTrack (Project&, double startBeat, int bars = 8);
// turns an existing Playlist track into the Session Drummer (8-bar region at `startBeat`); returns the channel id
int makeLaneDrummer (Project&, int lane, double startBeat, int bars = 8);
std::unique_ptr<ChannelInstrument> createDrummerInstrument (const Channel&, double sampleRate);
}
