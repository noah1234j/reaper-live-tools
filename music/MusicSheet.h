#pragma once

// ---------------------------------------------------------------------------
// MusicSheet - a page of music attached to a scene, and what has been drawn
// on it.
//
// A scene holds one sheet: a PDF or an image file, by path rather than
// embedded. A two-megabyte score written into the .RPP would be paid for on
// every project save, and a performer who re-exports the chart wants the new
// one, not a stale copy. The cost is that the file has to stay where it is;
// MusicSheet_Resolve takes that seriously (see below).
//
// Annotations are vector data, not pixels burned into the page. A stroke is
// the points the pencil went through; a note is a string and where it sits.
// Both are stored per page in normalized [0..1] coordinates, so they land in
// the same place on the music whatever size the window is, at whatever zoom,
// on whatever screen - and they survive re-attaching a re-exported chart as
// long as the layout has not moved.
//
// Rasterising is somebody else's thread: see MusicSheetRender.h. Nothing in
// this header touches a file.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

class ProjectStateContext;  // from the REAPER SDK (reaper_plugin.h)

// ---------------------------------------------------------------------------
// The six pen colours, and the same six for text.
//
// An index, not a COLORREF: the palette is a fixed set the UI shows as six
// swatches, and storing the index keeps a project readable if the palette is
// ever retuned for legibility on a dark stage. MusicSheet_PaletteColor maps
// it; an out-of-range index reads as the first colour rather than black-on-
// black.
// ---------------------------------------------------------------------------
static const int kSheetPaletteCount = 6;

// 0xRRGGBB. Picked to stay legible on white paper under stage light, and
// distinct from each other for someone glancing down mid-song.
unsigned int MusicSheet_PaletteColor(int idx);

// ---------------------------------------------------------------------------
// SheetPoint - a point on the page, normalized to the page box.
//
// 0,0 is the top-left of the page image and 1,1 the bottom-right, so a stroke
// drawn in a small window reappears in the right place full-screen. Stored as
// float: a sheet is read at arm's length, and the extra precision of a double
// would only make the project file bigger.
// ---------------------------------------------------------------------------
struct SheetPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

// ---------------------------------------------------------------------------
// SheetStroke - one pencil stroke: everything between button down and up.
// ---------------------------------------------------------------------------
struct SheetStroke
{
    int   colorIdx = 0;      // index into the palette
    float width    = 0.004f; // pen width as a fraction of the page's width,
                             // so it scales with the page like the points do
    std::vector<SheetPoint> pts;
};

// ---------------------------------------------------------------------------
// SheetText - a typed note sitting on the page.
// ---------------------------------------------------------------------------
struct SheetText
{
    SheetPoint  pos;              // top-left of the text
    int         colorIdx = 0;
    float       height   = 0.025f;// character height as a fraction of the page height
    float       angle    = 0.0f;  // degrees, clockwise, about the note's centre
    std::string text;
};

// ---------------------------------------------------------------------------
// SheetPage - the annotations on one page. Pages are addressed by index, and
// a page with nothing on it costs nothing to store.
// ---------------------------------------------------------------------------
struct SheetPage
{
    std::vector<SheetStroke> strokes;
    std::vector<SheetText>   texts;

    bool IsEmpty() const { return strokes.empty() && texts.empty(); }
};

// ---------------------------------------------------------------------------
// MusicSheet - the attachment on one scene.
// ---------------------------------------------------------------------------
struct MusicSheet
{
    // Where the file is. Relative to the project folder when it sits inside
    // it (so a project folder can be copied to another machine whole), and
    // absolute otherwise. MusicSheet_Resolve turns either back into a path to
    // open; MusicSheet_SetPath decides which of the two to store.
    std::string path;

    // What the file was called when it was attached. Shown on the button and
    // in the viewer's title, and still worth having when the file has gone
    // missing - "can't find Verse 2.pdf" beats "can't find the attachment".
    std::string displayName;

    // Annotations, indexed by page. Shorter than the document's page count
    // whenever the later pages are clean, which is the usual case.
    std::vector<SheetPage> pages;

    bool Attached() const { return !path.empty(); }

    bool HasAnnotations() const
    {
        for (const auto& p : pages)
            if (!p.IsEmpty()) return true;
        return false;
    }

    // Annotations only; the attachment stays.
    void ClearAnnotations() { pages.clear(); }

    // Everything: no file, no marks.
    void Clear() { path.clear(); displayName.clear(); pages.clear(); }

    // The page's annotations, grown into existence on demand. Call the const
    // form when only reading - it does not allocate for a clean page.
    SheetPage&       Page(int idx);
    const SheetPage* PageOrNull(int idx) const;
};

// ---------------------------------------------------------------------------
// Paths
//
// Stored project-relative where possible. Resolve is the only thing that
// should ever be handed to a file API.
// ---------------------------------------------------------------------------

// The folder the open project's .rpp is in, without a trailing separator;
// empty for a project that has never been saved.
std::string MusicSheet_ProjectDir();

// Store `absPath`, choosing the relative or absolute form, and set
// displayName from its file name.
void MusicSheet_SetPath(MusicSheet& sheet, const char* absPath);

// The absolute path to open, from whatever form is stored. Empty when
// nothing is attached.
std::string MusicSheet_Resolve(const MusicSheet& sheet);

// True when the attachment resolves to a file that is actually there. A
// missing file is not an error to fix silently: the viewer says so and leaves
// the annotations alone, because the path going stale (a moved folder, a
// project opened on another machine) must not throw away the markings.
bool MusicSheet_Exists(const MusicSheet& sheet);

// Extension test, on the stored path. Rendering a PDF and decoding an image
// are different jobs; this is how the renderer picks.
bool MusicSheet_IsPdf(const MusicSheet& sheet);

// ---------------------------------------------------------------------------
// Persistence - written inside the scene's own TSSNAPSHOT block.
//
// Nothing is written for a scene with no sheet and no marks, so a project
// that does not use the feature is byte-identical to what older builds
// produced, and an older build skips what it does not recognise.
// ---------------------------------------------------------------------------
void MusicSheet_Serialize(const MusicSheet& sheet, ProjectStateContext* ctx);

// One child line of a TSSNAPSHOT block. Returns true when the line was one of
// ours and has been consumed.
bool MusicSheet_ProcessLine(MusicSheet& sheet, const char* line);
