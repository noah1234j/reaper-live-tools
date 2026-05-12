// ---------------------------------------------------------------------------
// CSurfWizardDlg.cpp – 5-step setup wizard for the control surface
// ---------------------------------------------------------------------------
#include "ControlSurface.h"
#include "resource.h"
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cstdio>

// Apply settings to live instance (defined in ControlSurface.cpp)
void CSurf_ApplySettings(const CSurfSettings& s);

// ---------------------------------------------------------------------------
static constexpr int kNumSteps = 5;

static const char* k_stepTitles[kNumSteps] = {
    "Welcome",
    "Enable MIDI in REAPER",
    "Select Surface & MIDI Ports",
    "Options",
    "Summary"
};

static const char* k_stepText[kNumSteps] = {
    "Welcome to the Live Tools Control Surface Setup Wizard!\r\n\r\n"
    "This wizard will walk you through connecting and configuring your MIDI control surface.\r\n\r\n"
    "You will need:\r\n"
    "  \x95 A MIDI control surface (MCU, HUI, FaderPort, Behringer X-Touch, etc.)\r\n"
    "  \x95 It connected via USB or a hardware MIDI interface\r\n"
    "  \x95 MIDI enabled in REAPER Preferences\r\n\r\n"
    "Click Next to begin.",

    "Before REAPER can communicate with your surface, its MIDI ports must be\r\n"
    "enabled in REAPER's Preferences.\r\n\r\n"
    "Steps:\r\n"
    "  1. Open REAPER \x96 go to Options > Preferences > MIDI Devices\r\n"
    "  2. Find your surface's MIDI input in the list (shown as a USB or MIDI device)\r\n"
    "  3. Double-click the input to enable it\r\n"
    "  4. Repeat for the MIDI output\r\n"
    "  5. Click OK and return here\r\n\r\n"
    "Note: After enabling, MIDI device names will appear in the dropdowns on the next step.",

    // Step 3: controls shown dynamically (combo boxes), text is a label only
    "Select your surface type and choose the MIDI input and output ports\r\n"
    "that correspond to your physical device.\r\n\r\n"
    "If your device does not appear in the template list, choose the closest\r\n"
    "match (e.g. \"MCU Compatible\" for most Mackie-protocol devices).\r\n"
    "You can customise further from the main Settings dialog.",

    "Adjust these optional settings to your preference.\r\n"
    "All settings can be changed later in the Settings dialog.\r\n\r\n"
    "  Follow Track Selection:\r\n"
    "    Bank the surface automatically when you select a track outside\r\n"
    "    the current view.\r\n\r\n"
    "  Send Track Colors:\r\n"
    "    Push REAPER track colors to the surface (if supported).\r\n\r\n"
    "  Sends Spill to Receives:\r\n"
    "    When you press the Sends button, spill the selected track's\r\n"
    "    receive sends onto the fader bank.",

    // Step 5: filled dynamically
    nullptr
};

// ---------------------------------------------------------------------------
struct WizardData
{
    int          step         = 0;
    int          templateIdx  = 0;   // chosen template
    int          midiIn       = -1;  // chosen MIDI input index
    int          midiOut      = -1;  // chosen MIDI output index
    bool         followSel    = true;
    bool         sendColors   = false;
    bool         sendsSpill   = false;
};

static void PopulateMIDIIn(HWND hCombo)
{
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"(none)");
    if (!GetNumMIDIInputs || !GetMIDIInputName) return;
    int n = GetNumMIDIInputs();
    char buf[256];
    for (int i = 0; i < n; ++i)
        if (GetMIDIInputName(i, buf, sizeof(buf)))
            SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
}

static void PopulateMIDIOut(HWND hCombo)
{
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"(none)");
    if (!GetNumMIDIOutputs || !GetMIDIOutputName) return;
    int n = GetNumMIDIOutputs();
    char buf[256];
    for (int i = 0; i < n; ++i)
        if (GetMIDIOutputName(i, buf, sizeof(buf)))
            SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
}

static void PopulateTemplates(HWND hCombo)
{
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < k_csurfTemplateCount; ++i)
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)k_csurfTemplates[i].name);
    if (k_csurfTemplateCount > 0) SendMessage(hCombo, CB_SETCURSEL, 0, 0);
}

static void ShowStep3Controls(HWND hwnd, bool show)
{
    int sw = show ? SW_SHOW : SW_HIDE;
    ShowWindow(GetDlgItem(hwnd, IDC_WIZ_TMPL_LABEL), sw);
    ShowWindow(GetDlgItem(hwnd, IDC_WIZ_TMPL),       sw);
    ShowWindow(GetDlgItem(hwnd, IDC_WIZ_IN_LABEL),   sw);
    ShowWindow(GetDlgItem(hwnd, IDC_WIZ_MIDI_IN),    sw);
    ShowWindow(GetDlgItem(hwnd, IDC_WIZ_OUT_LABEL),  sw);
    ShowWindow(GetDlgItem(hwnd, IDC_WIZ_MIDI_OUT),   sw);
}

static void UpdateStep(HWND hwnd, WizardData* d)
{
    // Step indicator
    char stepBuf[32];
    snprintf(stepBuf, sizeof(stepBuf), "Step %d / %d", d->step + 1, kNumSteps);
    SetDlgItemText(hwnd, IDC_WIZ_STEP, stepBuf);

    // Window title reflects step
    char title[64];
    snprintf(title, sizeof(title), "Setup Wizard \x96 %s", k_stepTitles[d->step]);
    SetWindowText(hwnd, title);

    // Content area
    bool isStep3 = (d->step == 2);
    ShowStep3Controls(hwnd, isStep3);

    if (d->step == 4) // Summary
    {
        char buf[512];
        const char* tmplName = (d->templateIdx >= 0 && d->templateIdx < k_csurfTemplateCount)
            ? k_csurfTemplates[d->templateIdx].name : "(none)";
        char inName[128] = "(none)", outName[128] = "(none)";
        if (GetMIDIInputName  && d->midiIn  >= 0) GetMIDIInputName (d->midiIn,  inName,  sizeof(inName));
        if (GetMIDIOutputName && d->midiOut >= 0) GetMIDIOutputName(d->midiOut, outName, sizeof(outName));
        snprintf(buf, sizeof(buf),
            "Setup Summary:\r\n\r\n"
            "  Surface type : %s\r\n"
            "  MIDI Input   : %s\r\n"
            "  MIDI Output  : %s\r\n\r\n"
            "  Follow Selection : %s\r\n"
            "  Send Colors      : %s\r\n"
            "  Sends Spill      : %s\r\n\r\n"
            "Click Finish to apply these settings.\r\n"
            "You can change them later in the Settings dialog.",
            tmplName, inName, outName,
            d->followSel  ? "Yes" : "No",
            d->sendColors ? "Yes" : "No",
            d->sendsSpill ? "Yes" : "No");
        SetDlgItemText(hwnd, IDC_WIZ_CONTENT, buf);
    }
    else if (!isStep3)
    {
        SetDlgItemText(hwnd, IDC_WIZ_CONTENT, k_stepText[d->step]);
    }

    // Back button
    EnableWindow(GetDlgItem(hwnd, IDC_WIZ_BACK), d->step > 0);

    // Next / Finish label
    SetDlgItemText(hwnd, IDC_WIZ_NEXT,
        (d->step == kNumSteps - 1) ? "Finish" : "Next >");
}

static void ReadStep3(HWND hwnd, WizardData* d)
{
    // Template combo: index 0 = first template
    int sel = (int)SendDlgItemMessage(hwnd, IDC_WIZ_TMPL, CB_GETCURSEL, 0, 0);
    d->templateIdx = (sel == CB_ERR) ? 0 : sel;

    // MIDI In combo: index 0 = "(none)", 1 = device 0, etc.
    int inSel = (int)SendDlgItemMessage(hwnd, IDC_WIZ_MIDI_IN, CB_GETCURSEL, 0, 0);
    d->midiIn = (inSel <= 0) ? -1 : inSel - 1;

    int outSel = (int)SendDlgItemMessage(hwnd, IDC_WIZ_MIDI_OUT, CB_GETCURSEL, 0, 0);
    d->midiOut = (outSel <= 0) ? -1 : outSel - 1;
}

static INT_PTR CALLBACK WizardDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    WizardData* d = (WizardData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        d = new WizardData();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)d);

        // Populate step-3 combos (hidden initially)
        PopulateTemplates(GetDlgItem(hwnd, IDC_WIZ_TMPL));
        PopulateMIDIIn  (GetDlgItem(hwnd, IDC_WIZ_MIDI_IN));
        PopulateMIDIOut (GetDlgItem(hwnd, IDC_WIZ_MIDI_OUT));

        // Step 4 checkboxes (always visible in content area — use text for now)
        // They live in the content EDIT; step 4 expands them. For proper checkboxes
        // we repurpose 3 hidden BUTTON controls — but here we use the text-only approach.
        UpdateStep(hwnd, d);
        return TRUE;
    }

    case WM_COMMAND:
        if (!d) return FALSE;
        switch (LOWORD(wParam))
        {
        case IDC_WIZ_BACK:
            if (d->step > 0) { d->step--; UpdateStep(hwnd, d); }
            return TRUE;

        case IDC_WIZ_NEXT:
        {
            // Save data from current step before advancing
            if (d->step == 2) ReadStep3(hwnd, d);

            if (d->step == kNumSteps - 1)
            {
                // Finish — build and apply settings
                CSurfSettings s;
                s.templateIdx     = d->templateIdx;
                s.midiInDev       = d->midiIn;
                s.midiOutDev      = d->midiOut;
                s.followSel       = d->followSel;
                s.sendColors      = d->sendColors;
                s.sendsSpillReceives = d->sendsSpill;
                if (d->templateIdx >= 0 && d->templateIdx < k_csurfTemplateCount)
                {
                    s.channelCount = k_csurfTemplates[d->templateIdx].channelCount;
                    s.proto        = k_csurfTemplates[d->templateIdx].proto;
                }
                CSurf_ApplySettings(s);
                EndDialog(hwnd, IDOK);
            }
            else
            {
                d->step++;
                UpdateStep(hwnd, d);
            }
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        delete d;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
void CSurf_ShowWizard(HWND parent, HINSTANCE hInst)
{
    DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_CSURF_WIZARD),
                   parent, WizardDlgProc, 0);
}
