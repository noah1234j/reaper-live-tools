#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ZoneModel.h  —  data model for CSI .zon files
//
// A .zon file looks like:
//
//   Zone Home
//       Play  Play
//       Stop  Stop
//       Shift+Fader1  TrackVolume   // trailing comment
//   ZoneEnd
//
// Multiple modifier prefixes (Shift, Option, Control, Alt, Flip, Hold, Touch)
// can be combined with + before the widget name.
// The pipe character | in widget names expands to per-channel (handled by CSI
// at runtime; here we just store the raw expression).
// ---------------------------------------------------------------------------

namespace SurfaceEditor {

// One "widget → action" mapping line in a zone file
struct ZoneAssignment {
    std::string widgetExpr;   // raw widget expression, e.g. "Fader|" or "RotaryPush8"
    std::string modifier;     // modifier prefix, e.g. "Shift" / "Hold+Option" / ""
    std::string action;       // action name, e.g. "TrackVolume" / "Reaper 40001"
    std::string params;       // optional trailing parameters, e.g. "RingStyle=Dot"
    std::string comment;      // inline comment stripped from the line (for round-trip)

    // Build the full "widget" field as it appears in the .zon file
    std::string FullWidgetExpr() const
    {
        if (modifier.empty())
            return widgetExpr;
        return modifier + "+" + widgetExpr;
    }
};

// Special zone directive types (OnInitialization, IncludedZones, SubZones)
struct ZoneDirective {
    std::string keyword; // e.g. "OnInitialization", "IncludedZones", "SubZones"
    std::string value;   // the rest of the line
};

// The complete contents of one .zon file
struct ZoneFile {
    std::string                   filePath;
    std::string                   zoneName;    // the name after "Zone"
    std::string                   zoneAlias;   // optional alias after the name
    std::vector<std::string>      includedZones;
    std::vector<std::string>      subZones;
    std::vector<std::string>      onInit;      // OnInitialization action lines
    std::vector<ZoneAssignment>   assignments;
    // Unrecognised lines preserved for round-trip
    std::vector<std::string>      rawLines;
};

// ---------------------------------------------------------------------------
// Known zone type names (for dropdown in zone editor)
// ---------------------------------------------------------------------------
const char* const* GetZoneTypeNames(int* countOut);

// ---------------------------------------------------------------------------
// Known CSI built-in actions (for action search dialog)
// ---------------------------------------------------------------------------
struct CsiAction {
    const char* name;    // display name shown in picker
    const char* token;   // the token(s) written into the .zon file
};
const CsiAction* GetCsiActions(int* countOut);

// ---------------------------------------------------------------------------
// Parse / serialize
// ---------------------------------------------------------------------------
ZoneFile ParseZoneFile(const std::string& filePath);
bool     WriteZoneFile(const ZoneFile& zone);

} // namespace SurfaceEditor
