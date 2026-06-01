#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// SurfaceModel.h  —  data model for CSI Surface.txt (hardware definition)
//
// A Surface.txt file defines the physical controls on a MIDI controller:
//   Widget <name>
//       <GeneratorType>  bb nn vv      <- input message (what the hardware sends)
//       <FeedbackType>   bb nn vv      <- output message (what we send back)
//       Touch  bb nn vv                <- optional touch sense bytes
//       Toggle bb nn vv                <- optional toggle modifier
//   WidgetEnd
//
// The model here is intentionally simple: raw keyword + 3 hex bytes so we
// don't lose data during round-trip editing.
// ---------------------------------------------------------------------------

namespace SurfaceEditor {

// A single MIDI message triple (status, note/CC, value)
struct MidiBytes {
    uint8_t b0 = 0; // status byte  (e.g. 0x90 = note-on ch1)
    uint8_t b1 = 0; // data byte 1  (note number or CC)
    uint8_t b2 = 0x7f; // data byte 2 (velocity / value)
};

// One generator entry line inside a Widget block
struct MidiGenerator {
    std::string keyword;    // e.g. "Press", "Fader14Bit", "Encoder"
    MidiBytes   bytes;
    // Extra bytes used by some types (e.g. second touch message pair)
    bool        hasExtra   = false;
    MidiBytes   extraBytes;
};

// One feedback processor entry line inside a Widget block
struct FeedbackProcessor {
    std::string keyword;    // e.g. "FB_TwoState", "FB_Fader14Bit"
    MidiBytes   bytes;
};

// A widget represents one physical control on the hardware surface
struct Widget {
    std::string              name;          // e.g. "Fader1", "UpperButton3"
    std::vector<MidiGenerator>     generators;   // input message generators
    std::vector<FeedbackProcessor> feedbackProcs;// output feedback processors
    bool hasTouch  = false;
    MidiBytes touchOn;
    MidiBytes touchOff;
    bool hasToggle = false;
    MidiBytes toggleBytes;

    // Grid position for the visual canvas editor (not part of Surface.txt)
    int gridCol = 0;
    int gridRow = 0;
    // Free canvas position set by dragging; -1 = derive from grid layout.
    // Not persisted to Surface.txt — canvas layout only.
    int pixX = -1;
    int pixY = -1;
};

// The complete parsed contents of one Surface.txt file
struct Surface {
    std::string          name;          // derived from parent folder name
    std::string          filePath;      // absolute path to Surface.txt
    std::vector<Widget>  widgets;
    int                  channelCount = 8;   // how many multi-channel strips
    int                  gridCols     = 8;
    int                  gridRows     = 4;
    // Unparsed header lines (comments, StepSize, AccelerationValues blocks)
    // preserved verbatim so round-trip doesn't lose them
    std::string          headerBlock;
    // Per-row data for the visual canvas (populated after parsing, not stored to disk)
    std::vector<int>     rowHeights;     // pixel height of each canvas row
    std::vector<int>     rowPriorities;  // semantic priority per row (0=display, 60=fader, etc.)
};

// ---------------------------------------------------------------------------
// Parse a Surface.txt file from disk.
// Returns an empty Surface with filePath set on failure.
// ---------------------------------------------------------------------------
Surface ParseSurfaceFile(const std::string& filePath);

// ---------------------------------------------------------------------------
// Write a Surface back to disk.
// Returns true on success.
// ---------------------------------------------------------------------------
bool WriteSurfaceFile(const Surface& surf);

// ---------------------------------------------------------------------------
// Known MIDI generator keyword list (for UI dropdowns)
// ---------------------------------------------------------------------------
const char* const* GetGeneratorKeywords(int* countOut);

// ---------------------------------------------------------------------------
// Known feedback processor keyword list (for UI dropdowns)
// ---------------------------------------------------------------------------
const char* const* GetFeedbackKeywords(int* countOut);

// ---------------------------------------------------------------------------
// Infer widget "type" for canvas colouring / palette
// ---------------------------------------------------------------------------
enum class WidgetType {
    Button,
    Fader,
    Encoder,
    Display,
    VUMeter,
    Unknown
};
WidgetType InferWidgetType(const Widget& w);
const char* WidgetTypeName(WidgetType t);

} // namespace SurfaceEditor
