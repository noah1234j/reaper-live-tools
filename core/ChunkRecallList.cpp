#include "ChunkRecallList.h"
#include <cstring>
#include <cctype>

// ---------------------------------------------------------------------------
// Default keyword list – pre-loaded on each project reset
// ---------------------------------------------------------------------------
static const std::vector<std::string>& BuildDefaults()
{
    static const std::vector<std::string> k_defaults = {
        "Virtual Mix Rack",  // Waves VMR (fixed 148 params; slot module changes semantics)
        "StudioRack",        // Waves StudioRack (same architecture)
        "Scheps Omni",       // Scheps Omni Channel (rack-style fixed slots)
        "ML4000",            // McDSP ML4000 (multi-module dynamics rack)
        "SSL Native",        // SSL Native Channel Strip / Bus Comp (state not fully param-exposed)
        // NOTE: projects that ever saved settings persist their own
        // LTCHUNKPLUGIN list, which replaces these defaults on load —
        // existing projects must add new keywords via the dialog.
    };
    return k_defaults;
}

// ---------------------------------------------------------------------------
// Live keyword list (project-scoped, all entries editable/serialised)
// ---------------------------------------------------------------------------
std::vector<std::string> g_chunkRecallKeywords;

// Debug notify flag (session-scoped, not persisted)
bool g_chunkRecallNotify = false;

// ---------------------------------------------------------------------------
// GetChunkRecallDefaults
// ---------------------------------------------------------------------------
const std::vector<std::string>& GetChunkRecallDefaults()
{
    return BuildDefaults();
}

// ---------------------------------------------------------------------------
// icontains – portable case-insensitive substring search
// ---------------------------------------------------------------------------
static bool icontains(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !*needle) return false;
    const size_t hlen = strlen(haystack);
    const size_t nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; ++i)
    {
        bool match = true;
        for (size_t j = 0; j < nlen; ++j)
        {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
            {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// IsChunkRecallPlugin
// ---------------------------------------------------------------------------
bool IsChunkRecallPlugin(const char* name)
{
    if (!name || !*name) return false;
    for (const auto& kw : g_chunkRecallKeywords)
        if (icontains(name, kw.c_str())) return true;
    return false;
}
