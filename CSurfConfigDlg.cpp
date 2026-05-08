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
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cstdio>
#include <string>

// Module instance - set from ReaperPluginEntry before any dialog is shown
static HINSTANCE s_hInstForDlg = nullptr;

void CSurfConfigDlg_SetInstance(HINSTANCE hInst)
{
    s_hInstForDlg = hInst;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void PopulateMIDIDevices(HWND hDlg, int selIn, int selOut, int selIn2, int selOut2)
{
    HWND hIn   = GetDlgItem(hDlg, IDC_CSURF_MIDI_IN);
    HWND hOut  = GetDlgItem(hDlg, IDC_CSURF_MIDI_OUT);
    HWND hIn2  = GetDlgItem(hDlg, IDC_CSURF_MIDI_IN2);
    HWND hOut2 = GetDlgItem(hDlg, IDC_CSURF_MIDI_OUT2);

    auto fillInputs = [&](HWND hw, int selDev)
    {
        SendMessageA(hw, CB_RESETCONTENT, 0, 0);
        int idx = (int)SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"(none)");
        SendMessageA(hw, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)-1);
        SendMessageA(hw, CB_SETCURSEL, 0, 0);
        int n = GetNumMIDIInputs ? GetNumMIDIInputs() : 0;
        for (int i = 0; i < n; ++i)
        {
            char name[256] = "";
            if (GetMIDIInputName) GetMIDIInputName(i, name, (int)sizeof(name));
            if (!name[0]) snprintf(name, sizeof(name), "MIDI Input %d", i);
            int ci = (int)SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)name);
            SendMessageA(hw, CB_SETITEMDATA, (WPARAM)ci, (LPARAM)i);
            if (i == selDev) SendMessageA(hw, CB_SETCURSEL, (WPARAM)ci, 0);
        }
    };

    auto fillOutputs = [&](HWND hw, int selDev)
    {
        SendMessageA(hw, CB_RESETCONTENT, 0, 0);
        int idx = (int)SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"(none)");
        SendMessageA(hw, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)-1);
        SendMessageA(hw, CB_SETCURSEL, 0, 0);
        int n = GetNumMIDIOutputs ? GetNumMIDIOutputs() : 0;
        for (int i = 0; i < n; ++i)
        {
            char name[256] = "";
            if (GetMIDIOutputName) GetMIDIOutputName(i, name, (int)sizeof(name));
            if (!name[0]) snprintf(name, sizeof(name), "MIDI Output %d", i);
            int ci = (int)SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)name);
            SendMessageA(hw, CB_SETITEMDATA, (WPARAM)ci, (LPARAM)i);
            if (i == selDev) SendMessageA(hw, CB_SETCURSEL, (WPARAM)ci, 0);
        }
    };

    fillInputs (hIn,   selIn);
    fillOutputs(hOut,  selOut);
    if (hIn2)  fillInputs (hIn2,  selIn2);
    if (hOut2) fillOutputs(hOut2, selOut2);
}

static int ComboGetDeviceId(HWND hCombo)
{
    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return -1;
    return (int)SendMessageA(hCombo, CB_GETITEMDATA, (WPARAM)sel, 0);
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

    s.midiInDev  = ComboGetDeviceId(GetDlgItem(hDlg, IDC_CSURF_MIDI_IN));
    s.midiOutDev = ComboGetDeviceId(GetDlgItem(hDlg, IDC_CSURF_MIDI_OUT));
    s.midiInDev2  = ComboGetDeviceId(GetDlgItem(hDlg, IDC_CSURF_MIDI_IN2));
    s.midiOutDev2 = ComboGetDeviceId(GetDlgItem(hDlg, IDC_CSURF_MIDI_OUT2));

    s.followSel = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_SEL) == BST_CHECKED;
    s.showVU    = IsDlgButtonChecked(hDlg, IDC_CSURF_SHOW_VU)    == BST_CHECKED;
    s.showNames = IsDlgButtonChecked(hDlg, IDC_CSURF_SHOW_NAMES) == BST_CHECKED;

    if      (IsDlgButtonChecked(hDlg, IDC_CSURF_FADER_PAN)  == BST_CHECKED) s.faderMode = 1;
    else if (IsDlgButtonChecked(hDlg, IDC_CSURF_FADER_SEND) == BST_CHECKED) s.faderMode = 2;
    else                                                                       s.faderMode = 0;

    s.sendColors = IsDlgButtonChecked(hDlg, IDC_CSURF_SEND_COLORS) == BST_CHECKED;
    s.followMCP  = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_MCP)  == BST_CHECKED;

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

        HWND hTmpl = GetDlgItem(hDlg, IDC_CSURF_TEMPLATE);
        for (int i = 0; i < k_csurfTemplateCount; ++i)
            SendMessageA(hTmpl, CB_ADDSTRING, 0, (LPARAM)k_csurfTemplates[i].name);
        int tmplSel = (s.templateIdx >= 0 && s.templateIdx < k_csurfTemplateCount)
            ? s.templateIdx : 0;
        SendMessageA(hTmpl, CB_SETCURSEL, (WPARAM)tmplSel, 0);

        CheckRadioButton(hDlg, IDC_CSURF_PROTO_MCU, IDC_CSURF_PROTO_HUI,
            s.proto == CSurfProtocol::HUI ? IDC_CSURF_PROTO_HUI : IDC_CSURF_PROTO_MCU);

        PopulateMIDIDevices(hDlg, s.midiInDev, s.midiOutDev, s.midiInDev2, s.midiOutDev2);

        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_SEL, s.followSel ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_SHOW_VU,    s.showVU    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_SHOW_NAMES, s.showNames ? BST_CHECKED : BST_UNCHECKED);

        CheckRadioButton(hDlg, IDC_CSURF_FADER_VOL, IDC_CSURF_FADER_SEND,
            s.faderMode == 1 ? IDC_CSURF_FADER_PAN  :
            s.faderMode == 2 ? IDC_CSURF_FADER_SEND : IDC_CSURF_FADER_VOL);

        CheckDlgButton(hDlg, IDC_CSURF_SEND_COLORS, s.sendColors ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_MCP,  s.followMCP  ? BST_CHECKED : BST_UNCHECKED);

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