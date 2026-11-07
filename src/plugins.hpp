#pragma once
#include "studio.hpp"
#include "../sdk/memestudio_plugin.h"

namespace studio {
struct PluginCommand { std::string id; std::wstring title; };
struct PluginInfo {
    std::string id, version, error;
    std::wstring name, description;
    fs::path directory, library;
    std::vector<PluginCommand> commands;
};
fs::path pluginDirectory();
std::vector<PluginInfo> discoverPlugins(const fs::path& directory);
class Plugins {
    struct Module;
    std::vector<std::unique_ptr<Module>> modules;
    std::function<void(const std::wstring&)> logger;
    MemeStudioHostV1 host{};
public:
    Plugins(const std::vector<PluginInfo>& available, const std::vector<std::string>& enabled,
        std::function<void(const std::wstring&)> log = {});
    ~Plugins();
    Plugins(const Plugins&) = delete;
    Plugins& operator=(const Plugins&) = delete;
    Json fingerprint() const;
    std::string transform(const Json& context, std::string prompt) const;
    void process(const Json& context, Image& image) const;
    Json command(const std::string& plugin, const std::string& command, const Json& context, HWND owner) const;
};
}
