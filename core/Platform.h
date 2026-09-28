#pragma once

// ---------------------------------------------------------------------------
// Platform.h – small Win32 calls SWELL does not provide, so the same source
// builds on macOS. On Windows everything here is the real API.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#  include <chrono>
#  include <cstring>
#endif

// Fill a rectangle with one colour. Leaves the DC's background colour set to
// it, as the SetBkColor + ExtTextOut(ETO_OPAQUE) idiom this replaces did.
static inline void FillSolid(HDC hdc, const RECT* rc, COLORREF c)
{
    SetBkColor(hdc, c);
#ifdef _WIN32
    ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, rc, "", 0, nullptr);
#else
    HBRUSH br = CreateSolidBrush(c);
    FillRect(hdc, rc, br);
    DeleteObject(br);
#endif
}

#ifndef _WIN32
// High-resolution counter, in nanoseconds.
typedef union { struct { DWORD LowPart; LONG HighPart; }; long long QuadPart; } LARGE_INTEGER;

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f)
{
    f->QuadPart = 1000000000LL;
    return TRUE;
}

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c)
{
    c->QuadPart = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return TRUE;
}

static inline BOOL GetTextExtentPoint32A(HDC hdc, const char* s, int n, SIZE* sz)
{
    RECT r = { 0, 0, 0, 0 };
    DrawText(hdc, s, n, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    sz->cx = r.right - r.left;
    sz->cy = r.bottom - r.top;
    return TRUE;
}

#  ifndef _stricmp
#    define _stricmp stricmp
#  endif

// SWELL is UTF-8 throughout and only has the undecorated names.
#  define MessageBoxA             MessageBox
#  define DrawTextA               DrawText
#  define SetWindowTextA          SetWindowText
#  define GetWindowTextA          GetWindowText
#  define SetDlgItemTextA         SetDlgItemText
#  define GetDlgItemTextA         GetDlgItemText
#  define CreateFontA             CreateFont
#  define CreateFontIndirectA     CreateFontIndirect
#  define LOGFONTA                LOGFONT
#  define DialogBoxA              DialogBox
#  define DialogBoxParamA         DialogBoxParam
#  define CreateDialogParamA      CreateDialogParam
#  define MAKEINTRESOURCEA        MAKEINTRESOURCE
#  define DefWindowProcA          DefWindowProc
#  define SendMessageA            SendMessage
#  define GetClassNameA           GetClassName
#  define GetPrivateProfileIntA   GetPrivateProfileInt
#  define GetPrivateProfileStringA GetPrivateProfileString

static inline BOOL AppendMenuA(HMENU h, UINT flags, UINT_PTR id, const char* str)
{
    InsertMenu(h, GetMenuItemCount(h), flags | MF_BYPOSITION, id, str);
    return TRUE;
}

static inline BOOL TextOutA(HDC hdc, int x, int y, const char* s, int n)
{
    RECT r = { x, y, x, y };
    DrawText(hdc, s, n, &r, DT_NOCLIP | DT_SINGLELINE | DT_NOPREFIX);
    return TRUE;
}

// Child controls are built through SWELL's control factory, which knows the
// stock classes (list view, progress, edit, static, button, combo). Classes
// it has no equivalent for (scrollbar, tooltip) come back null, and callers
// already treat a null control as "not available".
static inline HWND CreateWindowExA(DWORD exStyle, const char* cls, const char* title,
                                   DWORD style, int x, int y, int w, int h,
                                   HWND parent, HMENU id, HINSTANCE, void*)
{
    if (!cls || !parent) return nullptr;
    if (!stricmp(cls, "msctls_progressbar32")) cls = "msctls_progress32";
    if (!stricmp(cls, "SCROLLBAR") || !stricmp(cls, "tooltips_class32")) return nullptr;

    int st = (int)(style & ~WS_VISIBLE);
    if (!(style & WS_VISIBLE)) st |= SWELL_NOT_WS_VISIBLE;

    SWELL_MakeSetCurParms(1.0f, 1.0f, 0, 0, parent, false, false);
    HWND hwnd = SWELL_MakeControl(title ? title : "", (int)(INT_PTR)id, cls,
                                  st, x, y, w, h, (int)exStyle);
    SWELL_MakeSetCurParms(1.0f, 1.0f, 0, 0, nullptr, false, false);
    return hwnd;
}
#endif
