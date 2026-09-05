#include "RecallLog.h"
#include "../api.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const size_t kMaxLogBytes  = 4 * 1024 * 1024;  // rotate past this
static const size_t kReserveBytes = 64 * 1024;        // typical trace size

static bool        s_active = false;
static std::string s_buf;
static std::string s_path;

#ifdef _WIN32
static const char kSep = '\\';
#else
static const char kSep = '/';
#endif

// ---------------------------------------------------------------------------
// Path resolution (cached - the resource path cannot change mid-session)
// ---------------------------------------------------------------------------
static const std::string& LogPath()
{
    if (s_path.empty() && GetResourcePath)
    {
        const char* res = GetResourcePath();
        if (res && *res)
        {
            s_path = res;
            if (!s_path.empty() && s_path.back() != kSep && s_path.back() != '/')
                s_path += kSep;
            s_path += "live_tools_recall.log";
        }
    }
    return s_path;
}

const char* RecallLog_Path() { return LogPath().c_str(); }

bool RecallLog_Active() { return s_active; }

// ---------------------------------------------------------------------------
// Trace lifecycle
// ---------------------------------------------------------------------------
void RecallLog_Begin(const char* sceneName, int mask, double duration)
{
    s_active = false;
    if (!g_recallLog) return;
    if (LogPath().empty()) return;   // no resource path - nothing we can write

    s_active = true;
    s_buf.clear();
    s_buf.reserve(kReserveBytes);

    char stamp[64] = "?";
    time_t     now = time(nullptr);
    struct tm* lt  = localtime(&now);
    if (lt) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", lt);

    RecallLog_Printf("========================================================");
    RecallLog_Printf("RECALL  %s  scene=\"%s\"  mask=0x%X  duration=%.3fs",
                     stamp, sceneName ? sceneName : "(unnamed)", mask, duration);
}

void RecallLog_Printf(const char* fmt, ...)
{
    if (!s_active || !fmt) return;

    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if (n >= (int)sizeof(line))       // truncated - mark it so we can tell
    {
        line[sizeof(line) - 4] = '.';
        line[sizeof(line) - 3] = '.';
        line[sizeof(line) - 2] = '.';
        line[sizeof(line) - 1] = '\0';
    }
    s_buf += line;
    s_buf += '\n';
}

// ---------------------------------------------------------------------------
// Rotation: move the current file aside once it grows past kMaxLogBytes.
// ---------------------------------------------------------------------------
static void RotateIfNeeded(const std::string& path)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return;                       // nothing there yet
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    if (sz < 0 || (size_t)sz < kMaxLogBytes) return;

    std::string old = path + ".1";
    remove(old.c_str());                   // rename() won't clobber on Windows
    rename(path.c_str(), old.c_str());
}

void RecallLog_End()
{
    if (!s_active) return;
    s_active = false;
    if (s_buf.empty()) return;

    const std::string& path = LogPath();
    if (path.empty()) { s_buf.clear(); return; }

    RotateIfNeeded(path);

    FILE* fp = fopen(path.c_str(), "ab");
    if (fp)
    {
        fwrite(s_buf.data(), 1, s_buf.size(), fp);
        fclose(fp);
    }
    // A failed write is deliberately silent: diagnostics must never interrupt
    // a live recall with a dialog.
    s_buf.clear();
    s_buf.shrink_to_fit();
}
