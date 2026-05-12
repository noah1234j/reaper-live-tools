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
#include "CSurfDebugWnd.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
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

// Forward declare wizard (defined in CSurfWizardDlg.cpp)
void CSurf_ShowWizard(HWND parent, HINSTANCE hInst);

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
        const char* protoStr = (s_dlgMainPort.proto == CSurfProtocol::HUI) ? "HUI"
                              :(s_dlgMainPort.proto == CSurfProtocol::FP16) ? "FP16"
                              :(s_dlgMainPort.proto == CSurfProtocol::RAW)  ? "RAW" : "MCU";
        snprintf(line, sizeof(line), "Main [%s]:  In=%s  Out=%s", protoStr, inName, outName);
        SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)line);
    }
    for (int i = 0; i < (int)s_dlgExtenders.size(); ++i)
    {
        char inName[64], outName[64], line[192];
        GetMidiInName (s_dlgExtenders[i].midiInDev,  inName,  sizeof(inName));
        GetMidiOutName(s_dlgExtenders[i].midiOutDev, outName, sizeof(outName));
        const char* protoStr = (s_dlgExtenders[i].proto == CSurfProtocol::HUI) ? "HUI"
                              :(s_dlgExtenders[i].proto == CSurfProtocol::FP16) ? "FP16"
                              :(s_dlgExtenders[i].proto == CSurfProtocol::RAW)  ? "RAW" : "MCU";
        snprintf(line, sizeof(line), "Ext %d [%s]:  In=%s  Out=%s  Ch+%d",
            i + 1, protoStr, inName, outName, s_dlgExtenders[i].channelOffset);
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

        // Channel offset
        HWND hOffsetEdit = GetDlgItem(hDlg, IDC_EXT_CHAN_OFFSET);
        HWND hOffsetSpin = GetDlgItem(hDlg, IDC_EXT_CHAN_OFFSET_SPIN);
        if (hOffsetEdit)  SetDlgItemInt(hDlg, IDC_EXT_CHAN_OFFSET, d->port.channelOffset, FALSE);
        if (hOffsetSpin)  SendMessageA(hOffsetSpin, UDM_SETRANGE32, 0, 127);
        if (hOffsetSpin && hOffsetEdit) SendMessageA(hOffsetSpin, UDM_SETBUDDY, (WPARAM)hOffsetEdit, 0);

        // Protocol radios
        {
            int radioId = IDC_EXT_PROTO_MCU;
            switch (d->port.proto)
            {
            case CSurfProtocol::HUI:  radioId = IDC_EXT_PROTO_HUI; break;
            case CSurfProtocol::RAW:  radioId = IDC_EXT_PROTO_RAW; break;
            default: break; // MCU
            }
            if (GetDlgItem(hDlg, IDC_EXT_PROTO_MCU))
                CheckRadioButton(hDlg, IDC_EXT_PROTO_MCU, IDC_EXT_PROTO_RAW, radioId);
        }

        // Device preset combo
        if (HWND hPreset = GetDlgItem(hDlg, IDC_EXT_DEVICE_PRESET))
        {
            SendMessageA(hPreset, CB_ADDSTRING, 0, (LPARAM)"(default for protocol)");
            SendMessageA(hPreset, CB_SETITEMDATA, 0, (LPARAM)-1);
            for (int i = 0; i < k_csurfTemplateCount; ++i)
            {
                int idx = (int)SendMessageA(hPreset, CB_ADDSTRING, 0, (LPARAM)k_csurfTemplates[i].name);
                SendMessageA(hPreset, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
            }
            // Select current preset
            int sel = 0;
            if (d->port.devicePreset >= 0)
                sel = d->port.devicePreset + 1;  // +1 because 0 = "(default)"
            SendMessageA(hPreset, CB_SETCURSEL, (WPARAM)sel, 0);
        }

        return TRUE;
    }

    case WM_COMMAND:
    {
        int ctrl = LOWORD(wParam);
        int notif = HIWORD(wParam);

        // Auto-set protocol when device preset changes
        if (ctrl == IDC_EXT_DEVICE_PRESET && notif == CBN_SELCHANGE)
        {
            HWND hPreset = GetDlgItem(hDlg, IDC_EXT_DEVICE_PRESET);
            int sel = (int)SendMessageA(hPreset, CB_GETCURSEL, 0, 0);
            int tmplIdx = (int)SendMessageA(hPreset, CB_GETITEMDATA, (WPARAM)sel, 0);
            if (tmplIdx >= 0 && tmplIdx < k_csurfTemplateCount && GetDlgItem(hDlg, IDC_EXT_PROTO_MCU))
            {
                int radioId = IDC_EXT_PROTO_MCU;
                switch (k_csurfTemplates[tmplIdx].proto)
                {
                case CSurfProtocol::HUI:  radioId = IDC_EXT_PROTO_HUI; break;
                case CSurfProtocol::FP16: radioId = IDC_EXT_PROTO_RAW; break; // map FP16→RAW for ext
                default: break;
                }
                CheckRadioButton(hDlg, IDC_EXT_PROTO_MCU, IDC_EXT_PROTO_RAW, radioId);
            }
            return TRUE;
        }

        // Per-port Button Map button
        if (ctrl == IDC_EXT_BTN_MAP)
        {
            ExtDlgData* d = reinterpret_cast<ExtDlgData*>(GetWindowLongPtrA(hDlg, DWLP_USER));
            if (d && s_hInstForDlg)
            {
                // Determine protocol for correct button list
                CSurfProtocol proto = d->port.proto;
                if (GetDlgItem(hDlg, IDC_EXT_PROTO_MCU))
                {
                    if      (IsDlgButtonChecked(hDlg, IDC_EXT_PROTO_HUI) == BST_CHECKED) proto = CSurfProtocol::HUI;
                    else if (IsDlgButtonChecked(hDlg, IDC_EXT_PROTO_RAW) == BST_CHECKED) proto = CSurfProtocol::RAW;
                    else proto = CSurfProtocol::MCU;
                }
                char portLabel[64];
                snprintf(portLabel, sizeof(portLabel), "%s", d->title ? d->title : "Port");
                BtnMapWnd_ShowModal(hDlg, s_hInstForDlg, d->port.btnMap, proto, portLabel);
            }
            return TRUE;
        }

        if (ctrl == IDOK)
        {
            ExtDlgData* d = reinterpret_cast<ExtDlgData*>(GetWindowLongPtrA(hDlg, DWLP_USER));
            if (d)
            {
                d->port.midiInDev     = ComboGetDeviceId(GetDlgItem(hDlg, IDC_EXT_MIDI_IN));
                d->port.midiOutDev    = ComboGetDeviceId(GetDlgItem(hDlg, IDC_EXT_MIDI_OUT));
                d->port.channelOffset = (int)GetDlgItemInt(hDlg, IDC_EXT_CHAN_OFFSET, nullptr, FALSE);

                // Protocol radios
                if (GetDlgItem(hDlg, IDC_EXT_PROTO_MCU))
                {
                    if      (IsDlgButtonChecked(hDlg, IDC_EXT_PROTO_HUI) == BST_CHECKED) d->port.proto = CSurfProtocol::HUI;
                    else if (IsDlgButtonChecked(hDlg, IDC_EXT_PROTO_RAW) == BST_CHECKED) d->port.proto = CSurfProtocol::RAW;
                    else d->port.proto = CSurfProtocol::MCU;
                }

                // Device preset combo
                if (HWND hPreset = GetDlgItem(hDlg, IDC_EXT_DEVICE_PRESET))
                {
                    int sel = (int)SendMessageA(hPreset, CB_GETCURSEL, 0, 0);
                    d->port.devicePreset = (sel > 0) ? (int)SendMessageA(hPreset, CB_GETITEMDATA, (WPARAM)sel, 0) : -1;
                }
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (ctrl == IDCANCEL)
        {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

static CSurfSettings ReadSettings(HWND hDlg)
{
    CSurfSettings s;
    // proto, templateIdx, channelCount come from per-port data (main port)
    s.proto        = s_dlgMainPort.proto;
    s.templateIdx  = s_dlgMainPort.devicePreset;
    s.channelCount = (s.templateIdx >= 0 && s.templateIdx < k_csurfTemplateCount)
        ? k_csurfTemplates[s.templateIdx].channelCount : 8;

    s.midiInDev  = s_dlgMainPort.midiInDev;
    s.midiOutDev = s_dlgMainPort.midiOutDev;

    s.sendColors          = true; // always on
    s.followSel           = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_SEL)    == BST_CHECKED;
    s.followMCP           = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_MCP)    == BST_CHECKED;
    s.followLayers        = IsDlgButtonChecked(hDlg, IDC_CSURF_FOLLOW_LAYERS) == BST_CHECKED;
    s.sendsSpillReceives  = IsDlgButtonChecked(hDlg, IDC_CSURF_SENDS_SPILL)   == BST_CHECKED;
    s.showTouchedChannels = IsDlgButtonChecked(hDlg, IDC_CSURF_TOUCH_CHAN)    == BST_CHECKED;
    s.debugLog            = IsDlgButtonChecked(hDlg, IDC_CSURF_DEBUG_LOG)     == BST_CHECKED;
    s.sendsDisplayMode    = (IsDlgButtonChecked(hDlg, IDC_CSURF_SENDS_CREATE) == BST_CHECKED) ? 1 : 0;

    s.extenders  = s_dlgExtenders;
    s.btnMap     = s_dlgBtnMap;
    s.bankOffset = 0;
    return s;
}

// ---------------------------------------------------------------------------
// Per-port context-menu action helpers
// ---------------------------------------------------------------------------
static void DoEditPort(HWND hDlg, int sel)
{
    HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
    if (sel == 0)
    {
        ExtDlgData data{ s_dlgMainPort, true, "Edit Main Surface Port" };
        if (s_hInstForDlg &&
            DialogBoxParamA(s_hInstForDlg, MAKEINTRESOURCEA(IDD_CSURF_EXTENDER_EDIT),
                hDlg, ExtenderDlgProc, (LPARAM)&data) == IDOK)
        {
            s_dlgMainPort = data.port;
            RebuildPortList(hList);
            SendMessageA(hList, LB_SETCURSEL, 0, 0);
        }
    }
    else if (sel > 0 && sel - 1 < (int)s_dlgExtenders.size())
    {
        ExtDlgData data{ s_dlgExtenders[sel - 1], true, "Edit Extender Port" };
        if (s_hInstForDlg &&
            DialogBoxParamA(s_hInstForDlg, MAKEINTRESOURCEA(IDD_CSURF_EXTENDER_EDIT),
                hDlg, ExtenderDlgProc, (LPARAM)&data) == IDOK)
        {
            s_dlgExtenders[sel - 1] = data.port;
            RebuildPortList(hList);
            SendMessageA(hList, LB_SETCURSEL, (WPARAM)sel, 0);
        }
    }
}

static void DoRemovePort(HWND hDlg, int sel)
{
    if (sel <= 0 || sel - 1 >= (int)s_dlgExtenders.size()) return;
    HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
    s_dlgExtenders.erase(s_dlgExtenders.begin() + (sel - 1));
    RebuildPortList(hList);
    int newSel = (sel - 1 < (int)s_dlgExtenders.size()) ? sel : sel - 1;
    SendMessageA(hList, LB_SETCURSEL, (WPARAM)newSel, 0);
}

static void DoBtnMapForPort(HWND hDlg, int sel)
{
    if (!s_hInstForDlg) return;
    if (sel == 0)
    {
        BtnMapWnd_ShowModal(hDlg, s_hInstForDlg, s_dlgMainPort.btnMap,
            s_dlgMainPort.proto, "Main Surface");
    }
    else if (sel > 0 && sel - 1 < (int)s_dlgExtenders.size())
    {
        ExtenderPort& ext = s_dlgExtenders[sel - 1];
        char label[64];
        snprintf(label, sizeof(label), "Extender %d", sel);
        BtnMapWnd_ShowModal(hDlg, s_hInstForDlg, ext.btnMap, ext.proto, label);
    }
}

static void DoExportSettings(HWND hDlg)
{
    CSurfSettings s = ReadSettings(hDlg);
    char path[MAX_PATH] = {};
    lstrcpynA(path, "LiveTools_Surface.ltcfg", MAX_PATH);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hDlg;
    ofn.lpstrFilter = "Live Tools Config\0*.ltcfg\0All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = "Export Settings";
    ofn.lpstrDefExt = "ltcfg";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) return;
    FILE* f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "# Live Tools Control Surface Settings Export\n");
        fprintf(f, "%s\n", s.Serialize().c_str());
        fclose(f);
    }
}

static void DoImportSettings(HWND hDlg)
{
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hDlg;
    ofn.lpstrFilter = "Live Tools Config\0*.ltcfg\0All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = "Import Settings";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char lineBuf[2048] = {};
    while (fgets(lineBuf, sizeof(lineBuf), f))
    {
        if (lineBuf[0] == '#' || lineBuf[0] == '\n' || lineBuf[0] == '\r') continue;
        char* nl = strrchr(lineBuf, '\n');
        if (nl) *nl = '\0';
        CSurfSettings imported = CSurfSettings::Deserialize(lineBuf);
        s_dlgBtnMap    = imported.btnMap;
        s_dlgMainPort  = { imported.midiInDev, imported.midiOutDev, 0,
                           imported.proto, imported.templateIdx, imported.btnMap };
        s_dlgExtenders = imported.extenders;
        RebuildPortList(GetDlgItem(hDlg, IDC_CSURF_EXT_LIST));
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_SEL,    imported.followSel           ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_MCP,    imported.followMCP           ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_LAYERS, imported.followLayers        ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_SENDS_SPILL,   imported.sendsSpillReceives  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_TOUCH_CHAN,     imported.showTouchedChannels ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_DEBUG_LOG,      imported.debugLog            ? BST_CHECKED : BST_UNCHECKED);
        if (GetDlgItem(hDlg, IDC_CSURF_SENDS_ACTIVE))
            CheckRadioButton(hDlg, IDC_CSURF_SENDS_ACTIVE, IDC_CSURF_SENDS_CREATE,
                imported.sendsDisplayMode == 1 ? IDC_CSURF_SENDS_CREATE : IDC_CSURF_SENDS_ACTIVE);
        break;
    }
    fclose(f);
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
        s_dlgMainPort  = { s.midiInDev, s.midiOutDev, 0, s.proto, s.templateIdx, s.btnMap };
        s_dlgExtenders = s.extenders;

        RebuildPortList(GetDlgItem(hDlg, IDC_CSURF_EXT_LIST));
        SendMessageA(GetDlgItem(hDlg, IDC_CSURF_EXT_LIST), LB_SETCURSEL, 0, 0);

        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_SEL,    s.followSel           ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_MCP,    s.followMCP           ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_FOLLOW_LAYERS, s.followLayers        ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_SENDS_SPILL,   s.sendsSpillReceives  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_TOUCH_CHAN,     s.showTouchedChannels ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CSURF_DEBUG_LOG,      s.debugLog            ? BST_CHECKED : BST_UNCHECKED);
        if (GetDlgItem(hDlg, IDC_CSURF_SENDS_ACTIVE))
            CheckRadioButton(hDlg, IDC_CSURF_SENDS_ACTIVE, IDC_CSURF_SENDS_CREATE,
                s.sendsDisplayMode == 1 ? IDC_CSURF_SENDS_CREATE : IDC_CSURF_SENDS_ACTIVE);
        if (HWND hDbg = GetDlgItem(hDlg, IDC_CSURF_DBG_OPEN))
            SetWindowTextA(hDbg, CSurfDebug_IsOpen() ? "Close Log" : "Open Log");

        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CSURF_EXT_ADD)
        {
            int defaultOffset = (int)(1 + s_dlgExtenders.size()) * 8;
            ExtDlgData data{ { -1, -1, defaultOffset }, false, "Add Extender Port" };
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
            }
        }
        else if (LOWORD(wParam) == IDC_CSURF_EXT_LIST && HIWORD(wParam) == LBN_DBLCLK)
        {
            HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
            int sel = (int)SendMessageA(hList, LB_GETCURSEL, 0, 0);
            if (sel >= 0) DoEditPort(hDlg, sel);
        }
        // ---- Debug Log toggle ---------------------------------------------
        else if (LOWORD(wParam) == IDC_CSURF_DBG_OPEN)
        {
            if (CSurfDebug_IsOpen())
            {
                CSurfDebug_Close();
                SetDlgItemTextA(hDlg, IDC_CSURF_DBG_OPEN, "Open Log");
            }
            else
            {
                CSurfDebug_Open(hDlg, s_hInstForDlg);
                CSurfSettings s = ReadSettings(hDlg);
                CSurfDebug_DumpSettings(s);
                SetDlgItemTextA(hDlg, IDC_CSURF_DBG_OPEN, "Close Log");
            }
        }
        // ---- Setup Wizard -------------------------------------------------
        else if (LOWORD(wParam) == IDC_CSURF_WIZARD_BTN)
        {
            if (s_hInstForDlg)
                CSurf_ShowWizard(hDlg, s_hInstForDlg);
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

    case WM_CONTEXTMENU:
    {
        HWND hList = GetDlgItem(hDlg, IDC_CSURF_EXT_LIST);
        if ((HWND)wParam != hList) break;

        // Auto-select item under cursor (if right-click, not keyboard)
        POINT screenPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (lParam != (LPARAM)-1)
        {
            POINT clientPt = screenPt;
            ScreenToClient(hList, &clientPt);
            LRESULT hit = SendMessageA(hList, LB_ITEMFROMPOINT, 0,
                MAKELPARAM(clientPt.x, clientPt.y));
            if (HIWORD(hit) == 0)
                SendMessageA(hList, LB_SETCURSEL, LOWORD(hit), 0);
        }
        else
        {
            int cur = (int)SendMessageA(hList, LB_GETCURSEL, 0, 0);
            if (cur >= 0)
            {
                RECT ir;
                SendMessageA(hList, LB_GETITEMRECT, (WPARAM)cur, (LPARAM)&ir);
                MapWindowPoints(hList, nullptr, (POINT*)&ir, 2);
                screenPt = { ir.left + 8, ir.top + 6 };
            }
        }

        int sel     = (int)SendMessageA(hList, LB_GETCURSEL, 0, 0);
        bool hasSel = (sel >= 0);
        bool isMain = (sel == 0);

        HMENU hMenu = CreatePopupMenu();
        AppendMenuA(hMenu, MF_STRING | (hasSel ? 0 : MF_GRAYED),              1, "Edit Port...");
        AppendMenuA(hMenu, MF_STRING | (!hasSel || isMain ? MF_GRAYED : 0),   2, "Remove Port");
        AppendMenuA(hMenu, MF_STRING | (hasSel ? 0 : MF_GRAYED),              3, "Button Map...");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING, 4, "Export Settings...");
        AppendMenuA(hMenu, MF_STRING, 5, "Import Settings...");

        int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPt.x, screenPt.y, 0, hDlg, nullptr);
        DestroyMenu(hMenu);

        switch (cmd)
        {
        case 1: DoEditPort(hDlg, sel);      break;
        case 2: DoRemovePort(hDlg, sel);    break;
        case 3: DoBtnMapForPort(hDlg, sel); break;
        case 4: DoExportSettings(hDlg);     break;
        case 5: DoImportSettings(hDlg);     break;
        }
        return TRUE;
    }

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