// ---------------------------------------------------------------------------
// SurfaceModel.cpp  —  parser and serializer for CSI Surface.txt files
// ---------------------------------------------------------------------------

#include "SurfaceModel.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace SurfaceEditor {

// Forward declarations
static int RowPriority(const Widget& w);

// ---------------------------------------------------------------------------
// Known keyword tables
// ---------------------------------------------------------------------------

static const char* s_genKeywords[] = {
    "Press",
    "AnyPress",
    "Touch",
    "Fader14Bit",
    "FaderportClassicFader14Bit",
    "Fader7Bit",
    "Encoder",
    "MFTEncoder",
    "EncoderPlain",
    "Encoder7Bit",
    "Toggle",
};
static const int s_genCount = (int)(sizeof(s_genKeywords) / sizeof(s_genKeywords[0]));

static const char* s_fbKeywords[] = {
    "FB_TwoState",
    "FB_Fader14Bit",
    "FB_FaderportClassicFader14Bit",
    "FB_Fader7Bit",
    "FB_Encoder",
    "FB_AsparionEncoder",
    "FB_MCUTimeDisplay",
    "FB_MCUAssignmentDisplay",
    "FB_MCUDisplayUpper",
    "FB_MCUDisplayLower",
    "FB_MCUXTDisplayUpper",
    "FB_MCUXTDisplayLower",
    "FB_XTouchDisplayUpper",
    "FB_XTouchDisplayLower",
    "FB_XTouchXTDisplayUpper",
    "FB_XTouchXTDisplayLower",
    "FB_MCUVUMeter",
    "FB_MCUXTVUMeter",
    "FB_QConProXMasterVUMeter",
    "FB_AsparionVUMeterL",
    "FB_AsparionVUMeterR",
    "FB_AsparionDisplayUpper",
    "FB_AsparionDisplayLower",
    "FB_AsparionDisplayEncoder",
    "FB_FaderportRGB",
    "FB_FaderportTwoStateRGB",
    "FB_FaderportValueBar",
    "FB_FPVUMeter",
    "FB_MFT_RGB",
    "FB_NovationLaunchpadMiniRGB7Bit",
    "FB_AsparionRGB",
    "FB_ConsoleOneVUMeter",
    "FB_ConsoleOneGainReductionMeter",
    "FB_SCE24LEDButton",
    "FB_SCE24OLEDButton",
    "FB_SCE24Encoder",
    "FB_SCE24EncoderText",
};
static const int s_fbCount = (int)(sizeof(s_fbKeywords) / sizeof(s_fbKeywords[0]));

const char* const* GetGeneratorKeywords(int* countOut)
{
    if (countOut) *countOut = s_genCount;
    return s_genKeywords;
}

const char* const* GetFeedbackKeywords(int* countOut)
{
    if (countOut) *countOut = s_fbCount;
    return s_fbKeywords;
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

// Split a string on whitespace into tokens
static std::vector<std::string> Tokenize(const std::string& line)
{
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok)
        tokens.push_back(tok);
    return tokens;
}

// Parse a hex string like "90" → 0x90; returns 0 on failure
static uint8_t ParseHex(const std::string& s)
{
    if (s.empty()) return 0;
    unsigned int v = 0;
    std::sscanf(s.c_str(), "%x", &v);
    return (uint8_t)(v & 0xFF);
}

// Fill MidiBytes from tokens starting at index idx
static void FillBytes(MidiBytes& mb, const std::vector<std::string>& tokens, size_t idx)
{
    if (idx     < tokens.size()) mb.b0 = ParseHex(tokens[idx]);
    if (idx + 1 < tokens.size()) mb.b1 = ParseHex(tokens[idx + 1]);
    if (idx + 2 < tokens.size()) mb.b2 = ParseHex(tokens[idx + 2]);
}

static bool IsFeedbackKeyword(const std::string& kw)
{
    return kw.size() > 3 && kw[0] == 'F' && kw[1] == 'B' && kw[2] == '_';
}

static bool IsSpecialKeyword(const std::string& kw)
{
    return kw == "Touch" || kw == "Toggle" || kw == "WidgetEnd";
}

// ---------------------------------------------------------------------------
// ParseSurfaceFile
// ---------------------------------------------------------------------------

Surface ParseSurfaceFile(const std::string& filePath)
{
    Surface surf;
    surf.filePath = filePath;

    // Derive name from parent directory (the surface folder)
    {
        std::string path = filePath;
        // Replace backslashes
        for (char& c : path) if (c == '\\') c = '/';
        size_t slash = path.rfind('/');
        std::string dir;
        if (slash != std::string::npos)
            dir = path.substr(0, slash);
        size_t dirSlash = dir.rfind('/');
        surf.name = (dirSlash != std::string::npos) ? dir.substr(dirSlash + 1) : dir;
    }

    std::ifstream f(filePath);
    if (!f.is_open())
        return surf;

    bool inWidget   = false;
    bool inHeader   = true; // accumulate non-Widget lines before first Widget block
    Widget current;
    std::ostringstream headerBuf;

    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = Trim(line);

        // Strip trailing comments from non-comment lines
        std::string noComment = trimmed;
        {
            size_t cpos = noComment.find("//");
            // Only strip if not at the very start (// starts a comment line)
            if (cpos != std::string::npos && cpos > 0)
                noComment = Trim(noComment.substr(0, cpos));
        }

        auto tokens = Tokenize(noComment);

        if (tokens.empty() || trimmed[0] == '/')
        {
            // Comment or blank line
            if (inHeader)
                headerBuf << line << "\n";
            continue;
        }

        if (tokens[0] == "Widget")
        {
            inHeader = false;
            inWidget = true;
            current  = Widget{};
            if (tokens.size() > 1)
                current.name = tokens[1];
            continue;
        }

        if (tokens[0] == "WidgetEnd")
        {
            if (inWidget)
                surf.widgets.push_back(current);
            inWidget = false;
            current  = Widget{};
            continue;
        }

        if (!inWidget)
        {
            headerBuf << line << "\n";
            continue;
        }

        // Inside a Widget block
        if (tokens[0] == "Touch")
        {
            current.hasTouch = true;
            FillBytes(current.touchOn,  tokens, 1);
            FillBytes(current.touchOff, tokens, 4); // second triple optional
            continue;
        }

        if (tokens[0] == "Toggle")
        {
            current.hasToggle = true;
            FillBytes(current.toggleBytes, tokens, 1);
            continue;
        }

        if (IsFeedbackKeyword(tokens[0]))
        {
            FeedbackProcessor fp;
            fp.keyword = tokens[0];
            FillBytes(fp.bytes, tokens, 1);
            current.feedbackProcs.push_back(fp);
            continue;
        }

        // Otherwise treat as a generator
        MidiGenerator gen;
        gen.keyword = tokens[0];
        FillBytes(gen.bytes, tokens, 1);
        if (tokens.size() >= 7) // has extra bytes
        {
            gen.hasExtra = true;
            FillBytes(gen.extraBytes, tokens, 4);
        }
        current.generators.push_back(gen);
    }

    // If file ended mid-widget, save what we have
    if (inWidget)
        surf.widgets.push_back(current);

    surf.headerBlock = headerBuf.str();

    // Determine channelCount heuristically from longest numeric suffix
    int maxNum = 0;
    for (auto& w : surf.widgets)
    {
        const std::string& n = w.name;
        size_t i = n.size();
        while (i > 0 && std::isdigit((unsigned char)n[i - 1]))
            --i;
        if (i < n.size())
        {
            int num = std::stoi(n.substr(i));
            if (num > maxNum) maxNum = num;
        }
    }
    if (maxNum > 0)
        surf.channelCount = maxNum;

    // -----------------------------------------------------------------------
    // Strip-aware grid assignment
    // If the surface has >= 2 numbered channels, lay widgets out as channel
    // strips: numbered widget N goes to column (N-1), row determined by the
    // control type (display on top, fader at bottom).  Non-numbered widgets
    // are spread into extra columns to the right.
    // Otherwise fall back to a simple left-to-right, top-to-bottom layout.
    // -----------------------------------------------------------------------
    bool doStripLayout = (maxNum >= 2);

    if (doStripLayout)
    {
        int n = (int)surf.widgets.size();
        std::vector<int> widgetCol(n, -1);
        std::vector<int> widgetPri(n, 45);

        // Pass 1: assign column from trailing channel number, row priority from name/type
        for (int i = 0; i < n; ++i)
        {
            const Widget& w = surf.widgets[i];
            const std::string& nm = w.name;
            size_t j = nm.size();
            while (j > 0 && std::isdigit((unsigned char)nm[j - 1]))
                --j;

            widgetPri[i] = RowPriority(w);

            if (j < nm.size())
                widgetCol[i] = std::stoi(nm.substr(j)) - 1; // 0-based
            // else stays -1 (non-numbered)
        }

        // Pass 2: collect distinct priorities from NUMBERED (strip) widgets only
        std::set<int> stripPris;
        for (int i = 0; i < n; ++i)
            if (widgetCol[i] >= 0)
                stripPris.insert(widgetPri[i]);

        // Map priority → compact row index (sorted ascending = display first)
        std::map<int, int> priToRow;
        int rowIdx = 0;
        for (int p : stripPris)
            priToRow[p] = rowIdx++;

        // Build rowHeights and rowPriorities arrays for strip rows
        int numStripRows = rowIdx;
        surf.rowHeights.resize(numStripRows);
        surf.rowPriorities.resize(numStripRows);
        for (auto& [p, r] : priToRow)
        {
            surf.rowPriorities[r] = p;
            if (p == 0)       surf.rowHeights[r] = 32;  // display / VU
            else if (p == 60) surf.rowHeights[r] = 180; // fader
            else              surf.rowHeights[r] = 44;  // buttons / encoders
        }

        int gridCols = std::max(maxNum, 8);

        // Assign strip widget positions (numbered; duplicates share a cell intentionally)
        for (int i = 0; i < n; ++i)
        {
            if (widgetCol[i] >= 0)
            {
                int p   = widgetPri[i];
                int row = priToRow.count(p) ? priToRow.at(p) : numStripRows - 1;
                surf.widgets[i].gridCol = widgetCol[i];
                surf.widgets[i].gridRow = row;
            }
        }

        // Non-numbered widgets → extra rows at the BOTTOM, grouped by priority
        std::map<int, std::vector<int>> nonNumGroups;
        for (int i = 0; i < n; ++i)
            if (widgetCol[i] == -1)
                nonNumGroups[widgetPri[i]].push_back(i);

        int extraRow = numStripRows;
        for (auto& [p, indices] : nonNumGroups)
        {
            int col = 0;
            surf.rowHeights.push_back(p == 60 ? 180 : 44);
            surf.rowPriorities.push_back(p);
            for (int idx : indices)
            {
                if (col >= gridCols)
                {
                    col = 0;
                    ++extraRow;
                    surf.rowHeights.push_back(p == 60 ? 180 : 44);
                    surf.rowPriorities.push_back(p);
                }
                surf.widgets[idx].gridCol = col;
                surf.widgets[idx].gridRow = extraRow;
                ++col;
            }
            ++extraRow;
        }

        surf.gridCols = gridCols;
        surf.gridRows = extraRow > numStripRows ? extraRow : numStripRows;
    }
    else
    {
        // Sequential left-to-right, top-to-bottom layout
        surf.gridCols = std::max(8, surf.channelCount);
        int col = 0, row = 0;
        for (auto& w : surf.widgets)
        {
            w.gridCol = col;
            w.gridRow = row;
            ++col;
            if (col >= surf.gridCols)
            {
                col = 0;
                ++row;
            }
        }
        surf.gridRows = std::max(1, row + (col > 0 ? 1 : 0));
        surf.rowHeights.assign(surf.gridRows, 44);
        surf.rowPriorities.assign(surf.gridRows, 45);
    }

    return surf;
}

// ---------------------------------------------------------------------------
// WriteSurfaceFile
// ---------------------------------------------------------------------------

static std::string HexByte(uint8_t b)
{
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02x", b);
    return buf;
}

static std::string FormatBytes(const MidiBytes& mb)
{
    return HexByte(mb.b0) + " " + HexByte(mb.b1) + " " + HexByte(mb.b2);
}

bool WriteSurfaceFile(const Surface& surf)
{
    std::ofstream f(surf.filePath);
    if (!f.is_open())
        return false;

    // Write preserved header block (comments, StepSize, AccelerationValues, etc.)
    if (!surf.headerBlock.empty())
        f << surf.headerBlock;

    for (const auto& w : surf.widgets)
    {
        f << "Widget " << w.name << "\n";

        for (const auto& gen : w.generators)
        {
            f << "    " << gen.keyword << " " << FormatBytes(gen.bytes);
            if (gen.hasExtra)
                f << " " << FormatBytes(gen.extraBytes);
            f << "\n";
        }

        if (w.hasTouch)
        {
            f << "    Touch " << FormatBytes(w.touchOn);
            // Check if off bytes are non-zero
            if (w.touchOff.b0 || w.touchOff.b1 || w.touchOff.b2)
                f << " " << FormatBytes(w.touchOff);
            f << "\n";
        }

        if (w.hasToggle)
            f << "    Toggle " << FormatBytes(w.toggleBytes) << "\n";

        for (const auto& fp : w.feedbackProcs)
            f << "    " << fp.keyword << " " << FormatBytes(fp.bytes) << "\n";

        f << "WidgetEnd\n\n";
    }

    return true;
}

// ---------------------------------------------------------------------------
// WidgetType inference
// ---------------------------------------------------------------------------

WidgetType InferWidgetType(const Widget& w)
{
    for (const auto& gen : w.generators)
    {
        const std::string& kw = gen.keyword;
        if (kw.find("Fader") != std::string::npos)
            return WidgetType::Fader;
        if (kw.find("Encoder") != std::string::npos)
            return WidgetType::Encoder;
        if (kw == "Press" || kw == "AnyPress")
            return WidgetType::Button;
    }
    for (const auto& fp : w.feedbackProcs)
    {
        const std::string& kw = fp.keyword;
        if (kw.find("Display") != std::string::npos)
            return WidgetType::Display;
        if (kw.find("VUMeter") != std::string::npos || kw.find("VU") != std::string::npos)
            return WidgetType::VUMeter;
    }
    // No generators but has feedback → output-only widget (display / VU)
    if (w.generators.empty() && !w.feedbackProcs.empty())
        return WidgetType::Display;
    return WidgetType::Unknown;
}

// ---------------------------------------------------------------------------
// Row priority for strip-aware grid assignment
//   Lower value = higher row on the canvas (displays at top, fader at bottom)
// ---------------------------------------------------------------------------

static int RowPriority(const Widget& w)
{
    const std::string& nm = w.name;
    std::string lower = nm;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto has = [&](const char* s) -> bool {
        return lower.find(s) != std::string::npos;
    };

    WidgetType wt = InferWidgetType(w);

    if (wt == WidgetType::Display || wt == WidgetType::VUMeter
        || has("display") || has("meter") || has("vu") || has("time") || has("assignment"))
        return 0;

    if (wt == WidgetType::Encoder || has("encoder") || has("pan") || has("knob"))
        return 10;

    if (has("recordarm") || has("recarm") || has("rec arm") || has("arm"))
        return 20;

    if (has("solo"))
        return 30;

    if (has("mute"))
        return 40;

    if (has("select"))
        return 50;

    if (wt == WidgetType::Fader || has("fader"))
        return 60;

    return 45; // unknown button
}

const char* WidgetTypeName(WidgetType t)
{
    switch (t)
    {
    case WidgetType::Button:  return "Button";
    case WidgetType::Fader:   return "Fader";
    case WidgetType::Encoder: return "Encoder";
    case WidgetType::Display: return "Display";
    case WidgetType::VUMeter: return "VUMeter";
    default:                  return "Unknown";
    }
}

} // namespace SurfaceEditor
