#include "MusicSheet.h"
#include "api.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

// ---------------------------------------------------------------------------
// Palette
//
// Six pens, chosen to stay apart from each other and off the black of printed
// music under stage light. Red first because it is what everyone reaches for.
// ---------------------------------------------------------------------------
unsigned int MusicSheet_PaletteColor(int idx)
{
    static const unsigned int k[kSheetPaletteCount] = {
        0xD42A2A,  // red
        0x1F6FD0,  // blue
        0x1E9E4A,  // green
        0xE08A00,  // amber
        0x8B46C8,  // violet
        0x202020,  // near-black, for marks meant to read as part of the score
    };
    if (idx < 0 || idx >= kSheetPaletteCount) idx = 0;
    return k[idx];
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
SheetPage& MusicSheet::Page(int idx)
{
    if (idx < 0) idx = 0;
    if (idx >= (int)pages.size()) pages.resize((size_t)idx + 1);
    return pages[(size_t)idx];
}

const SheetPage* MusicSheet::PageOrNull(int idx) const
{
    if (idx < 0 || idx >= (int)pages.size()) return nullptr;
    return &pages[(size_t)idx];
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
#ifdef _WIN32
static const char kSep = '\\';
static bool SameChar(char a, char b)
{
    // Windows paths are case-insensitive, and a project folder written with a
    // forward slash still names the same place.
    if (a == '/') a = '\\';
    if (b == '/') b = '\\';
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}
#else
static const char kSep = '/';
static bool SameChar(char a, char b) { return a == b; }
#endif

static bool IsAbsolute(const char* p)
{
    if (!p || !*p) return false;
#ifdef _WIN32
    if (p[0] == '\\' || p[0] == '/') return true;                 // \\server\share, /x
    if (p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true; // C:\x
    return false;
#else
    return p[0] == '/';
#endif
}

// The project's folder, without a trailing separator. Empty for a project
// that has never been saved — which is exactly when there is nothing to be
// relative to, so everything stays absolute.
//
// From the .rpp's own path, not GetProjectPath: that is the recording path,
// which is the project's Media subfolder on most setups, so a chart kept
// beside the .rpp would have been stored absolute.
std::string MusicSheet_ProjectDir()
{
    char buf[4096] = {};
    if (EnumProjects) EnumProjects(-1, buf, (int)sizeof(buf));
    std::string s = buf;
    const size_t cut = s.find_last_of("\\/");
    if (cut == std::string::npos) return std::string();
    s.resize(cut);
    return s;
}

static std::string FileNameOf(const std::string& path)
{
    size_t cut = std::string::npos;
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == '\\' || path[i] == '/') cut = i;
    return (cut == std::string::npos) ? path : path.substr(cut + 1);
}

void MusicSheet_SetPath(MusicSheet& sheet, const char* absPath)
{
    if (!absPath || !*absPath) { sheet.Clear(); return; }

    const std::string abs = absPath;
    sheet.displayName = FileNameOf(abs);

    // Inside the project folder, store it relative: a project folder copied
    // to the other machine at the venue takes its charts with it. Outside it
    // (a scores folder shared by every show), absolute is the only form that
    // keeps meaning.
    const std::string dir = MusicSheet_ProjectDir();
    if (!dir.empty() && abs.size() > dir.size() + 1)
    {
        bool under = true;
        for (size_t i = 0; i < dir.size() && under; ++i)
            if (!SameChar(dir[i], abs[i])) under = false;
        const char after = abs[dir.size()];
        if (under && (after == '\\' || after == '/'))
        {
            sheet.path = abs.substr(dir.size() + 1);
            return;
        }
    }
    sheet.path = abs;
}

std::string MusicSheet_Resolve(const MusicSheet& sheet)
{
    if (sheet.path.empty()) return std::string();
    if (IsAbsolute(sheet.path.c_str())) return sheet.path;

    const std::string dir = MusicSheet_ProjectDir();
    if (dir.empty()) return sheet.path;   // nothing to resolve against
    return dir + kSep + sheet.path;
}

bool MusicSheet_Exists(const MusicSheet& sheet)
{
    const std::string full = MusicSheet_Resolve(sheet);
    if (full.empty()) return false;
#ifdef _WIN32
    // REAPER's paths are UTF-8; the A form would read them in the ANSI code
    // page and miss any file with an accent in its name.
    wchar_t wbuf[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, wbuf, 2048) <= 0) return false;
    const DWORD a = GetFileAttributesW(wbuf);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
#endif
}

bool MusicSheet_IsPdf(const MusicSheet& sheet)
{
    const std::string& p = sheet.path;
    if (p.size() < 4) return false;
    const char* ext = p.c_str() + p.size() - 4;
    return (ext[0] == '.') &&
           (ext[1] == 'p' || ext[1] == 'P') &&
           (ext[2] == 'd' || ext[2] == 'D') &&
           (ext[3] == 'f' || ext[3] == 'F');
}

// ---------------------------------------------------------------------------
// Persistence
//
// Flat lines, each one self-contained and naming the page it belongs to, so
// the loader stays the keyword chain TransitionSnapshot::Deserialize already
// is - no nesting, no parse state to carry between lines.
//
// A stroke writes its points inline. They are decimated when they are drawn
// (see the viewer), so a stroke is tens of points rather than the hundreds a
// raw mouse-move trail would give, and the line stays a sane length.
// ---------------------------------------------------------------------------

// Quote-delimited field, escaped the way NOTES is: a quote becomes an
// apostrophe since the quote closes the field, and backslash and newline are
// escaped so the unescape is unambiguous.
static std::string Escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in)
    {
        switch (c)
        {
        case '"':  out += '\'';   break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r':                break;
        default:   out += c;      break;
        }
    }
    return out;
}

// Reads the quoted field starting at the first quote in `p`; leaves `p` just
// past the closing quote. Returns false when there is no field there.
static bool ReadQuoted(const char*& p, std::string& out)
{
    const char* q = strchr(p, '"');
    if (!q) return false;
    ++q;
    out.clear();
    while (*q && *q != '"')
    {
        if (*q == '\\' && q[1] == 'n')       { out += '\n'; q += 2; }
        else if (*q == '\\' && q[1] == '\\') { out += '\\'; q += 2; }
        else                                 { out += *q++; }
    }
    if (*q == '"') ++q;
    p = q;
    return true;
}

void MusicSheet_Serialize(const MusicSheet& sheet, ProjectStateContext* ctx)
{
    if (!ctx) return;
    // Nothing attached and nothing drawn: write nothing at all, so a project
    // that does not use sheets is byte-identical to what older builds wrote.
    if (!sheet.Attached() && !sheet.HasAnnotations()) return;

    if (sheet.Attached())
        ctx->AddLine("SHEET \"%s\" \"%s\"",
                     Escape(sheet.path).c_str(),
                     Escape(sheet.displayName).c_str());

    for (int pi = 0; pi < (int)sheet.pages.size(); ++pi)
    {
        const SheetPage& pg = sheet.pages[(size_t)pi];

        for (const auto& st : pg.strokes)
        {
            if (st.pts.empty()) continue;
            std::string line;
            char head[96];
            snprintf(head, sizeof(head), "SHEETSTROKE %d %d %.5f %d",
                     pi, st.colorIdx, st.width, (int)st.pts.size());
            line = head;
            for (const auto& pt : st.pts)
            {
                char xy[48];
                snprintf(xy, sizeof(xy), " %.5f %.5f", pt.x, pt.y);
                line += xy;
            }
            ctx->AddLine("%s", line.c_str());
        }

        for (const auto& tx : pg.texts)
        {
            if (tx.text.empty()) continue;
            {
            // The angle goes after the text, and only when there is one:
            // an older build reads up to the closing quote and stops.
            if (tx.angle != 0.0f)
                ctx->AddLine("SHEETTEXT %d %d %.5f %.5f %.5f \"%s\" %.2f",
                             pi, tx.colorIdx, tx.height, tx.pos.x, tx.pos.y,
                             Escape(tx.text).c_str(), tx.angle);
            else
                ctx->AddLine("SHEETTEXT %d %d %.5f %.5f %.5f \"%s\"",
                             pi, tx.colorIdx, tx.height, tx.pos.x, tx.pos.y,
                             Escape(tx.text).c_str());
        }
        }
    }
}

bool MusicSheet_ProcessLine(MusicSheet& sheet, const char* line)
{
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;

    if (strncmp(line, "SHEET \"", 7) == 0)
    {
        const char* p = line + 5;
        std::string path, name;
        if (!ReadQuoted(p, path)) return true;   // ours, but malformed
        ReadQuoted(p, name);
        sheet.path        = path;
        sheet.displayName = name.empty() ? FileNameOf(path) : name;
        return true;
    }

    if (strncmp(line, "SHEETSTROKE ", 12) == 0)
    {
        const char* p = line + 12;
        int page = 0, colorIdx = 0, n = 0;
        float width = 0.004f;
        if (sscanf(p, "%d %d %f %d", &page, &colorIdx, &width, &n) != 4) return true;

        // Step past the four header fields to the first coordinate.
        int seen = 0;
        while (*p && seen < 4)
        {
            while (*p == ' ') ++p;
            if (!*p) break;
            while (*p && *p != ' ') ++p;
            ++seen;
        }

        SheetStroke st;
        st.colorIdx = colorIdx;
        st.width    = (width > 0.0f) ? width : 0.004f;
        if (n > 0 && n < 100000) st.pts.reserve((size_t)n);
        for (int i = 0; i < n; ++i)
        {
            float x = 0.0f, y = 0.0f;
            char* end = nullptr;
            x = strtof(p, &end);
            if (end == p) break;
            p = end;
            y = strtof(p, &end);
            if (end == p) break;
            p = end;
            st.pts.push_back({ x, y });
        }
        if (!st.pts.empty()) sheet.Page(page).strokes.push_back(st);
        return true;
    }

    if (strncmp(line, "SHEETTEXT ", 10) == 0)
    {
        const char* p = line + 10;
        int page = 0, colorIdx = 0;
        float h = 0.025f, x = 0.0f, y = 0.0f;
        if (sscanf(p, "%d %d %f %f %f", &page, &colorIdx, &h, &x, &y) != 5) return true;

        std::string text;
        if (!ReadQuoted(p, text) || text.empty()) return true;

        SheetText tx;
        tx.colorIdx = colorIdx;
        tx.height   = (h > 0.0f) ? h : 0.025f;
        tx.pos      = { x, y };
        tx.text     = text;
        // Optional, after the closing quote (see Serialize).
        float angle = 0.0f;
        if (sscanf(p, " %f", &angle) == 1 && std::isfinite(angle))
            tx.angle = std::fmod(angle, 360.0f);
        sheet.Page(page).texts.push_back(tx);
        return true;
    }

    return false;
}
