#pragma once
#include "studio.hpp"

namespace studio {
struct SourceItem {
    fs::path path;
    int frames = 1;
    bool animated = false;
};
std::vector<SourceItem> collectSources(const std::vector<fs::path>& paths,
    const std::function<void(const std::wstring&)>& skipped = {});
int sourceCalls(const SourceItem& source, const Job& options);
std::vector<Job> sourceJobs(const std::vector<SourceItem>& sources, const Job& options,
    const fs::path& directory);
void runSourceJobs(const std::vector<SourceItem>& sources, const std::vector<Job>& jobs,
    Cancel& cancel, const Reporter& report, std::vector<fs::path>& completed);
}
