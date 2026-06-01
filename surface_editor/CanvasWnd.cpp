// ---------------------------------------------------------------------------
// CanvasWnd.cpp  —  owner-drawn 2D widget canvas for the Surface & Zone editor
// ---------------------------------------------------------------------------

#include "CanvasWnd.h"
#include "SurfaceModel.h"
#include "ZoneModel.h"

#include <windowsx.h>
#include <string>
#include <algorithm>
#include <map>
#include <vector>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "Gdi32.lib")

namespace SurfaceEditor {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char* k_ClassName = "LT_CanvasWnd";

// Default cell dimensions (at zoom 1.0)
static const int k_CellW  = 80;
static const int k_CellH  = 50;
static const int k_CellPad = 4;
static const int k_ScrollH = 20;

// Widget-type colour palette (R, G, B)
struct RGB3 { BYTE r, g, b; };
static RGB3 WidgetTypeColor(WidgetType t)
{
    switch (t)
    {
    case WidgetType::Button:  return { 70,  130, 220 }; // blue
    case WidgetType::Fader:   return { 60,  180,  80 }; // green
    case WidgetType::Encoder: return { 220, 140,  40 }; // orange
    case WidgetType::Display: return { 140, 140, 140 }; // gray
    case WidgetType::VUMeter: return { 200,  70,  70 }; // red-ish
    default:                  return { 100, 100, 100 }; // dark gray
    }
}

static COLORREF MakeCOLORREF(RGB3 c) { return RGB(c.r, c.g, c.b); }

// ---------------------------------------------------------------------------
// Per-window state
// ---------------------------------------------------------------------------
struct CanvasState {
    Surface*       surf     = nullptr;
    ZoneFile*      zone     = nullptr;
    CanvasMode     mode     = CanvasMode::SurfaceEdit;
    CanvasCallbacks cb;
    float          zoom     = 1.0f;
    int            selCol   = -1;
    int            selRow   = -1;
    std::string    palette; // active new-widget type
    // Drag state
    bool           dragging    = false;
    int            dragSrcCol  = -1;
    int            dragSrcRow  = -1;
    int            dragOffX    = 0;    // cursor offset within cell at drag start (canvas coords)
    int            dragOffY    = 0;
    int            dragCurPixX = -1;   // cell top-left at current drag position (canvas coords)
    int            dragCurPixY = -1;
    // Scrolling
    int            scrollX  = 0;   // horizontal scroll (pixels)
    int            scrollY  = 0;   // vertical scroll (pixels, mousewheel)
    HWND           hScroll  = nullptr;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static CanvasState* GetState(HWND hwnd)
{
    return reinterpret_cast<CanvasState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
}

// ---------------------------------------------------------------------------
// Coordinate helpers (variable row heights)
// ---------------------------------------------------------------------------

static int ColWidth(const CanvasState* s)
{
    return (int)(k_CellW * s->zoom);
}

static int RowHeight(const CanvasState* s, int row)
{
    if (s->surf && row < (int)s->surf->rowHeights.size())
        return (int)(s->surf->rowHeights[row] * s->zoom);
    return (int)(k_CellH * s->zoom);
}

// Y coordinate of the top of a given row (screen space, not accounting for scroll)
static int RowY(const CanvasState* s, int row)
{
    int y = 0;
    for (int r = 0; r < row; ++r)
        y += RowHeight(s, r);
    return y;
}

static int TotalCanvasHeight(const CanvasState* s)
{
    if (!s->surf) return 0;
    return RowY(s, s->surf->gridRows);
}

static int TotalCanvasWidth(const CanvasState* s)
{
    if (!s->surf) return 0;
    return s->surf->gridCols * ColWidth(s);
}

// Map client-space point to grid col/row (-1 if outside)
static void HitTest(const CanvasState* s, int mx, int my, int& col, int& row)
{
    col = row = -1;
    if (!s->surf) return;

    int cw  = ColWidth(s);
    int ax  = mx + s->scrollX;  // canvas coords
    int ay  = my + s->scrollY;

    // Free-positioned widgets take priority — check them before the grid
    for (const auto& w : s->surf->widgets)
    {
        if (w.pixX < 0) continue;
        int rh = RowHeight(s, w.gridRow);
        if (ax >= w.pixX && ax < w.pixX + cw &&
            ay >= w.pixY && ay < w.pixY + rh)
        {
            col = w.gridCol;
            row = w.gridRow;
            return;
        }
    }

    if (ax < 0 || ay < 0) return;
    int c = ax / cw;
    if (c >= s->surf->gridCols) return;

    // Walk rows to find which row ay falls in
    int y = 0;
    for (int r = 0; r < s->surf->gridRows; ++r)
    {
        int rh = RowHeight(s, r);
        if (ay >= y && ay < y + rh)
        {
            col = c;
            row = r;
            return;
        }
        y += rh;
    }
}

// Find widget at grid position; returns nullptr if empty
static Widget* WidgetAt(Surface* surf, int col, int row)
{
    if (!surf) return nullptr;
    for (auto& w : surf->widgets)
        if (w.gridCol == col && w.gridRow == row)
            return &w;
    return nullptr;
}

// Look up zone action for a widget name
static std::string ActionForWidget(const ZoneFile* zone, const std::string& widgetName)
{
    if (!zone) return "";
    for (const auto& za : zone->assignments)
    {
        // Match by exact widget name or pipe-stripped name
        std::string we = za.widgetExpr;
        if (we == widgetName) return za.action;
        // Check pipe expansion: "Fader|" matches "Fader1", "Fader2", etc.
        if (!we.empty() && we.back() == '|')
        {
            std::string base = we.substr(0, we.size() - 1);
            if (widgetName.size() > base.size() &&
                widgetName.substr(0, base.size()) == base)
                return za.action;
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

static void DoPaint(HWND hwnd, CanvasState* s)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client;
    GetClientRect(hwnd, &client);
    int W = client.right;
    int H = client.bottom - (s->hScroll ? k_ScrollH : 0);

    // Off-screen buffer
    HDC hdcBuf = CreateCompatibleDC(hdc);
    HBITMAP hbm = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBm = SelectObject(hdcBuf, hbm);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 35));
    RECT bgRect = { 0, 0, W, H };
    FillRect(hdcBuf, &bgRect, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdcBuf, TRANSPARENT);

    if (s->surf)
    {
        int cw  = ColWidth(s);

        // --- Fonts ---
        HFONT hFontNormal = CreateFontA(
            (int)(14 * s->zoom), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hFontSmall = CreateFontA(
            (int)(12 * s->zoom), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdcBuf, hFontNormal);

        // --- Widgets (grouped by cell so encoder+push share one cell) ---
        using CellKey = std::pair<int,int>;
        std::map<CellKey, std::vector<const Widget*>> cellMap;
        for (const auto& w : s->surf->widgets)
            cellMap[{w.gridCol, w.gridRow}].push_back(&w);

        for (auto& [key, cWidgets] : cellMap)
        {
            int col = key.first;
            int row = key.second;
            int rh  = RowHeight(s, row);

            // Use free pixel position if the cell has been dragged, otherwise grid
            bool hasFreePos = (!cWidgets.empty() && cWidgets[0]->pixX >= 0);
            int x = (hasFreePos ? cWidgets[0]->pixX : col * cw) - s->scrollX;
            int y = (hasFreePos ? cWidgets[0]->pixY : RowY(s, row)) - s->scrollY;

            if (x + cw < 0 || x > W) continue;
            if (y + rh < 0 || y > H) continue;

            WidgetType wt = InferWidgetType(*cWidgets[0]);
            bool isSelected = (col == s->selCol && row == s->selRow);
            bool isDragSrc  = (s->dragging && col == s->dragSrcCol && row == s->dragSrcRow);

            int pri = (row < (int)s->surf->rowPriorities.size())
                      ? s->surf->rowPriorities[row] : 45;

            // Collect zone actions for all widgets in this cell
            std::vector<std::string> cellActions;
            if (s->mode == CanvasMode::ZoneAssign)
            {
                for (const Widget* cw2 : cWidgets)
                    cellActions.push_back(ActionForWidget(s->zone, cw2->name));
            }

            // Reserve space at bottom for action labels (one line per widget)
            int lh = (int)(14 * s->zoom);
            int labelH = (s->mode == CanvasMode::ZoneAssign)
                         ? (int)cWidgets.size() * lh
                         : 0;
            if (labelH > rh * 2 / 3) labelH = rh * 2 / 3;
            int lhActual = (s->mode == CanvasMode::ZoneAssign && !cWidgets.empty())
                           ? labelH / (int)cWidgets.size()
                           : lh;
            // Visual area above the label strip
            int topH = rh - labelH;

            RECT cellRect = { x + k_CellPad, y + k_CellPad,
                              x + cw - k_CellPad, y + rh - k_CellPad };

            if (isDragSrc)
            {
                int rr = (int)(6 * s->zoom);
                HPEN dp = CreatePen(PS_DASH, 1, RGB(200, 200, 70));
                HGDIOBJ op2 = SelectObject(hdcBuf, dp);
                SelectObject(hdcBuf, GetStockObject(NULL_BRUSH));
                RoundRect(hdcBuf, cellRect.left, cellRect.top, cellRect.right, cellRect.bottom, rr, rr);
                SelectObject(hdcBuf, op2);
                DeleteObject(dp);
                continue;
            }

            // ---- Fader ----
            if (wt == WidgetType::Fader)
            {
                int cx     = x + cw / 2;
                int trackW = std::max(4, (int)(5 * s->zoom));
                int capW   = std::max(20, (int)(34 * s->zoom));
                int capH   = std::max(8,  (int)(12 * s->zoom));
                int tTop   = y + (int)(22 * s->zoom);
                int tBot   = y + topH - (int)(8 * s->zoom);
                int capY   = (tTop + tBot) / 2 - capH / 2;

                if (isSelected)
                {
                    HBRUSH sb = CreateSolidBrush(RGB(40, 70, 40));
                    FillRect(hdcBuf, &cellRect, sb);
                    DeleteObject(sb);
                }

                SelectObject(hdcBuf, hFontSmall);
                SetTextColor(hdcBuf, RGB(180, 220, 180));
                RECT nr = { x + 2, y + 2, x + cw - 2, y + (int)(20 * s->zoom) };
                DrawTextA(hdcBuf, cWidgets[0]->name.c_str(), -1, &nr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdcBuf, hFontNormal);

                HBRUSH trackBr = CreateSolidBrush(RGB(20, 20, 20));
                RECT trackRc = { cx - trackW/2, tTop, cx + trackW/2 + 1, tBot };
                FillRect(hdcBuf, &trackRc, trackBr);
                DeleteObject(trackBr);
                HPEN trackPen = CreatePen(PS_SOLID, 1, RGB(70, 80, 70));
                HGDIOBJ op2 = SelectObject(hdcBuf, trackPen);
                SelectObject(hdcBuf, GetStockObject(NULL_BRUSH));
                Rectangle(hdcBuf, trackRc.left, trackRc.top, trackRc.right, trackRc.bottom);

                COLORREF capClr = isSelected ? RGB(200, 230, 200) : RGB(140, 180, 145);
                HBRUSH capBr = CreateSolidBrush(capClr);
                SelectObject(hdcBuf, capBr);
                RECT capRc = { cx - capW/2, capY, cx + capW/2 + 1, capY + capH };
                FillRect(hdcBuf, &capRc, capBr);
                DeleteObject(capBr);

                HPEN capPen = CreatePen(PS_SOLID, 1, RGB(80, 100, 80));
                SelectObject(hdcBuf, capPen);
                SelectObject(hdcBuf, GetStockObject(NULL_BRUSH));
                Rectangle(hdcBuf, capRc.left, capRc.top, capRc.right, capRc.bottom);
                MoveToEx(hdcBuf, capRc.left + 3, capY + capH/2, nullptr);
                LineTo(hdcBuf, capRc.right - 3, capY + capH/2);

                SelectObject(hdcBuf, op2);
                DeleteObject(trackPen);
                DeleteObject(capPen);

                if (isSelected)
                {
                    HPEN sp = CreatePen(PS_SOLID, 2, RGB(150, 255, 150));
                    op2 = SelectObject(hdcBuf, sp);
                    SelectObject(hdcBuf, GetStockObject(NULL_BRUSH));
                    Rectangle(hdcBuf, cellRect.left, cellRect.top, cellRect.right, cellRect.bottom);
                    SelectObject(hdcBuf, op2);
                    DeleteObject(sp);
                }
            }
            // ---- Encoder ----
            else if (wt == WidgetType::Encoder)
            {
                int cx  = x + cw / 2;
                int cy  = y + topH / 2 - (int)(2 * s->zoom);
                int rad = std::min(cw, topH) / 2 - (int)(5 * s->zoom);
                if (rad < 4) rad = 4;

                COLORREF outerClr = isSelected ? RGB(255, 200, 80) : RGB(210, 130, 35);
                HBRUSH outerBr = CreateSolidBrush(outerClr);
                HPEN outerPen = CreatePen(PS_SOLID, isSelected ? 2 : 1,
                                          isSelected ? RGB(255, 255, 100) : RGB(160, 120, 30));
                HGDIOBJ op2 = SelectObject(hdcBuf, outerPen);
                SelectObject(hdcBuf, outerBr);
                Ellipse(hdcBuf, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
                DeleteObject(outerBr);

                int ir = rad * 2 / 3;
                HBRUSH innerBr = CreateSolidBrush(RGB(240, 160, 50));
                SelectObject(hdcBuf, innerBr);
                SelectObject(hdcBuf, GetStockObject(NULL_PEN));
                Ellipse(hdcBuf, cx - ir, cy - ir, cx + ir + 1, cy + ir + 1);
                DeleteObject(innerBr);

                HBRUSH dotBr = CreateSolidBrush(RGB(15, 15, 15));
                SelectObject(hdcBuf, dotBr);
                int dotR = std::max(2, (int)(3 * s->zoom));
                int dotY2 = cy - rad + (int)(4 * s->zoom);
                Ellipse(hdcBuf, cx - dotR, dotY2, cx + dotR + 1, dotY2 + dotR * 2 + 1);
                DeleteObject(dotBr);

                SelectObject(hdcBuf, op2);
                DeleteObject(outerPen);

                // In surface-edit mode show name; in zone-assign, labels shown below
                if (s->mode != CanvasMode::ZoneAssign)
                {
                    SelectObject(hdcBuf, hFontSmall);
                    SetTextColor(hdcBuf, RGB(220, 200, 160));
                    RECT nr = { x + 1, cy + rad + 2, x + cw - 1, y + rh - 1 };
                    DrawTextA(hdcBuf, cWidgets[0]->name.c_str(), -1, &nr,
                              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SelectObject(hdcBuf, hFontNormal);
                }
            }
            // ---- Display / VUMeter ----
            else if (wt == WidgetType::Display || wt == WidgetType::VUMeter)
            {
                int rr = (int)(6 * s->zoom);
                HBRUSH dispBr = CreateSolidBrush(isSelected ? RGB(60, 60, 80) : RGB(25, 25, 40));
                HPEN dispPen = CreatePen(PS_SOLID, isSelected ? 2 : 1,
                                         isSelected ? RGB(150, 150, 255) : RGB(80, 80, 120));
                HGDIOBJ op2 = SelectObject(hdcBuf, dispPen);
                SelectObject(hdcBuf, dispBr);
                RoundRect(hdcBuf, cellRect.left, cellRect.top, cellRect.right, cellRect.bottom, rr, rr);
                SelectObject(hdcBuf, op2);
                DeleteObject(dispBr);
                DeleteObject(dispPen);

                SetTextColor(hdcBuf, RGB(160, 200, 255));
                SelectObject(hdcBuf, hFontSmall);
                RECT nr = { cellRect.left + 2, cellRect.top + 2,
                             cellRect.right - 2, y + topH - 2 };
                DrawTextA(hdcBuf, cWidgets[0]->name.c_str(), -1, &nr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdcBuf, hFontNormal);
            }
            // ---- Button / Unknown ----
            else
            {
                COLORREF btnFill;
                switch (pri)
                {
                case 20: btnFill = isSelected ? RGB(200, 60, 60)  : RGB(110, 40, 40); break; // arm
                case 30: btnFill = isSelected ? RGB(220, 200, 30) : RGB(95, 85, 14);  break; // solo
                case 40: btnFill = isSelected ? RGB(220, 130, 30) : RGB(105, 65, 20); break; // mute
                case 50: btnFill = isSelected ? RGB(80, 120, 220) : RGB(35, 62, 108); break; // select
                default: btnFill = isSelected ? RGB(140, 140, 210): RGB(62, 62, 95);  break;
                }
                int rr = (int)(6 * s->zoom);
                HBRUSH btnBr = CreateSolidBrush(btnFill);
                HGDIOBJ op2 = SelectObject(hdcBuf, btnBr);
                SelectObject(hdcBuf, GetStockObject(NULL_PEN));
                RoundRect(hdcBuf, cellRect.left, cellRect.top, cellRect.right, cellRect.bottom, rr, rr);
                SelectObject(hdcBuf, op2);
                DeleteObject(btnBr);

                // Selection ring
                if (isSelected)
                {
                    HPEN sp = CreatePen(PS_SOLID, 2, RGB(255, 255, 100));
                    op2 = SelectObject(hdcBuf, sp);
                    SelectObject(hdcBuf, GetStockObject(NULL_BRUSH));
                    RoundRect(hdcBuf, cellRect.left, cellRect.top, cellRect.right, cellRect.bottom, rr, rr);
                    SelectObject(hdcBuf, op2);
                    DeleteObject(sp);
                }

                if (s->mode != CanvasMode::ZoneAssign)
                {
                    SetTextColor(hdcBuf, RGB(230, 230, 230));
                    RECT nr = cellRect;
                    nr.left += 3; nr.right -= 3;
                    DrawTextA(hdcBuf, cWidgets[0]->name.c_str(), -1, &nr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }

            // ---- Action labels at bottom (ZoneAssign mode, one per widget in cell) ----
            if (s->mode == CanvasMode::ZoneAssign && !cWidgets.empty())
            {
                int lTop = y + topH;
                SelectObject(hdcBuf, hFontSmall);
                for (int ai = 0; ai < (int)cWidgets.size(); ++ai)
                {
                    const std::string& act = cellActions[ai];
                    bool hasAct = !act.empty();
                    SetTextColor(hdcBuf, hasAct ? RGB(200, 255, 180) : RGB(150, 150, 150));
                    RECT ar = { x + 2, lTop + ai * lhActual,
                                x + cw - 2, lTop + (ai + 1) * lhActual };
                    if (ar.bottom > y + rh) ar.bottom = y + rh;
                    const char* lbl = hasAct ? act.c_str() : cWidgets[ai]->name.c_str();
                    DrawTextA(hdcBuf, lbl, -1, &ar,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                SelectObject(hdcBuf, hFontNormal);
            }
        }

        // Drag ghost — follows cursor pixel-for-pixel
        if (s->dragging && s->dragCurPixX >= 0)
        {
            int x  = s->dragCurPixX - s->scrollX;
            int y  = s->dragCurPixY - s->scrollY;
            int rh = RowHeight(s, s->dragSrcRow);
            RECT ghostRect = { x + k_CellPad, y + k_CellPad,
                               x + cw - k_CellPad, y + rh - k_CellPad };
            int rr = (int)(6 * s->zoom);
            HPEN ghostPen = CreatePen(PS_SOLID, 2, RGB(210, 210, 70));
            HGDIOBJ op2 = SelectObject(hdcBuf, ghostPen);
            SelectObject(hdcBuf, GetStockObject(NULL_BRUSH));
            RoundRect(hdcBuf, ghostRect.left, ghostRect.top, ghostRect.right, ghostRect.bottom, rr, rr);
            SelectObject(hdcBuf, op2);
            DeleteObject(ghostPen);
        }

        SelectObject(hdcBuf, oldFont);
        DeleteObject(hFontNormal);
        DeleteObject(hFontSmall);
    }
    else
    {
        // No surface loaded — show placeholder
        SetTextColor(hdcBuf, RGB(120, 120, 120));
        SetBkMode(hdcBuf, TRANSPARENT);
        DrawTextA(hdcBuf, "No surface loaded.\r\nOpen or create a surface to begin.",
                  -1, &bgRect, DT_CENTER | DT_VCENTER);
    }

    BitBlt(hdc, 0, 0, W, H, hdcBuf, 0, 0, SRCCOPY);
    SelectObject(hdcBuf, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdcBuf);

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Update scrollbar
// ---------------------------------------------------------------------------

static void UpdateScrollBar(HWND hwnd, CanvasState* s)
{
    if (!s->hScroll || !s->surf) return;

    RECT client;
    GetClientRect(hwnd, &client);
    int visW   = client.right;
    int totalW = TotalCanvasWidth(s);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = totalW;
    si.nPage  = visW;
    si.nPos   = s->scrollX;
    SetScrollInfo(s->hScroll, SB_CTL, &si, TRUE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CanvasState* s = GetState(hwnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        s = new CanvasState();
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)s);

        // Create scrollbar at bottom
        RECT rc; GetClientRect(hwnd, &rc);
        s->hScroll = CreateWindowExA(0, "SCROLLBAR", nullptr,
            WS_CHILD | WS_VISIBLE | SBS_HORZ,
            0, rc.bottom - k_ScrollH, rc.right, k_ScrollH,
            hwnd, nullptr,
            (HINSTANCE)GetWindowLongPtrA(hwnd, GWLP_HINSTANCE), nullptr);
        return 0;
    }

    case WM_DESTROY:
        if (s) { delete s; SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0); }
        return 0;

    case WM_SIZE:
    {
        if (s && s->hScroll)
        {
            int W = LOWORD(lp), H = HIWORD(lp);
            SetWindowPos(s->hScroll, nullptr, 0, H - k_ScrollH, W, k_ScrollH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateScrollBar(hwnd, s);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_PAINT:
        if (s) DoPaint(hwnd, s);
        return 0;

    case WM_ERASEBKGND:
        return 1; // handled in WM_PAINT

    case WM_HSCROLL:
    {
        if (!s) return 0;
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(s->hScroll, SB_CTL, &si);

        int newPos = si.nPos;
        switch (LOWORD(wp))
        {
        case SB_LINELEFT:       newPos -= 10; break;
        case SB_LINERIGHT:      newPos += 10; break;
        case SB_PAGELEFT:       newPos -= si.nPage; break;
        case SB_PAGERIGHT:      newPos += si.nPage; break;
        case SB_THUMBTRACK:     newPos = si.nTrackPos; break;
        case SB_THUMBPOSITION:  newPos = si.nTrackPos; break;
        }
        if (newPos < 0) newPos = 0;
        s->scrollX = newPos;
        SetScrollPos(s->hScroll, SB_CTL, newPos, TRUE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        if (!s) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        s->scrollY -= delta / 3;
        if (s->scrollY < 0) s->scrollY = 0;
        // Clamp to content height
        RECT rcCl; GetClientRect(hwnd, &rcCl);
        int visH   = rcCl.bottom - (s->hScroll ? k_ScrollH : 0);
        int maxScrollY = TotalCanvasHeight(s) - visH;
        if (maxScrollY < 0) maxScrollY = 0;
        if (s->scrollY > maxScrollY) s->scrollY = maxScrollY;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        if (!s) return 0;
        SetCapture(hwnd);
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int col, row;
        HitTest(s, mx, my, col, row);

        if (col >= 0 && row >= 0)
        {
            Widget* w = WidgetAt(s->surf, col, row);
            if (w)
            {
                s->selCol = col; s->selRow = row;
                if (s->mode == CanvasMode::SurfaceEdit)
                {
                    int cw2   = ColWidth(s);
                    int ax    = mx + s->scrollX;
                    int ay    = my + s->scrollY;
                    int cellX = (w->pixX >= 0) ? w->pixX : col * cw2;
                    int cellY = (w->pixY >= 0) ? w->pixY : RowY(s, row);
                    s->dragging    = true;
                    s->dragSrcCol  = col;
                    s->dragSrcRow  = row;
                    s->dragOffX    = ax - cellX;
                    s->dragOffY    = ay - cellY;
                    s->dragCurPixX = cellX;
                    s->dragCurPixY = cellY;
                }
                if (s->cb.onWidgetSelected)
                    s->cb.onWidgetSelected(col, row);
            }
            else
            {
                s->selCol = -1; s->selRow = -1;
                if (s->mode == CanvasMode::SurfaceEdit && s->cb.onBlankCellClicked)
                    s->cb.onBlankCellClicked(col, row);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
        if (!s) return 0;
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int col, row;
        HitTest(s, mx, my, col, row);
        if (col >= 0 && row >= 0 && WidgetAt(s->surf, col, row))
        {
            if (s->cb.onWidgetActivated)
                s->cb.onWidgetActivated(col, row);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!s || !s->dragging) return 0;
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int newX = mx + s->scrollX - s->dragOffX;
        int newY = my + s->scrollY - s->dragOffY;
        if (newX != s->dragCurPixX || newY != s->dragCurPixY)
        {
            s->dragCurPixX = newX;
            s->dragCurPixY = newY;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        if (!s) return 0;
        ReleaseCapture();
        if (s->dragging)
        {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            int newPixX = mx + s->scrollX - s->dragOffX;
            int newPixY = my + s->scrollY - s->dragOffY;
            // Compare against original position to detect a real move
            int cw2 = ColWidth(s);
            Widget* dw = WidgetAt(s->surf, s->dragSrcCol, s->dragSrcRow);
            int origX = (dw && dw->pixX >= 0) ? dw->pixX : s->dragSrcCol * cw2;
            int origY = (dw && dw->pixY >= 0) ? dw->pixY : RowY(s, s->dragSrcRow);
            bool moved = (newPixX != origX || newPixY != origY);
            if (moved && s->cb.onWidgetMoved)
                s->cb.onWidgetMoved(s->dragSrcCol, s->dragSrcRow, newPixX, newPixY);
            s->dragging    = false;
            s->dragSrcCol  = -1;
            s->dragSrcRow  = -1;
            s->dragOffX    = 0;
            s->dragOffY    = 0;
            s->dragCurPixX = -1;
            s->dragCurPixY = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Public API implementation
// ---------------------------------------------------------------------------

bool CanvasWnd_RegisterClass(HINSTANCE hInst)
{
    WNDCLASSEXA wc = {};
    wc.cbSize       = sizeof(wc);
    wc.style        = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc  = CanvasProc;
    wc.hInstance    = hInst;
    wc.hCursor      = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground= nullptr; // we paint background ourselves
    wc.lpszClassName= k_ClassName;
    return RegisterClassExA(&wc) != 0;
}

HWND CanvasWnd_Create(HWND hParent, HINSTANCE hInst, int x, int y, int w, int h)
{
    return CreateWindowExA(0, k_ClassName, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        x, y, w, h,
        hParent, nullptr, hInst, nullptr);
}

void CanvasWnd_SetSurface(HWND hCanvas, Surface* surf)
{
    if (auto* s = GetState(hCanvas)) { s->surf = surf; UpdateScrollBar(hCanvas, s); InvalidateRect(hCanvas, nullptr, FALSE); }
}

void CanvasWnd_SetZoneFile(HWND hCanvas, ZoneFile* zone)
{
    if (auto* s = GetState(hCanvas)) { s->zone = zone; InvalidateRect(hCanvas, nullptr, FALSE); }
}

void CanvasWnd_SetMode(HWND hCanvas, CanvasMode mode)
{
    if (auto* s = GetState(hCanvas)) { s->mode = mode; InvalidateRect(hCanvas, nullptr, FALSE); }
}

void CanvasWnd_SetPaletteSelection(HWND hCanvas, const char* type)
{
    if (auto* s = GetState(hCanvas)) s->palette = type ? type : "";
}

void CanvasWnd_SetSelection(HWND hCanvas, int col, int row)
{
    if (auto* s = GetState(hCanvas)) { s->selCol = col; s->selRow = row; InvalidateRect(hCanvas, nullptr, FALSE); }
}

void CanvasWnd_SetCallbacks(HWND hCanvas, const CanvasCallbacks& cb)
{
    if (auto* s = GetState(hCanvas)) s->cb = cb;
}

void CanvasWnd_SetZoom(HWND hCanvas, float zoom)
{
    if (auto* s = GetState(hCanvas)) { s->zoom = zoom < 0.3f ? 0.3f : zoom; UpdateScrollBar(hCanvas, s); InvalidateRect(hCanvas, nullptr, FALSE); }
}

void CanvasWnd_Refresh(HWND hCanvas)
{
    if (hCanvas) InvalidateRect(hCanvas, nullptr, FALSE);
}

void CanvasWnd_Destroy(HWND hCanvas)
{
    if (hCanvas) DestroyWindow(hCanvas);
}

} // namespace SurfaceEditor
