#pragma once

// ---------------------------------------------------------------------------
// MusicSheetRender - turning an attached file into page bitmaps.
//
// Two sources, one result. An image is one page, decoded with GDI+. A PDF is
// as many pages as it has, rasterised by Windows' own PDF renderer
// (Windows.Data.Pdf, in the OS since 8.1) - no vendored engine, nothing added
// to the binary, and nothing for the user to install.
//
// All of it happens on a worker thread. Rasterising a two-page score is tens
// of milliseconds on a good day and a visible stall on a bad one, and this
// plugin's whole reason for existing is that nothing blocks the main thread
// during a show. The window asks for a document, carries on drawing, and
// gets a message when the pages are ready.
//
// Pages are rendered once at a generous width and scaled down when drawn,
// rather than re-rendered on every resize: a sheet is read, not zoomed into
// repeatedly, and a re-render per WM_SIZE would be the one expensive thing in
// a window that otherwise costs nothing.
// ---------------------------------------------------------------------------

#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif

#include <string>
#include <vector>

// Width every page is rasterised at. A 4K display showing one page full
// height wants about this much; beyond it the memory costs more than the
// sharpness is worth (a page is ~16MB at 32bpp as it is).
static const int kSheetRenderWidth = 1700;

// ---------------------------------------------------------------------------
// SheetDoc - the rendered pages of one attached file.
//
// Owned by whoever asked for it; free it with MusicSheetRender_Free. Only one
// is normally alive at a time (the sheet currently on screen).
// ---------------------------------------------------------------------------
struct SheetDocPage
{
    HBITMAP bmp = nullptr;   // 32-bit top-down DIB, kSheetRenderWidth wide
    int     w   = 0;
    int     h   = 0;
};

struct SheetDoc
{
    std::string               path;     // what was rendered, resolved absolute
    std::vector<SheetDocPage> pages;
    std::string               error;    // empty on success

    bool Ok() const { return error.empty() && !pages.empty(); }
    int  PageCount() const { return (int)pages.size(); }
};

// ---------------------------------------------------------------------------
// Asynchronous load.
//
// Starts a worker and returns immediately. When it finishes, `notifyWnd` is
// sent `notifyMsg` with lParam set to the SheetDoc*, which the window then
// owns and must free. A failed load still arrives, carrying `error` - the
// window says why rather than showing an empty frame.
//
// A second request supersedes the first: the earlier document is dropped when
// it lands rather than posted, so flipping through scenes quickly cannot pile
// up windows' worth of bitmaps or deliver pages for a scene already gone.
// ---------------------------------------------------------------------------
void MusicSheetRender_LoadAsync(const char* absPath, HWND notifyWnd, UINT notifyMsg);

// Drop any in-flight request. The worker still finishes, but its result is
// thrown away instead of posted. Call before the window closes.
void MusicSheetRender_Cancel();

void MusicSheetRender_Free(SheetDoc* doc);

// Start the imaging stack (GDI+) if it is not running. Main thread only:
// LoadAsync calls it before handing off, so the workers never race to start
// it, and the viewer calls it before drawing annotations with it. Returns
// false when it cannot be started; callers fall back to plain GDI.
bool MusicSheetRender_Startup();

// Shut the imaging stack down. Called once from the plugin's cleanup.
void MusicSheetRender_Cleanup();
