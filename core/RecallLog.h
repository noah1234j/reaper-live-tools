#pragma once

// ---------------------------------------------------------------------------
// RecallLog - file-based diagnostics for scene recall.
//
// Enabled by the "Write recall log to file" global setting. When on, each
// Recall() accumulates a human-readable trace IN MEMORY and flushes it to
//
//     <REAPER resource path>/live_tools_recall.log
//
// once the recall has finished. Nothing touches the filesystem while the
// recall is running, so enabling the log cannot add latency mid-transition.
//
// The file is appended to and rotated once it passes kMaxLogBytes: the old
// content moves to live_tools_recall.log.1 and a fresh file is started, so a
// long run can never fill the disk.
// ---------------------------------------------------------------------------

// Defined in scenes/TransitionWnd.cpp alongside the other global settings.
extern bool g_recallLog;

// True while a recall is being traced. Callers can use this to skip building
// expensive strings when the log is off.
bool RecallLog_Active();

// Open a trace for one recall. No-op (and leaves RecallLog_Active() false)
// when the setting is off.
void RecallLog_Begin(const char* sceneName, int mask, double duration);

// Append one line. A trailing newline is added automatically. No-op when the
// log is not active.
void RecallLog_Printf(const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Finish the trace and flush it to disk. Safe to call when not active.
void RecallLog_End();

// Absolute path of the log file, or "" if it could not be determined.
// Useful for telling the user where to find it.
const char* RecallLog_Path();
