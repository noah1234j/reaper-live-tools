// ---------------------------------------------------------------------------
// ZoneModel.cpp  —  parser and serializer for CSI .zon files
// ---------------------------------------------------------------------------

#include "ZoneModel.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace SurfaceEditor {

// ---------------------------------------------------------------------------
// Zone type names
// ---------------------------------------------------------------------------

static const char* s_zoneTypeNames[] = {
    "Home",
    "Track",
    "SelectedTrack",
    "SelectedTracks",
    "MasterTrack",
    "MasterTrackFXMenu",
    "TrackSend",
    "TrackReceive",
    "TrackFXMenu",
    "SelectedTrackSend",
    "SelectedTrackReceive",
    "SelectedTrackFXMenu",
    "SelectedTrackFX",
    "VCA",
    "Folder",
    "FocusedFX",
    "LastTouchedFXParam",
    "GoZones",
};
static const int s_zoneTypeCount = (int)(sizeof(s_zoneTypeNames) / sizeof(s_zoneTypeNames[0]));

const char* const* GetZoneTypeNames(int* countOut)
{
    if (countOut) *countOut = s_zoneTypeCount;
    return s_zoneTypeNames;
}

// ---------------------------------------------------------------------------
// CSI built-in action list
// ---------------------------------------------------------------------------

static const CsiAction s_csiActions[] = {
    // Volume / Pan / Width
    { "Track Volume",               "TrackVolume"            },
    { "Track Pan",                  "TrackPan"               },
    { "Track Pan Auto Left",        "TrackPanAutoLeft"       },
    { "Track Pan Auto Right",       "TrackPanAutoRight"      },
    { "Track Pan Width",            "TrackPanWidth"          },
    // Mute / Solo / Rec
    { "Track Mute",                 "TrackMute"              },
    { "Track Solo",                 "TrackSolo"              },
    { "Track Record Arm",           "TrackRecordArm"         },
    // Selection
    { "Track Unique Select",        "TrackUniqueSelect"      },
    { "Track Range Select",         "TrackRangeSelect"       },
    { "Track Select",               "TrackSelect"            },
    // Sends / Receives
    { "Track Send Volume",          "TrackSendVolume"        },
    { "Track Send Pan",             "TrackSendPan"           },
    { "Track Send Mute",            "TrackSendMute"          },
    // FX parameters
    { "FX Param 0",                 "FXParam 0"              },
    { "FX Param Name Display 0",    "FXParamNameDisplay 0"   },
    { "FX Param Value Display 0",   "FXParamValueDisplay 0"  },
    // Displays
    { "Track Name Display",         "TrackNameDisplay"       },
    { "Track Pan Display",          "TrackPanAutoLeftDisplay" },
    { "Track Volume Display",       "TrackVolumeDisplay"     },
    { "Track Output Meter Max Peak LR", "TrackOutputMeterMaxPeakLR" },
    // Transport
    { "Play",                       "Play"                   },
    { "Stop",                       "Stop"                   },
    { "Record",                     "Record"                 },
    { "Rewind",                     "Rewind"                 },
    { "Fast Forward",               "FastForward"            },
    { "Cycle Timeline",             "CycleTimeline"          },
    // Modifiers
    { "Shift (modifier)",           "Shift"                  },
    { "Option (modifier)",          "Option"                 },
    { "Control (modifier)",         "Control"                },
    { "Alt (modifier)",             "Alt"                    },
    { "Flip (modifier)",            "Flip"                   },
    // Banking
    { "Bank Track -8",              "Bank Track -8"          },
    { "Bank Track +8",              "Bank Track 8"           },
    { "Bank Track -1",              "Bank Track -1"          },
    { "Bank Track +1",              "Bank Track 1"           },
    { "Bank Send -1",               "Bank Send -1"           },
    { "Bank Send +1",               "Bank Send 1"            },
    { "Bank FX -1",                 "Bank FX -1"             },
    { "Bank FX +1",                 "Bank FX 1"              },
    // Zone navigation
    { "Go Zone: Selected Track Send",    "GoZone SelectedTrackSend"     },
    { "Go Zone: Selected Track Receive", "GoZone SelectedTrackReceive"  },
    { "Go Zone: Selected Track FX Menu", "GoZone SelectedTrackFXMenu"   },
    { "Go Zone: Master Track FX Menu",   "GoZone MasterTrackFXMenu"     },
    { "Go Home Zone",                    "GoZone Home"                  },
    // Channel control
    { "Toggle Channel",             "ToggleChannel"          },
    // Misc
    { "Reaper Command (by ID)",     "Reaper "                },
    { "No Action",                  "NoAction"               },
};
static const int s_csiActionCount = (int)(sizeof(s_csiActions) / sizeof(s_csiActions[0]));

const CsiAction* GetCsiActions(int* countOut)
{
    if (countOut) *countOut = s_csiActionCount;
    return s_csiActions;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> Tokenize(const std::string& line)
{
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok)
        tokens.push_back(tok);
    return tokens;
}

// Split "Shift+Control+Fader1" into modifier="Shift+Control", widget="Fader1"
static void SplitModifierWidget(const std::string& expr,
                                std::string& modifierOut,
                                std::string& widgetOut)
{
    // Find the last '+' that separates a modifier from the widget name
    // Modifiers are: Shift, Option, Control, Alt, Flip, Hold, Touch, InvertFB
    static const char* kModifiers[] = {
        "Shift", "Option", "Control", "Alt", "Flip", "Hold", "Touch", "InvertFB",
        "Toggle", nullptr
    };

    size_t lastPlus = std::string::npos;
    size_t pos      = 0;
    while ((pos = expr.find('+', pos)) != std::string::npos)
    {
        // Check if the portion before this '+' ends with a known modifier
        std::string before = expr.substr(0, pos);
        // Find the last '+' in 'before' to get the last segment
        size_t prev = before.rfind('+');
        std::string seg = (prev == std::string::npos) ? before : before.substr(prev + 1);
        bool isKnownMod = false;
        for (int i = 0; kModifiers[i]; ++i)
        {
            if (seg == kModifiers[i]) { isKnownMod = true; break; }
        }
        if (isKnownMod)
            lastPlus = pos;
        ++pos;
    }

    if (lastPlus != std::string::npos)
    {
        modifierOut = expr.substr(0, lastPlus);
        widgetOut   = expr.substr(lastPlus + 1);
    }
    else
    {
        modifierOut = "";
        widgetOut   = expr;
    }
}

// ---------------------------------------------------------------------------
// ParseZoneFile
// ---------------------------------------------------------------------------

ZoneFile ParseZoneFile(const std::string& filePath)
{
    ZoneFile zf;
    zf.filePath = filePath;

    std::ifstream f(filePath);
    if (!f.is_open())
        return zf;

    enum State { sTop, sZone, sIncluded, sSubZones };
    State state = sTop;

    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = Trim(line);

        if (trimmed.empty())
            continue;

        // Extract and save inline comment
        std::string comment;
        std::string body = trimmed;
        if (body.size() > 0 && body[0] != '/')
        {
            size_t cpos = body.find("//");
            if (cpos != std::string::npos)
            {
                comment = Trim(body.substr(cpos + 2));
                body    = Trim(body.substr(0, cpos));
            }
        }

        if (trimmed[0] == '/' || body.empty())
            continue; // pure comment line

        auto tokens = Tokenize(body);
        if (tokens.empty()) continue;

        if (state == sTop)
        {
            if (tokens[0] == "Zone")
            {
                if (tokens.size() > 1) zf.zoneName  = tokens[1];
                if (tokens.size() > 2) zf.zoneAlias = tokens[2];
                state = sZone;
            }
            continue;
        }

        if (state == sZone)
        {
            if (tokens[0] == "ZoneEnd")
            {
                state = sTop;
                continue;
            }

            if (tokens[0] == "IncludedZones")
            {
                state = sIncluded;
                continue;
            }

            if (tokens[0] == "SubZones")
            {
                state = sSubZones;
                continue;
            }

            if (tokens[0] == "OnInitialization" && tokens.size() > 1)
            {
                // remainder is the action
                std::string act;
                for (size_t i = 1; i < tokens.size(); ++i)
                {
                    if (i > 1) act += " ";
                    act += tokens[i];
                }
                zf.onInit.push_back(act);
                continue;
            }

            // Otherwise: a widget → action assignment line
            if (tokens.size() >= 2)
            {
                ZoneAssignment za;
                SplitModifierWidget(tokens[0], za.modifier, za.widgetExpr);

                // Action token (first after widget)
                za.action = tokens[1];

                // Parameters: everything after the action token
                for (size_t i = 2; i < tokens.size(); ++i)
                {
                    if (i > 2) za.params += " ";
                    za.params += tokens[i];
                }

                za.comment = comment;
                zf.assignments.push_back(za);
            }
            continue;
        }

        if (state == sIncluded)
        {
            if (tokens[0] == "IncludedZonesEnd")
            {
                state = sZone;
                continue;
            }
            zf.includedZones.push_back(tokens[0]);
            continue;
        }

        if (state == sSubZones)
        {
            if (tokens[0] == "SubZonesEnd")
            {
                state = sZone;
                continue;
            }
            zf.subZones.push_back(tokens[0]);
            continue;
        }
    }

    return zf;
}

// ---------------------------------------------------------------------------
// WriteZoneFile
// ---------------------------------------------------------------------------

bool WriteZoneFile(const ZoneFile& zone)
{
    std::ofstream f(zone.filePath);
    if (!f.is_open())
        return false;

    // Zone header
    f << "Zone " << zone.zoneName;
    if (!zone.zoneAlias.empty())
        f << " " << zone.zoneAlias;
    f << "\n";

    // OnInitialization lines
    for (const auto& init : zone.onInit)
        f << "    OnInitialization     " << init << "\n";

    if (!zone.onInit.empty())
        f << "\n";

    // IncludedZones block
    if (!zone.includedZones.empty())
    {
        f << "    IncludedZones\n";
        for (const auto& iz : zone.includedZones)
            f << "        " << iz << "\n";
        f << "    IncludedZonesEnd\n\n";
    }

    // SubZones block
    if (!zone.subZones.empty())
    {
        f << "    SubZones\n";
        for (const auto& sz : zone.subZones)
            f << "        " << sz << "\n";
        f << "    SubZonesEnd\n\n";
    }

    // Assignments — group by modifier for readability
    std::string lastMod = "\x01"; // sentinel to force first header
    for (const auto& za : zone.assignments)
    {
        // Simple indent
        f << "    " << za.FullWidgetExpr();

        // Pad to column 24 for alignment (best-effort)
        std::string full = za.FullWidgetExpr();
        int pad = 24 - (int)full.size();
        if (pad < 1) pad = 1;
        f << std::string(pad, ' ');

        f << za.action;

        if (!za.params.empty())
            f << " " << za.params;

        if (!za.comment.empty())
            f << "   // " << za.comment;

        f << "\n";
    }

    f << "ZoneEnd\n";
    return true;
}

} // namespace SurfaceEditor
