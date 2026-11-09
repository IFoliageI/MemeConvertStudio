#include "batch.hpp"
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <set>
#include <sstream>

namespace studio {
static std::wstring pathKey(const fs::path& path) {
    auto value = fs::weakly_canonical(path).wstring();
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
std::vector<SourceItem> collectSources(const std::vector<fs::path>& paths,
    const std::function<void(const std::wstring&)>& skipped) {
    std::vector<fs::path> candidates;
    std::set<std::wstring> seen;
    auto add = [&](const fs::path& path) {
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        if (extension != L".png" && extension != L".jpg" && extension != L".jpeg" &&
            extension != L".gif" && extension != L".bmp" && extension != L".webp" &&
            extension != L".tif" && extension != L".tiff") return;
        if (!seen.insert(pathKey(path)).second) return;
        if (candidates.size() >= 500) throw Error("单次最多导入 500 个源文件，请分批导入");
        candidates.push_back(fs::absolute(path).lexically_normal());
    };
    for (const auto& path : paths) {
        if (fs::is_directory(path)) {
            // Import only the selected folder, avoiding accidental recursive imports of outputs.
            std::vector<fs::path> files;
            for (const auto& entry : fs::directory_iterator(path))
                if (entry.is_regular_file()) files.push_back(entry.path());
            std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
                return _wcsicmp(a.c_str(), b.c_str()) < 0;
            });
            for (const auto& file : files) add(file);
        } else if (fs::is_regular_file(path)) add(path);
        else if (skipped) skipped(L"跳过不存在的素材：" + path.wstring());
    }
    std::vector<SourceItem> result;
    for (const auto& path : candidates) {
        try {
            auto animation = decode(path);
            result.push_back({path, static_cast<int>(animation.frames.size()), animation.animated});
        } catch (const std::exception& error) {
            if (skipped) skipped(L"跳过无法读取的图片 " + path.filename().wstring() + L"：" + wide(error.what()));
        }
    }
    if (result.empty()) throw Error("没有可读取的图片或 GIF；文件夹仅导入当前层级");
    return result;
}
int sourceCalls(const SourceItem& source, const Job& options) {
    if (options.stride < 1 || options.stride > 600 || options.maxFrames < 0 ||
        options.maxFrames > 600 || options.generationCount < 1 || options.generationCount > 10)
        throw Error("无效的批量转换参数");
    int stride = options.stride;
    if (options.maxFrames) stride = std::max(stride, (source.frames + options.maxFrames - 1) / options.maxFrames);
    return ((source.frames + stride - 1) / stride) * options.generationCount;
}
std::vector<Job> sourceJobs(const std::vector<SourceItem>& sources, const Job& options,
    const fs::path& directory) {
    if (sources.empty() || sources.size() > 500 || options.refine) throw Error("无效的源素材队列");
    if (directory.empty() || (fs::exists(directory) && !fs::is_directory(directory)))
        throw Error("批量输出必须选择文件夹");
    std::vector<Job> jobs;
    std::vector<fs::path> inputs{options.character};
    inputs.insert(inputs.end(), options.styles.begin(), options.styles.end());
    for (const auto& source : sources) inputs.push_back(source.path);
    std::set<std::wstring> destinations;
    for (size_t i = 0; i < sources.size(); ++i) {
        sourceCalls(sources[i], options);
        auto job = options;
        job.source = sources[i].path;
        std::wostringstream name;
        name << std::setw(3) << std::setfill(L'0') << i + 1 << L"_" <<
            job.source.stem().wstring() << L"-edited" << (sources[i].animated ? L".gif" : L".png");
        job.output = directory / name.str();
        for (const auto& output : outputPaths(job)) {
            if (!destinations.insert(pathKey(output)).second) throw Error("批量输出路径重复");
            for (const auto& input : inputs) {
                if (input.empty() || !fs::is_regular_file(input)) throw Error("输入图片不存在");
                if (pathKey(output) == pathKey(input) ||
                    (fs::exists(output) && fs::equivalent(output, input)))
                    throw Error("批量输出不能覆盖任何源素材或参考图");
            }
        }
        jobs.push_back(std::move(job));
    }
    return jobs;
}
void runSourceJobs(const std::vector<SourceItem>& sources, const std::vector<Job>& jobs,
    Cancel& cancel, const Reporter& report, std::vector<fs::path>& completed) {
    if (jobs.empty() || sources.size() != jobs.size()) throw Error("无效的批量任务");
    int total = 0, done = 0;
    for (size_t i = 0; i < jobs.size(); ++i) total += sourceCalls(sources[i], jobs[i]);
    for (size_t i = 0; i < jobs.size(); ++i) {
        cancel.verify();
        int calls = sourceCalls(sources[i], jobs[i]);
        runJob(jobs[i], cancel, [&](const Progress& progress) {
            auto event = progress;
            event.frameTotal = progress.total / std::max(1, progress.copies);
            event.done = done + std::min(calls, progress.done);
            event.total = total;
            event.message = L"素材 " + std::to_wstring(i + 1) + L" / " +
                std::to_wstring(jobs.size()) + L" [" + sources[i].path.filename().wstring() +
                L"]：" + progress.message;
            report(event);
        });
        auto outputs = outputPaths(jobs[i]);
        completed.insert(completed.end(), outputs.begin(), outputs.end());
        done += calls;
    }
}
}
