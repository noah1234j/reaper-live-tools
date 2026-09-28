#pragma once

// ---------------------------------------------------------------------------
// GUID <-> string, portable (Windows + SWELL).
// Same "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" uppercase format that
// StringFromGUID2 produced, so strings saved by older builds still parse.
// Parsing accepts the braces as optional; a malformed string yields a
// zeroed GUID, as CLSIDFromString did.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
#else
#include "WDL/swell/swell.h"
#endif

#include <cstdio>
#include <string>

inline std::string GuidToString(const GUID& g)
{
    char buf[40];
    snprintf(buf, sizeof(buf),
        "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        (unsigned)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
        (unsigned)g.Data4[0], (unsigned)g.Data4[1],
        (unsigned)g.Data4[2], (unsigned)g.Data4[3],
        (unsigned)g.Data4[4], (unsigned)g.Data4[5],
        (unsigned)g.Data4[6], (unsigned)g.Data4[7]);
    return buf;
}

inline GUID StringToGuid(const char* s)
{
    GUID g = {};
    if (!s) return g;
    while (*s == ' ' || *s == '\t') ++s;
    const char* p = (*s == '{') ? s + 1 : s;
    unsigned d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7;
    if (sscanf(p, "%8X-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X",
               &d1, &d2, &d3, &b0, &b1, &b2, &b3, &b4, &b5, &b6, &b7) != 11)
        return g;
    g.Data1    = (DWORD)d1;
    g.Data2    = (WORD)d2;
    g.Data3    = (WORD)d3;
    g.Data4[0] = (BYTE)b0; g.Data4[1] = (BYTE)b1;
    g.Data4[2] = (BYTE)b2; g.Data4[3] = (BYTE)b3;
    g.Data4[4] = (BYTE)b4; g.Data4[5] = (BYTE)b5;
    g.Data4[6] = (BYTE)b6; g.Data4[7] = (BYTE)b7;
    return g;
}
