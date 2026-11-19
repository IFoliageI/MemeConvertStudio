#pragma once
#include "plugins.hpp"

namespace studio {
Provider configuredProvider(const Json& config);
Crop configuredCrop(const Json& data);
std::vector<std::string> configuredPlugins(const Json& config);
fs::path configuredPluginDirectory(const Json& config);
Job configuredJob(const Json& config, std::shared_ptr<Plugins> plugins);
std::vector<fs::path> runConfiguredJob(const Json& config, Job job, Cancel& cancel,
    const Reporter& progress = {}, const std::function<void(const fs::path&)>& checkOutput = {});
}
