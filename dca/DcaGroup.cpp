// ---------------------------------------------------------------------------
// DcaGroup.cpp  –  DCA group data model + RPP serialization
// ---------------------------------------------------------------------------
#include "DcaGroup.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// GUID helpers (Windows-only; matches TransitionSnapshot.cpp pattern)
// ---------------------------------------------------------------------------
static std::string GuidToStr(const GUID& g)
{
    WCHAR wbuf[64];
    StringFromGUID2(g, wbuf, 64);
    char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 64, nullptr, nullptr);
    return buf;
}

static GUID StrToGuid(const char* s)
{
    GUID g = {};
    if (!s || !s[0]) return g;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return g;
    std::vector<WCHAR> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wbuf.data(), wlen);
    CLSIDFromString(wbuf.data(), &g);
    return g;
}

// ---------------------------------------------------------------------------
// Serialize
// Format:
//   <DCAGROUP groupNum flags {GUID} "name"
//   >
// ---------------------------------------------------------------------------
void DcaGroup::Serialize(ProjectStateContext* ctx) const
{
    std::string sguid = GuidToStr(trackGuid);

    // Escape double-quotes in the name
    std::string safeName = name;
    for (char& c : safeName) if (c == '"') c = '\'';

    ctx->AddLine("<DCAGROUP %d %u %s \"%s\"",
                 groupNum, flags, sguid.c_str(), safeName.c_str());
    ctx->AddLine(">");
}

// ---------------------------------------------------------------------------
// Deserialize
// headerLine = "<DCAGROUP groupNum flags {GUID} \"name\""
// Reads child lines until '>'.
// ---------------------------------------------------------------------------
DcaGroup* DcaGroup::Deserialize(const char* headerLine,
                                 ProjectStateContext* ctx)
{
    // headerLine arrives with the leading '<' intact (same as LTLAYOUT pattern)
    int          groupNum = 0;
    unsigned int flags    = 0;
    char         sguid[64] = {};

    if (sscanf(headerLine, "<DCAGROUP %d %u %63s", &groupNum, &flags, sguid) < 3)
        return nullptr;

    // Extract quoted name (may contain spaces)
    char name[512] = {};
    const char* q = strchr(headerLine, '"');
    if (q)
    {
        q++;
        int i = 0;
        while (*q && *q != '"' && i < (int)sizeof(name) - 1)
            name[i++] = *q++;
        name[i] = '\0';
    }

    auto* dca       = new DcaGroup();
    dca->groupNum   = groupNum;
    dca->flags      = (uint32_t)flags;
    dca->trackGuid  = StrToGuid(sguid);
    dca->name       = name;

    // Drain child lines until '>'
    char line[4096];
    while (ctx->GetLine(line, sizeof(line)) == 0)
    {
        char* t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (strcmp(t, ">") == 0) break;
        // No child lines currently; reserved for future expansion
    }

    return dca;
}
