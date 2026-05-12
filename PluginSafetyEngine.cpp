#include "PluginSafetyEngine.h"

PluginSafetyEngine& PluginSafetyEngine::Get()
{
    static PluginSafetyEngine inst;
    return inst;
}

bool PluginSafetyEngine::HasRiskyPlugin(const std::vector<std::string>& /*fxNames*/) const
{
    return false;
}
