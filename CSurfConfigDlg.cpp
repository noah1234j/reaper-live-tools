// ---------------------------------------------------------------------------
// CSurfConfigDlg.cpp  -  Control Surface settings dialog
//
// REAPER embeds this as a child panel inside its own "Control Surface Settings"
// outer dialog. The correct pattern (per SDK csurf reference) is:
//   - ShowConfig returns CreateDialogParamA(...) - a modeless child HWND
//   - REAPER sends WM_USER+1024 when its outer OK is clicked;
//     we write the serialised config string into (char*)lParam
//   - No OK/Cancel buttons in this panel - REAPER provides them
// ---------------------------------------------------------------------------
#include "ControlSurface.h"
#include "BtnMapWnd.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// Module instance - set from ReaperPluginEntry before any dialog is shown
static HINSTANCE s_hInstForDlg = nullptr;

// Working copy of main port, extenders and btnMap used by the open dialog
static ExtenderPort              s_dlgMainPort;
static std::vector<ExtenderPort> s_dlgExtenders;
static BtnMap                    s_dlgBtnMap;

void CSurfConfigDlg_SetInstance(HINSTANCE hInst)
{
    s_hInstForDlg = hInst;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void FillMidiInputCombo(HWND hCombo, int selDev)
{
    if (!hCombo) return;
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    int idx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"(none)");
    SendMessageA(hCombo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)-1);
    SendMessageA(hCombo, CB_SETCURSEL, 0, 0);
    int n = GetNumMIDIInputs ? GetNumMIDIInputs() : 0;
    for (int i = 0; i < n; ++i)
    {
        char name[256] = "";
        if (GetMIDIInputName) GetMIDIInputName(i, name, (int)sizeof(name));
        if (!name[0]) snprintf(name, sizeof(name), "MIDI Input %d", i);
        int ci = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)name);
        SendMessageA(hCombo, CB_SETITEMDATA, (WPARAM)ci, (LPARAM)i);
        if (i == selDev) SendMessageA(hCombo, CB_SETCURSEL, (WPARAM)ci, 0);
    }
}

static void FillMidiOutputCombo(HWND hCombo, int selDev)
{
    if (!hCombo) return;
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    int idx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"(none)");
    SendMessageA(hCombo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)-1);
    SendMessageA(hCombo, CB_SETCURSEL, 0, 0);
    int n = GetNumMIDIOutputs ? GetNumMIDIOutputs() : 0;
    for (int i = 0; i < n; ++i)
    {
        char name[256] = "";
        if (GetMIDIOutputName) GetMIDIOutputName(i, name, (int)sizeof(name));
        if (!name[0]) snprintf(name, sizeof(name), "MIDI Output %d", i);
        int ci = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)name);
        SendMessageA(hCombo, CB_SETITEMDATA, (WPARAM)ci, (LPARAM)i);
        if (i == selDev) SendMessageA(hCombo, CB_SETCURSEL, (WPARAM)ci, 0);
    }
}

static void GetMidiInName(int dev, char* buf, int bufsz)
{
    buf[0] = '\0';
    if (dev < 0) { lstrcpynA(buf, "(none)", bufsz); return; }
    if (GetMIDIInputName) GetMIDIInputName(dev, buf, bufsz);
    if (!buf[0]) snprintf(buf, bufsz, "MIDI Input %d", dev);
}

static void GetMidiOutName(int dev, char* buf, int bufsz)
{
    buf[0] = '\0';
    if (dev < 0) { lstrcpynA(buf, "(none)", bufsz); return; }
    if (GetMIDIOutputName) GetMIDIOutputName(dev, buf, bufsz);
    if (!buf[0]) snprintf(buf, bufsz, "MIDI Output %d", dev);
}

static void RebuildPortList(HWND hList)
{
    if (!hList) return;
    SendMessageA(hList, LB_RESETCONTENT, 0, 0);
    // Index 0: main surface port
    {
        char inName[64], outName[64], line[192];
        GetMidiInName (s_dlgMainPort.midiInDev,  inName,  sizeof(inName));
        GetMidiOutName(s_dlgMainPort.midiOutDev, outName, sizeof(outName));
        snprintf(line, sizeof(line), "Main:   In=%s  Out=%s", inName, outName);
        SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)line);
    }
    for (int i = 0; i < (int)s_dlgExtenders.size(); ++i)
    {
        char inName[64], outName[64], line[192];
        GetMidiInName (s_dlgExtenders[i].midiInDev,  inName,  sizeof(inName));
        GetMidiOutName(s_dlgExtenders[i].midiOutDev, outName, sizeof(outName));
        snprintf(line, sizeof(line), "Ext %d:  In=%s  Out=%s", i + 1, inName, outName);
        SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)line);
    }
}

static int ComboGetDeviceId(HWND hCombo)
{
    if (!hCombo) return -1;
    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return -1;
    return (int)SendMessageA(hCombo, CB_GETITEMDATA, (WPARAM)sel, 0);
}

// ---------------------------------------------------------------------------
// Extender add/edit sub-dialog
// ---------------------------------------------------------------------------
struct ExtDlgData
{
    ExtenderPort port;
    bool         isEdit;
    const char*  title;  // dialog window title
};

static INT_PTR CALLBACK ExtenderDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ExtDlgData* d = reinterpret_cast<ExtDlgData*>(lParam);
        SetWindowLongPtrA(hDlg, DWLP_USER, (LONG_PTR)d);
        SetWindowTextA(hDlg, d->title ? d->title : (d->isEdit ? "Edit Port" : "Add Port"));
        FillMidiInputCombo (GetDlgItem(hDlg, IDC_EXT_MIDI_IN),  d->port.midiInDev);
        FillMidiOutputCombo(GetDlgItem(hDlg, IDC_EXT_MIDI_OUT), d->port.midiOutDev);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            ExtDlgData* d = reinterpret_cast<ExtDlgData*>(
                GetWindowLongPtrA(hDlg, DWLP_USER));
            if (d)
            {
                d->port.midiInDev  = ComboGetDeviceId(GetDlgItem(hDlg, IDC_EXT_MIDI_IN));
                d->port.midiOutDev = ComboGetDeviceId(GetDlgItem(hDlg, IDC_EXT_MIDI_OUT));
            }
            EndDialog(hDlg, IDOK);
        }
        else if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDCANCEL);
        }
        return FALSE;
    }
    return FALSE;
}

static CSurfSettings ReadSettings(HWND hDlg)
{
    CSurfSettings s;
    s.templateIdx = (int)SendDlgItemMessageA(hDlg, IDC_CSURF_TEMPLATE, CB_GETCURSEL, 0, 0);
    if (s.templateIdx < 0) s.templateIdx = 0;

    s.proto = IsDlgButtonChecked(hDlg, IDC_CSURF_PROTO_HUI) == BST_CHECKED
        ? CSurfProtocol::HUI : CSurfProtocol::MCU;

    s.channelCount = (s.templateIdx >= 0 && s.templateIdx < k_csurfTemplateCount)
        ? k_csurfTemplates[s.templateIdx].channelCount : 8;

    s.midiInDev  = s_dlgMainPort.midiInDev;
    s.midiOutDev = s_dlgMainPort.midiOutDev;

    s.followSel = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_SEL) == BST_CHECKED;

    if      (IsDlgButtonChecked(hDlg, IDC_CSURF_FADER_PAN)  == BST_CHECKED) s.faderMode = 1;
    else if (IsDlgButtonChecked(hDlg, IDC_CSURF_FADER_SEND) == BST_CHECKED) s.faderMode = 2;
    else                                                                       s.faderMode = 0;

    s.sendColors   = IsDlgButtonChecked(hDlg, IDC_CSURF_SEND_COLORS)    == BST_CHECKED;
    s.followMCP    = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_MCP)     == BST_CHECKED;
    s.followLayers = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_LAYERS)  == BST_CHECKED;

    s.extenders  = s_dlgExtenders;
    s.btnMap     = s_dlgBtnMap;
    s.bankOffset = 0;
    return s;
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK CSurfConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        const char* initCfg = reinterpret_cast<const char*>(lParam);
        CSurfSettings s = CSurfSettings::Deserialize(initCfg ? initCfg : "");

        // Preserve main port, btn map and extenders in dialog-scope statics
        s_dlgBtnMap    = s.btnMap;
        s_dlgMainPort  = { s.midiInDev, s.midiOutDev };
        s_dlgExtenders = s.extenders;

        HWND hTmpl = GetDlgItem(hDlg, IDC_CSURF_TEMPLATE);
        for (int i = 0; i < k_csurfTemplateCount; ++i)
            SendMessageA(hTmpl, CB_ADDSTRING, 0, (LPARAM)k_csurfTemplates[i].name);
        int tmplSel = (s.templateIdx >= 0 && s.templateIdx < k_csurfTemplateCount)
            ? s.templateIdx : 0;
        SendMessageA(hTmpl, CB_SETCURSEL, (WPARAM)tmplSel, 0);

        CheckRadioButton(hDlg, IDC_CSURF_PROTO_MCU, IDC_CSURF_PROTO_HUI,
            s.proto == CSurfProtocol::HUI ? IDC_CSURF_PROTO_HUI : IDC_CSURF_PROTO_MCU);

        RebuildPortList(GetDlgItem(hDlg, IDC_CSURF_EXT_LIST));
        SendMessageA(GetDlgItem(hDlg, IDC_CSURF_EXT_LIST), LB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(hDlg, IDC_CSURF_EXT_REMOVE), FALSE);

        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_SEL,    s.followSel    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_SEND_COLORS,   s.sendColors   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_MCP,    s.followMCP    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_LAYERS, s.followLayers ? BST_CHECKED : BST_UNCHECKED);

        CheckRadioButton(hDlg, IDC_CSURF_FADER_VOL, IDC_CSURF_FADER_SEND,
            s.faderMode == 1 ? IDC_CSURF_FADER_PAN  :
            s.faderMode == 2 ? IDC_CSURF_FADER_SEND : IDC_CSURF_FADER_VOL);

        if (tmplSel < k_csurfTemplateCount)
            SetDlgItemTextA(hDlg, IDC_CSURF_DESC, k_csurfTemplates[tmplSel].desc);

        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CSURF_TEMPLATE && HIWORD(wParam) == CBN_SELCHANGE)
        {
            int idx = (int)SendDlgItemMessageA(hDlg, IDC_CSURF_TEMPLATE, CB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < k_csurfTemplateCount)
            {
                const CSurfTemplate& t = k_csurfTemplates[idx];
                CheckRadioButton(hDlg, IDC_CSURF_PROTO_MCU, IDC_CSURF_PROTO_HUI,
                    t.proto == CSurfProtocol::HUI ? IDC_CSURF_PROTO_HUI : IDC_CSURF_PROTO_MCU);
                SetDlgItemTextA(hDlg, IDC_CSURF_DESC, t.desc);
            }
        }
        else if (LOWORD(wParam) == IDC_CSURF_EXT_ADD)
        {
            ExtDlgData data{ { -1, -1 }, false, "Add Extender Port" };
            if (s_hInstForDlg &&
                DialogBoxParamA(s_hInstForDlg,
                    MAKEINTRESOURCEA(IDD_CSURF_EXTENDER_EDIT),
                    hDlg, ExtenderDlgProc, (LPARAM)&data) == IDOK)
            {
                s_dlgExtenders.push_back(data.port);
                HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
                RebuildPortList(hList);
                int newSel = 1 + (int)s_dlgExtenders.size() - 1;
                SendMessageA(hList, LB_SETCURSEL, (WPARAM)newSel, 0);
                EnableWindow(GetDlgItem(hDlg, IDC_CSURF_EXT_REMOVE), TRUE);
            }
        }
        else if (LOWORD(wParam) == IDC_CSURF_EXT_EDIT)
        {
            HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
            int sel = hList ? (int)SendMessageA(hList, LB_GETCURSEL, 0, 0) : -1;
            if (sel == 0)
            {
                // Edit main surface port
                ExtDlgData data{ s_dlgMainPort, true, "Edit Main Surface Port" };
                if (s_hInstForDlg &&
                    DialogBoxParamA(s_hInstForDlg,
                        MAKEINTRESOURCEA(IDD_CSURF_EXTENDER_EDIT),
                        hDlg, ExtenderDlgProc, (LPARAM)&data) == IDOK)
                {
                    s_dlgMainPort = data.port;
                    RebuildPortList(hList);
                    SendMessageA(hList, LB_SETCURSEL, 0, 0);
                }
            }
            else if (sel > 0 && sel - 1 < (int)s_dlgExtenders.size())
            {
                // Edit extender port
                ExtDlgData data{ s_dlgExtenders[sel - 1], true, "Edit Extender Port" };
                if (s_hInstForDlg &&
                    DialogBoxParamA(s_hInstForDlg,
                        MAKEINTRESOURCEA(IDD_CSURF_EXTENDER_EDIT),
                        hDlg, ExtenderDlgProc, (LPARAM)&data) == IDOK)
                {
                    s_dlgExtenders[sel - 1] = data.port;
                    RebuildPortList(hList);
                    SendMessageA(hList, LB_SETCURSEL, (WPARAM)sel, 0);
                }
            }
        }
        else if (LOWORD(wParam) == IDC_CSURF_EXT_REMOVE)
        {
            HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
            int sel = hList ? (int)SendMessageA(hList, LB_GETCURSEL, 0, 0) : -1;
            if (sel > 0 && sel - 1 < (int)s_dlgExtenders.size())
            {
                s_dlgExtenders.erase(s_dlgExtenders.begin() + (sel - 1));
                RebuildPortList(hList);
                // Select the item before the removed one (or main if none left)
                int newSel = (sel - 1 < (int)s_dlgExtenders.size()) ? sel : sel - 1;
                SendMessageA(hList, LB_SETCURSEL, (WPARAM)newSel, 0);
                EnableWindow(GetDlgItem(hDlg, IDC_CSURF_EXT_REMOVE), newSel > 0);
            }
        }
        else if (LOWORD(wParam) == IDC_CSURF_EXT_LIST && HIWORD(wParam) == LBN_SELCHANGE)
        {
            HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
            int sel = hList ? (int)SendMessageA(hList, LB_GETCURSEL, 0, 0) : -1;
            EnableWindow(GetDlgItem(hDlg, IDC_CSURF_EXT_REMOVE), sel > 0);
        }
        else if (LOWORD(wParam) == IDC_CSURF_BTN_MAP_BTN)
        {
            BtnMapWnd_ShowModal(hDlg, s_hInstForDlg, s_dlgBtnMap);
        }
        else if (LOWORD(wParam) == IDOK)
        {
            CSurf_ApplySettings(ReadSettings(hDlg));
            EndDialog(hDlg, IDOK);
        }
        else if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDCANCEL);
        }
        return FALSE;

    case WM_USER + 1024:
        if (wParam > 1 && lParam)
        {
            CSurfSettings s = ReadSettings(hDlg);
            std::string cfg = s.Serialize();
            lstrcpynA(reinterpret_cast<char*>(lParam), cfg.c_str(), (int)wParam);
        }
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// ShowConfig - csurf factory entry point
// Returns a modeless child HWND that REAPER embeds in its outer dialog.
// REAPER sends WM_USER+1024 to retrieve the config string on outer OK.
// ---------------------------------------------------------------------------
HWND CSurf_ShowConfig(
    const char* /*typeString*/,
    HWND        parent,
    const char* initConfigString)
{
    if (!s_hInstForDlg) return nullptr;

    return CreateDialogParamA(
        s_hInstForDlg,
        MAKEINTRESOURCEA(IDD_CSURF_CONFIG),
        parent,
        CSurfConfigDlgProc,
        (LPARAM)initConfigString);
}

// ---------------------------------------------------------------------------
// CSurf_ShowStandaloneConfig - modal dialog opened from Extensions > Live Tools
// Pre-populates from the running instance's current config; applies on OK.
// ---------------------------------------------------------------------------
void CSurf_ShowStandaloneConfig(HWND parent)
{
    if (!s_hInstForDlg) return;
    std::string cfg = CSurf_GetCurrentConfig();
    DialogBoxParamA(
        s_hInstForDlg,
        MAKEINTRESOURCEA(IDD_CSURF_STANDALONE),
        parent,
        CSurfConfigDlgProc,
        (LPARAM)cfg.c_str());
}