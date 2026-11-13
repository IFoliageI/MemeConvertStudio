#pragma once
#include "studio.hpp"

namespace studio {
struct ProbeCase {
    std::string name;
    Provider provider;
    bool selected = false;
};
struct ProbeOptions {
    Provider provider;
    bool generation = false;
    std::vector<Upload> images;
    std::vector<ProbeCase> cases;
};
struct ProbeTarget { std::string name; Provider provider; };
std::vector<ProbeCase> parameterProbeCases(const Provider& provider, bool generation);
ProbeCase customProbeCase(const Provider& provider, const Json& value);
Json probeCaseParameters(const Provider& provider);
int estimateProbeTokens(const std::string& prompt);
Json estimateProbeUsage(const Provider& provider);
std::wstring probeTokenLabel(const Provider& provider);
std::vector<Upload> probeImages(const fs::path& source = {}, const fs::path& character = {}, const Crop& crop = {});
Json runParameterProbe(const ProbeOptions& options, Cancel& cancel,
    const std::function<void(const Json&)>& report = {});
int parameterProbeTest(const fs::path& directory, const std::wstring& base);
void showParameterProbe(HINSTANCE instance, HWND owner, const Provider& provider,
    const fs::path& source, const fs::path& character, const Crop& crop,
    bool capture = false, const std::function<void(HWND)>& snapshot = {},
    const std::vector<ProbeTarget>& targets = {});
}
