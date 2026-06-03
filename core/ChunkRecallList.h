#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ChunkRecallList – keyword-based list of plugins that require full vst_chunk
// save/restore instead of per-parameter values.
//
// Use case: plugins with a fixed param-count but non-fixed param semantics
// (e.g. Waves VMR where swapping a module changes what params 0..15 mean for
// that slot). Storing normVals by index would recall wrong values.
//
// Keywords are stored in g_chunkRecallKeywords (project-scoped). On reset,
// it is pre-populated with defaults (see GetChunkRecallDefaults).
// All entries are user-editable and serialised as LTCHUNKPLUGIN lines.
// ---------------------------------------------------------------------------

// All keywords (defaults + user-added; cleared/reloaded per-project)
extern std::vector<std::string> g_chunkRecallKeywords;

// Whether to notify (via message box) after saving a scene with chunk plugins
extern bool g_chunkRecallNotify;

// Returns the default keyword list (used to pre-populate on project reset)
const std::vector<std::string>& GetChunkRecallDefaults();

// Returns true if the plugin display name contains any keyword
// (case-insensitive substring match).
bool IsChunkRecallPlugin(const char* name);
