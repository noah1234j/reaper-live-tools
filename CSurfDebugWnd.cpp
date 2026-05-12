// ---------------------------------------------------------------------------
// CSurfDebugWnd.cpp – modeless MIDI event log / settings dump window
// ---------------------------------------------------------------------------
#include "CSurfDebugWnd.h"
#include "resource.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <ctime>

// REAPER API
extern HWND (*GetMainHwnd)();
extern int  (*GetNumMIDIInputs)();
extern bool (*GetMIDIInputName)(int, char*, int);
extern int  (*GetNumMIDIOutputs)();
extern bool (*GetMIDIOutputName)(int, char*, int);

// ---------------------------------------------------------------------------
static HWND        s_hwnd    = nullptr;
static HINSTANCE   s_hInst   = nullptr;

// Ring buffer for log lines (keep last 1000)
static std::vector<std::string> s_log;
static constexpr int kMaxLines = 1000;

// ---------------------------------------------------------------------------
static void FlushLogToList()
{
    if (!s_hwnd) return;
    HWND hList = GetDlgItem(s_hwnd, IDC_CSURF_DBG_LIST);
    if (!hList) return;
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    for (const auto& line : s_log)
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    int cnt = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    if (cnt > 0) SendMessage(hList, LB_SETTOPINDEX, cnt - 1, 0);
}

// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DebugDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        s_hwnd = hwnd;
        SetWindowText(hwnd, "Live Tools – MIDI Debug Log");
        FlushLogToList();
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CSURF_DBG_CLEAR:
            s_log.clear();
            if (HWND hList = GetDlgItem(hwnd, IDC_CSURF_DBG_LIST))
                SendMessage(hList, LB_RESETCONTENT, 0, 0);
            return TRUE;

        case IDC_CSURF_DBG_COPY:
        {
            if (s_log.empty()) return TRUE;
            std::string all;
            for (const auto& l : s_log) { all += l; all += "\r\n"; }
            if (OpenClipboard(hwnd))
            {
                EmptyClipboard();
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, all.size() + 1);
                if (hg)
                {
                    void* p = GlobalLock(hg);
                    if (p) { memcpy(p, all.c_str(), all.size() + 1); GlobalUnlock(hg); }
                    SetClipboardData(CF_TEXT, hg);
                }
                CloseClipboard();
            }
            return TRUE;
        }

        case IDCANCEL:
        case IDOK:
            DestroyWindow(hwnd);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        s_hwnd = nullptr;
        return TRUE;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
void CSurfDebug_Open(HWND parent, HINSTANCE hInst)
{
    if (s_hwnd) { SetForegroundWindow(s_hwnd); return; }
    s_hInst = hInst;
    HWND p = parent ? parent : (GetMainHwnd ? GetMainHwnd() : nullptr);
    CreateDialogParam(hInst, MAKEINTRESOURCE(IDD_CSURF_DEBUG), p, DebugDlgProc, 0);
    if (s_hwnd) ShowWindow(s_hwnd, SW_SHOW);
}

void CSurfDebug_Close()
{
    if (s_hwnd) { DestroyWindow(s_hwnd); s_hwnd = nullptr; }
}

bool CSurfDebug_IsOpen()
{
    return s_hwnd != nullptr;
}

void CSurfDebug_Log(const char* msg)
{
    if (!msg) return;

    // Timestamp prefix
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[1024];
    snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] %s",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    s_log.push_back(buf);
    if ((int)s_log.size() > kMaxLines)
        s_log.erase(s_log.begin(), s_log.begin() + (s_log.size() - kMaxLines));

    if (s_hwnd)
    {
        HWND hList = GetDlgItem(s_hwnd, IDC_CSURF_DBG_LIST);
        if (hList)
        {
            SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)buf);
            int cnt = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
            if (cnt > kMaxLines)
                SendMessage(hList, LB_DELETESTRING, 0, 0);
            SendMessage(hList, LB_SETTOPINDEX, cnt - 1, 0);
        }
    }
}

static const char* ProtocolName(CSurfProtocol p)
{
    switch (p)
    {
    case CSurfProtocol::MCU: return "MCU";
    case CSurfProtocol::HUI: return "HUI";
    case CSurfProtocol::FP16: return "FP16";
    case CSurfProtocol::RAW: return "RAW";
    }
    return "?";
}

void CSurfDebug_DumpSettings(const CSurfSettings& s)
{
    CSurfDebug_Log("=== Settings Dump ===");

    char buf[256];
    snprintf(buf, sizeof(buf), "Protocol: %s  MIDI In: %d  MIDI Out: %d",
        ProtocolName(s.proto), s.midiInDev, s.midiOutDev);
    CSurfDebug_Log(buf);

    snprintf(buf, sizeof(buf),
        "Template: %d  ChannelCount: %d  FaderMode: %d  BankOff: %d",
        s.templateIdx, s.channelCount, s.faderMode, s.bankOffset);
    CSurfDebug_Log(buf);

    snprintf(buf, sizeof(buf),
        "FollowSel: %d  FollowMCP: %d  FollowLayers: %d  SendColors: %d",
        s.followSel, s.followMCP, s.followLayers, s.sendColors);
    CSurfDebug_Log(buf);

    snprintf(buf, sizeof(buf),
        "SendsSpill: %d  SendsDisplayMode: %d  TouchChan: %d  DebugLog: %d",
        s.sendsSpillReceives, s.sendsDisplayMode, s.showTouchedChannels, s.debugLog);
    CSurfDebug_Log(buf);

    snprintf(buf, sizeof(buf), "Extenders: %d  GlobalBtnMap entries: %d",
        (int)s.extenders.size(), (int)s.btnMap.size());
    CSurfDebug_Log(buf);

    for (int i = 0; i < (int)s.extenders.size(); ++i)
    {
        const auto& ep = s.extenders[i];
        snprintf(buf, sizeof(buf),
            "  Ext[%d]: In=%d Out=%d Offset=%d Proto=%s Preset=%d BtnMap=%d",
            i, ep.midiInDev, ep.midiOutDev, ep.channelOffset,
            ProtocolName(ep.proto), ep.devicePreset, (int)ep.btnMap.size());
        CSurfDebug_Log(buf);
    }

    // MIDI device list
    if (GetNumMIDIInputs && GetMIDIInputName)
    {
        int n = GetNumMIDIInputs();
        snprintf(buf, sizeof(buf), "MIDI Inputs available: %d", n);
        CSurfDebug_Log(buf);
        char name[128];
        for (int i = 0; i < n && i < 16; ++i)
        {
            if (GetMIDIInputName(i, name, sizeof(name)))
            {
                snprintf(buf, sizeof(buf), "  [%d] %s", i, name);
                CSurfDebug_Log(buf);
            }
        }
    }

    if (GetNumMIDIOutputs && GetMIDIOutputName)
    {
        int n = GetNumMIDIOutputs();
        snprintf(buf, sizeof(buf), "MIDI Outputs available: %d", n);
        CSurfDebug_Log(buf);
        char name[128];
        for (int i = 0; i < n && i < 16; ++i)
        {
            if (GetMIDIOutputName(i, name, sizeof(name)))
            {
                snprintf(buf, sizeof(buf), "  [%d] %s", i, name);
                CSurfDebug_Log(buf);
            }
        }
    }

    CSurfDebug_Log("=== End Dump ===");
}
