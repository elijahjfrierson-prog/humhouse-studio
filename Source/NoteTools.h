#pragma once
#include "Model.h"

// FL-style piano roll tools. Every tool acts on selected notes, or on all notes if none are selected.
namespace hhs::tools
{
bool anySelected (const NoteList&);
template <typename F> void forTargets (NoteList& notes, F&& f)
{
    const bool sel = anySelected (notes);
    for (auto& n : notes) if (! sel || n.selected) f (n);
}

struct ArpSettings  { double rate = 0.125; bool triplet = false; int range = 1; int direction = 0; /*0 up 1 down 2 up-down 3 as played*/
                      float gate = 0.9f; float velStart = 1.0f, velEnd = 1.0f; };
struct StrumSettings{ double time = 0.03; float tension = 0.0f; float velocity = 0.0f; bool preserveEnd = true; bool alternate = false; };
struct ChopSettings { double rate = 0.125; bool triplet = false; float gate = 1.0f; };
struct FlamSettings { double time = 0.02; float velocity = 0.6f; };
struct QuantizeSettings { double grid = 0.25; float strength = 1.0f; bool starts = true, ends = false; };
struct RandomSettings { int octave = 5; int octaves = 1; double region = 4.0; double length = 0.25; float population = 0.5f;
                        float velMin = 0.5f, velMax = 1.0f; int seed = 1; };
struct LimitSettings { int lowKey = 36, highKey = 96; bool toScale = true; };
struct ScaleLevelSettings { float multiply = 1.0f, offset = 0.0f; };
struct ClawSettings { int period = 2; int trashEvery = 2; float timeDistort = 0.0f; };
struct LfoSettings  { int shape = 0; double period = 1.0; float minV = 0.3f, maxV = 1.0f; float phase = 0.0f; };

void arpeggiate (NoteList&, const ArpSettings&);           // Alt+A (also snare / hi-hat rolls)
void strum (NoteList&, const StrumSettings&);              // Alt+S
void chop (NoteList&, const ChopSettings&);                // Alt+U
void flam (NoteList&, const FlamSettings&);                // Alt+F
void quantize (NoteList&, const QuantizeSettings&);        // Alt+Q / Ctrl+Alt+Q / Shift+Q
void randomize (NoteList&, const RandomSettings&, int root, int scaleIdx, double atBeat); // Alt+R
void limit (NoteList&, const LimitSettings&, int root, int scaleIdx); // Alt+K
void articulate (NoteList&, float lengthScale);            // Alt+L
void scaleLevels (NoteList&, const ScaleLevelSettings&);   // Alt+X
void flip (NoteList&, bool horizontal, bool vertical);     // Alt+Y
void claw (NoteList&, const ClawSettings&);                // Alt+W
void lfo (NoteList&, const LfoSettings&);                  // Alt+O

void glue (NoteList&);                     // Ctrl+G
void legato (NoteList&);                   // Ctrl+L
void quickChop (NoteList&, double grid);   // Ctrl+U
void transpose (NoteList&, int semitones); // Shift/Ctrl+Up/Down
void move (NoteList&, double beats);       // Shift+Left/Right, Alt nudge
void duplicateRight (NoteList&);           // Ctrl+B
void deleteSelected (NoteList&);           // Del
void muteSelected (NoteList&, bool mute);  // Alt+M / Alt+Shift+M
void discardLengths (NoteList&, double len);// Shift+D
void selectAll (NoteList&, bool);
void invertSelection (NoteList&);
void selectRandom (NoteList&, int seed);
void sortNotes (NoteList&);
void rollStep (NoteList&, double start, double stepLen, int divisions, int key, float vel); // channel rack roll
}
