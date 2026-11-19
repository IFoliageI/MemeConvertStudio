#pragma once
#include "studio.hpp"

namespace studio {
struct CliOptions {
    std::string command, plugin, action;
    Json config = Json::object();
    fs::path configFile;
    bool quiet = false;
};
CliOptions cliOptions(int argc, wchar_t** argv);
}
