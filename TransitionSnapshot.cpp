#include "TransitionSnapshot.h"
#include "LayersEngine.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
TransitionSnapshot::TransitionSnapshot(int slot, const char* name)
    : m_slot(slot)
    , m_name(name ? name : "")
    , m_mask(0)
    , m_time((int)std::time(nullptr))
{
}

// ---------------------------------------------------------------------------
// Helpers: GUID <-> string (Windows-only, no WDL required)
// ---------------------------------------------------------------------------
static std::string GuidToString(const GUID& g)
{
    WCHAR wbuf[64];
    StringFromGUID2(g, wbuf, 64);
    char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 64, nullptr, nullptr);
    return buf;
}

static GUID StringToGuid(const char* s)
{
    GUID g = {};
    if (!s || !s[0]) return g;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return g;
    std::vector<WCHAR> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wbuf.data(), wlen);
    CLSIDFromString(wbuf.data(), &g);
    return g;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------
void TransitionSnapshot::Capture(int mask)
{
    m_mask = mask;
    m_tracks.clear();

    const int numTracks = GetNumTracks();
    m_tracks.reserve(numTracks);

    for (int i = 0; i < numTracks; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;

        TrackState ts;

        // Always capture GUID (needed to re-match the track on recall)
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg) ts.guid = *pg;

        if (mask & TS_VOL)
        {
            double* pv = (double*)GetSetMediaTrackInfo(tr, "D_VOL", nullptr);
            if (pv) ts.vol = *pv;
        }

        if (mask & TS_PAN)
        {
            double* pp  = (double*)GetSetMediaTrackInfo(tr, "D_PAN",      nullptr);
            int*    pm  = (int*)   GetSetMediaTrackInfo(tr, "I_PANMODE",  nullptr);
            double* pw  = (double*)GetSetMediaTrackInfo(tr, "D_WIDTH",    nullptr);
            double* pdl = (double*)GetSetMediaTrackInfo(tr, "D_DUALPANL", nullptr);
            double* pdr = (double*)GetSetMediaTrackInfo(tr, "D_DUALPANR", nullptr);
            double* pl  = (double*)GetSetMediaTrackInfo(tr, "D_PANLAW",   nullptr);
            if (pp)  ts.pan      = *pp;
            if (pm)  ts.panMode  = *pm;
            if (pw)  ts.width    = *pw;
            if (pdl) ts.dualPanL = *pdl;
            if (pdr) ts.dualPanR = *pdr;
            if (pl)  ts.panLaw   = *pl;
        }

        if (mask & TS_MUTE)
        {
            bool* pm = (bool*)GetSetMediaTrackInfo(tr, "B_MUTE", nullptr);
            if (pm) ts.mute = *pm;
        }

        if (mask & TS_SOLO)
        {
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
            if (ps) ts.solo = *ps;
        }

        if (mask & TS_PHASE)
        {
            bool* pp = (bool*)GetSetMediaTrackInfo(tr, "B_PHASE", nullptr);
            if (pp) ts.phase = *pp;
        }

        if (mask & TS_VIS)
        {
            int* mixer = (int*)GetSetMediaTrackInfo(tr, "I_SHOWINMIXER", nullptr);
            int* tcp   = (int*)GetSetMediaTrackInfo(tr, "I_SHOWINTCP",   nullptr);
            int mv = mixer ? *mixer : 1;
            int tv = tcp   ? *tcp   : 1;
            ts.vis = (mv & 1) | ((tv & 1) << 1);
        }

        if (mask & TS_SELECTION)
        {
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SELECTED", nullptr);
            if (ps) ts.selected = *ps;
        }

        if (mask & TS_PLAY_OFFSET)
        {
            int*    pf = (int*)   GetSetMediaTrackInfo(tr, "I_PLAY_OFFSET_FLAG", nullptr);
            double* po = (double*)GetSetMediaTrackInfo(tr, "D_PLAY_OFFSET",      nullptr);
            if (pf) ts.playOffsetFlag = *pf;
            if (po) ts.playOffset     = *po;
        }

        // Layout properties (instant-only, no lerp)
        if (mask & TS_TRACKNAME)
        {
            char nm[512] = {};
            if (GetTrackName(tr, nm, sizeof(nm)))
                ts.trackName = nm;
        }
        if (mask & TS_TRACKCOLOR)
        {
            int* pc = (int*)GetSetMediaTrackInfo(tr, "I_CUSTOMCOLOR", nullptr);
            if (pc) ts.color = *pc;
        }
        if (mask & TS_TRACKHEIGHT)
        {
            int*  ph = (int*) GetSetMediaTrackInfo(tr, "I_HEIGHTOVERRIDE", nullptr);
            bool* pl = (bool*)GetSetMediaTrackInfo(tr, "B_HEIGHTLOCK",     nullptr);
            if (ph) ts.heightOverride = *ph;
            if (pl) ts.heightLocked   = *pl;
        }
        // capturedIndex is set after the loop (needs final i)
        if (mask & TS_TRACKORDER)
            ts.capturedIndex = i;

        // FX params – live-safe path (no chunk ops)
        if (mask & TS_FXPARAMS)
        {
            const int nfx = TrackFX_GetCount(tr);
            ts.fx.reserve(nfx);
            for (int fx = 0; fx < nfx; fx++)
            {
                FXState fs;
                TrackFX_GetFXName(tr, fx, fs.name, (int)sizeof(fs.name));
                fs.slotIndex  = fx;
                fs.paramCount = TrackFX_GetNumParams(tr, fx);
                fs.enabled    = TrackFX_GetEnabled(tr, fx);
                fs.normVals.resize(fs.paramCount);
                for (int p = 0; p < fs.paramCount; p++)
                    fs.normVals[p] = TrackFX_GetParamNormalized(tr, fx, p);
                ts.fx.push_back(std::move(fs));
            }
        }

        // Offline FX chain chunk (only when explicitly requested AND not recording)
        if ((mask & TS_FXCHAIN) && !(GetPlayState() & 4))
        {
            char* chunk = GetSetObjectState2(tr, nullptr, false);
            if (chunk)
            {
                ts.fxChainChunk = chunk;
                FreeHeapPtr(chunk);
            }
        }

        m_tracks.push_back(std::move(ts));
    }

    // --- Full layer state ---------------------------------------------------
    // Always capture all layers regardless of recall preference.
    // The recall path decides whether to apply them.
    {
        LayersEngine& le = LayersEngine::Get();
        m_layerIdx = le.GetActiveLayer();
        m_layers.clear();
        for (int li = 0; li < le.GetLayerCount(); li++)
        {
            const LayerDef& ld = le.GetLayer(li);
            CapturedLayer cl;
            cl.name        = ld.name;
            cl.maxChannels = ld.maxChannels;
            for (const LayerTrack& lt : ld.tracks)
            {
                CapturedLayerTrack clt;
                clt.guid     = lt.guid;
                clt.isSpacer = lt.isSpacer;
                cl.tracks.push_back(clt);
            }
            m_layers.push_back(cl);
        }
    }
}

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------
void TransitionSnapshot::WriteParamLine(ProjectStateContext* ctx,
                                        const std::vector<double>& vals,
                                        int start, int end)
{
    char line[512];
    line[0] = 'P'; line[1] = '\0';
    for (int i = start; i < end; i++)
    {
        char num[32];
        snprintf(num, sizeof(num), " %.17g", vals[i]);
        strcat(line, num);
    }
    ctx->AddLine("%s", line);
}

// ---------------------------------------------------------------------------
// Serialize (called from SaveExtensionConfig)
// Format written to .RPP:
//   <TSSNAPSHOT <slot> <mask> <time> "<name>"
//     TRACK {GUID}
//     VOL <vol>
//     PAN <pan> <panMode> <width> <dualPanL> <dualPanR> <panLaw>
//     MISC <mute> <solo> <phase> <vis> <sel> <poFlag> <poVal>
//     FX <slot> <count> <enabled> "<name>"
//     P <val0> <val1> ... (up to 8 per line)
//     FXEND
//     TRACKEND
//   >
// ---------------------------------------------------------------------------
void TransitionSnapshot::Serialize(ProjectStateContext* ctx) const
{
    // Escape double-quotes in the name by replacing them with single-quotes
    // Spacer rows are serialized as a minimal two-line block
    if (m_isSpacer)
    {
        ctx->AddLine("<TSSPACER %d", m_slot);
        ctx->AddLine(">");
        return;
    }

    std::string safeName = m_name;
    for (char& c : safeName)
        if (c == '"') c = '\'';

    ctx->AddLine("<TSSNAPSHOT %d %d %d \"%s\"",
                 m_slot, m_mask, m_time, safeName.c_str());

    // Per-snapshot transition settings
    ctx->AddLine("DURATION %.4f %d %.4f", m_duration, (int)m_taper, m_taperExp);

    // Active layer index (old-format compat; always written)
    if (m_layerIdx >= 0)
        ctx->AddLine("LAYER %d", m_layerIdx);

    // Full layer state (new format – supersedes bare LAYER on recall)
    if (!m_layers.empty())
    {
        ctx->AddLine("LAYERCOUNT %d", (int)m_layers.size());
        for (const auto& cl : m_layers)
        {
            std::string safeName = cl.name;
            for (char& c : safeName) if (c == '"') c = '\'';
            ctx->AddLine("LAYERDEF \"%s\" %d", safeName.c_str(), cl.maxChannels);
            for (const auto& clt : cl.tracks)
            {
                if (clt.isSpacer)
                {
                    ctx->AddLine("LAYERSPACER");
                }
                else
                {
                    char guidStr[40];
                    LayersEngine::GuidToStr(clt.guid, guidStr);
                    ctx->AddLine("LAYERTRACK %s", guidStr);
                }
            }
            ctx->AddLine("LAYERDEFEND");
        }
    }

    // Optional notes (newlines collapsed to spaces for single-line storage)
    if (!m_notes.empty()) {
        std::string safeNotes = m_notes;
        for (char& c : safeNotes) {
            if (c == '"') c = '\'';
            if (c == '\n' || c == '\r') c = ' ';
        }
        ctx->AddLine("NOTES \"%s\"", safeNotes.c_str());
    }

    for (const auto& ts : m_tracks)
    {
        std::string sguid = GuidToString(ts.guid);
        ctx->AddLine("TRACK %s", sguid.c_str());
        ctx->AddLine("VOL %.17g", ts.vol);
        ctx->AddLine("PAN %.17g %d %.17g %.17g %.17g %.17g",
                     ts.pan, ts.panMode, ts.width,
                     ts.dualPanL, ts.dualPanR, ts.panLaw);
        ctx->AddLine("MISC %d %d %d %d %d %d %.17g",
                     (int)ts.mute, ts.solo, (int)ts.phase,
                     ts.vis, ts.selected,
                     ts.playOffsetFlag, ts.playOffset);

        // Layout: name, color, height, heightLocked, capturedIndex
        {
            std::string safeName2 = ts.trackName;
            for (char& c : safeName2) if (c == '"') c = '\'';
            ctx->AddLine("LAYOUT %d %d %d %d \"%s\"",
                         ts.color, ts.heightOverride, (int)ts.heightLocked,
                         ts.capturedIndex, safeName2.c_str());
        }

        for (const auto& fx : ts.fx)
        {
            // Escape quotes in FX name
            std::string safeFxName = fx.name;
            for (char& c : safeFxName)
                if (c == '"') c = '\'';

            ctx->AddLine("FX %d %d %d \"%s\"",
                         fx.slotIndex, fx.paramCount, (int)fx.enabled,
                         safeFxName.c_str());

            // Write normalized values in batches of 8
            for (int p = 0; p < (int)fx.normVals.size(); p += 8)
            {
                int pend = std::min(p + 8, (int)fx.normVals.size());
                WriteParamLine(ctx, fx.normVals, p, pend);
            }
            ctx->AddLine("FXEND");
        }

        // Offline chain chunk (multi-line data, written as-is between markers)
        if (!ts.fxChainChunk.empty())
        {
            ctx->AddLine("FXCHAINSTART");
            // Write the chunk line-by-line
            const char* p = ts.fxChainChunk.c_str();
            while (*p)
            {
                const char* nl = strchr(p, '\n');
                if (!nl) nl = p + strlen(p);
                int len = (int)(nl - p);
                if (len > 0)
                {
                    char linebuf[4096];
                    int copylen = std::min(len, (int)sizeof(linebuf) - 1);
                    memcpy(linebuf, p, copylen);
                    linebuf[copylen] = '\0';
                    ctx->AddLine("%s", linebuf);
                }
                p = (*nl == '\n') ? nl + 1 : nl;
            }
            ctx->AddLine("FXCHAINEND");
        }

        ctx->AddLine("TRACKEND");
    }
    ctx->AddLine(">");
}

// ---------------------------------------------------------------------------
// Deserialize (called from ProcessExtensionLine when "TSSNAPSHOT" is seen)
// The leading '<' has already been stripped by REAPER before calling us.
// headerLine  = "TSSNAPSHOT <slot> <mask> <time> \"<name>\""
// ctx->GetLine reads subsequent child lines; returns -1 at EOF or on ">"
// ---------------------------------------------------------------------------
TransitionSnapshot* TransitionSnapshot::Deserialize(const char* headerLine,
                                                     ProjectStateContext* ctx)
{
    int slot = 0, mask = 0, ts_time = 0;
    // headerLine arrives with the leading '<' (REAPER does not strip it)
    if (sscanf(headerLine, "<TSSNAPSHOT %d %d %d", &slot, &mask, &ts_time) < 3)
        return nullptr;

    // Extract quoted name (last quoted token on the header line)
    char name[512] = "";
    const char* q = strchr(headerLine, '"');
    if (q)
    {
        q++;
        int i = 0;
        while (*q && *q != '"' && i < (int)sizeof(name) - 1)
            name[i++] = *q++;
        name[i] = '\0';
    }

    auto* ss     = new TransitionSnapshot(slot, name);
    ss->m_mask   = mask;
    ss->m_time   = ts_time;

    // ---- Parse child lines ------------------------------------------------
    char line[4096];
    TrackState  curTrack;
    bool        inTrack = false;
    FXState     curFX;
    bool        inFX    = false;
    bool        inChunk = false;

    while (ctx->GetLine(line, sizeof(line)) == 0)
    {
        // Trim leading whitespace
        char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        // FX chain chunk: absorb all lines (including '>') until FXCHAINEND
        if (inChunk)
        {
            if (strcmp(trimmed, "FXCHAINEND") == 0)
                inChunk = false;
            else
            {
                curTrack.fxChainChunk += trimmed;
                curTrack.fxChainChunk += '\n';
            }
            continue;
        }

        if (strcmp(trimmed, ">") == 0)
            break; // end of TSSNAPSHOT block

        else if (strncmp(trimmed, "LAYER ", 6) == 0)
        {
            int li = -1;
            sscanf(trimmed + 6, "%d", &li);
            ss->m_layerIdx = li;
        }
        else if (strncmp(trimmed, "LAYERCOUNT ", 11) == 0)
        {
            // Just a count marker; actual data follows as LAYERDEF blocks.
            ss->m_layers.clear();
        }
        else if (strncmp(trimmed, "LAYERDEF ", 9) == 0)
        {
            CapturedLayer cl;
            const char* nq = strchr(trimmed + 9, '"');
            if (nq)
            {
                nq++;
                char nbuf[64] = {};
                int ni = 0;
                while (*nq && *nq != '"' && ni < 63) nbuf[ni++] = *nq++;
                nbuf[ni] = '\0';
                cl.name = nbuf;
                // maxChannels is after the closing quote
                const char* afterQ = (*nq == '"') ? nq + 1 : nq;
                int mc = 0;
                sscanf(afterQ, " %d", &mc);
                cl.maxChannels = mc;
            }
            // Read sub-lines until LAYERDEFEND
            char subLine[4096];
            while (ctx->GetLine(subLine, sizeof(subLine)) == 0)
            {
                char* st = subLine;
                while (*st == ' ' || *st == '\t') st++;
                if (strcmp(st, "LAYERDEFEND") == 0) break;
                if (strcmp(st, "LAYERSPACER") == 0)
                {
                    CapturedLayerTrack clt;
                    clt.isSpacer = true;
                    cl.tracks.push_back(clt);
                }
                else if (strncmp(st, "LAYERTRACK ", 11) == 0)
                {
                    CapturedLayerTrack clt;
                    clt.isSpacer = false;
                    LayersEngine::StrToGuid(st + 11, clt.guid);
                    cl.tracks.push_back(clt);
                }
            }
            ss->m_layers.push_back(cl);
        }
        else if (strncmp(trimmed, "DURATION ", 9) == 0)
        {
            // Per-snapshot transition settings (top-level line, before any TRACK)
            double dur = 2.0;  int tap = TAPER_SCURVE;  double ex = 2.0;
            sscanf(trimmed + 9, "%lf %d %lf", &dur, &tap, &ex);
            ss->m_duration = dur  > 0.0 ? dur : 2.0;
            ss->m_taper    = tap;
            ss->m_taperExp = ex > 0.0 ? ex  : 2.0;
        }
        else if (strncmp(trimmed, "NOTES ", 6) == 0)
        {
            const char* nq = strchr(trimmed + 6, '"');
            if (nq) {
                nq++;
                std::string notes;
                while (*nq && *nq != '"') notes += *nq++;
                ss->m_notes = notes;
            }
        }
        else if (strncmp(trimmed, "TRACK ", 6) == 0)
        {
            if (inFX)    { curTrack.fx.push_back(curFX); inFX = false; }
            if (inTrack) { ss->m_tracks.push_back(curTrack); }
            inTrack  = true;
            curTrack = TrackState{};
            char sguid[64] = "";
            sscanf(trimmed, "TRACK %63s", sguid);
            curTrack.guid = StringToGuid(sguid);
        }
        else if (strncmp(trimmed, "VOL ", 4) == 0)
        {
            sscanf(trimmed, "VOL %lf", &curTrack.vol);
        }
        else if (strncmp(trimmed, "PAN ", 4) == 0)
        {
            sscanf(trimmed, "PAN %lf %d %lf %lf %lf %lf",
                   &curTrack.pan, &curTrack.panMode,
                   &curTrack.width, &curTrack.dualPanL,
                   &curTrack.dualPanR, &curTrack.panLaw);
        }
        else if (strncmp(trimmed, "MISC ", 5) == 0)
        {
            int mute = 0, phase = 0;
            sscanf(trimmed, "MISC %d %d %d %d %d %d %lf",
                   &mute, &curTrack.solo, &phase,
                   &curTrack.vis, &curTrack.selected,
                   &curTrack.playOffsetFlag, &curTrack.playOffset);
            curTrack.mute  = (mute  != 0);
            curTrack.phase = (phase != 0);
        }
        else if (strncmp(trimmed, "LAYOUT ", 7) == 0)
        {
            int locked = 0;
            sscanf(trimmed, "LAYOUT %d %d %d %d",
                   &curTrack.color, &curTrack.heightOverride,
                   &locked, &curTrack.capturedIndex);
            curTrack.heightLocked = (locked != 0);
            // Extract quoted track name
            const char* nq = strchr(trimmed + 7, '"');
            if (nq) {
                nq++;
                int i = 0;
                char nbuf[512] = {};
                while (*nq && *nq != '"' && i < 510)
                    nbuf[i++] = *nq++;
                curTrack.trackName = nbuf;
            }
        }
        else if (strncmp(trimmed, "FX ", 3) == 0)
        {
            if (inFX) curTrack.fx.push_back(curFX);
            inFX  = true;
            curFX = FXState{};
            int en = 1;
            sscanf(trimmed, "FX %d %d %d",
                   &curFX.slotIndex, &curFX.paramCount, &en);
            curFX.enabled = (en != 0);
            curFX.normVals.reserve(curFX.paramCount);
            // Extract quoted name
            const char* nq = strchr(trimmed + 3, '"');
            if (nq)
            {
                nq++;
                int i = 0;
                while (*nq && *nq != '"' && i < 255)
                    curFX.name[i++] = *nq++;
                curFX.name[i] = '\0';
            }
        }
        else if (trimmed[0] == 'P' && trimmed[1] == ' ')
        {
            // Param values line: "P val0 val1 ..."
            const char* p = trimmed + 2;
            while (*p)
            {
                double val;
                int consumed = 0;
                if (sscanf(p, " %lf%n", &val, &consumed) != 1) break;
                curFX.normVals.push_back(val);
                p += consumed;
            }
        }
        else if (strcmp(trimmed, "FXEND") == 0)
        {
            if (inFX) { curTrack.fx.push_back(curFX); inFX = false; }
        }
        else if (strcmp(trimmed, "FXCHAINSTART") == 0)
        {
            inChunk = true;
            curTrack.fxChainChunk.clear();
        }
        else if (strcmp(trimmed, "TRACKEND") == 0)
        {
            if (inFX)    { curTrack.fx.push_back(curFX); inFX = false; }
            if (inTrack) { ss->m_tracks.push_back(curTrack); inTrack = false; }
        }
    }

    // Flush any unclosed track/FX
    if (inFX)    curTrack.fx.push_back(curFX);
    if (inTrack) ss->m_tracks.push_back(curTrack);

    return ss;
}
