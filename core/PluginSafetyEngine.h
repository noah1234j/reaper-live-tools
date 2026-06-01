#pragma once
#include <string>
#include <vector>

class PluginSafetyEngine
{
public:
    static PluginSafetyEngine& Get();
    bool HasRiskyPlugin(const std::vector<std::string>& fxNames) const;
};
