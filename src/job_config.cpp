#include "job_config.hpp"
#include "api_routing.hpp"
#include "batch.hpp"
#include <algorithm>
#include <cmath>

namespace studio {
std::vector<std::string> configuredPlugins(const Json& config) {
    return config.value("plugins", std::vector<std::string>{});
}
fs::path configuredPluginDirectory(const Json& config) {
    if (config.contains("plugin_directory")) return wide(config.at("plugin_directory").get<std::string>());
    return pluginDirectory();
}
namespace {
bool hasPlugin(const Json& config, const char* id) {
    auto plugins = configuredPlugins(config);
    return std::find(plugins.begin(), plugins.end(), id) != plugins.end();
}
}
Provider configuredProvider(const Json& config) {
    Provider provider;
    provider.baseUrl = wide(config.value("base_url", "https://api.openai.com/v1"));
    provider.model = config.value("model", "gpt-image-1");
    provider.quality = config.value("quality", "auto");
    provider.size = config.value("size", "auto");
    provider.complexity = config.value("complexity", "");
    provider.outputFormat = config.value("output_format", "");
    provider.compression = config.value("output_compression", -1);
    provider.stream = config.value("stream", false);
    provider.partialImages = config.value("partial_images", 2);
    provider.imageField = config.value("image_field", "image[]");
    provider.extra = config.value("extra", Json::object());
    provider.timeoutSeconds = config.value("timeout_seconds", 600);
    auto name = wide(config.value("api_key_env", "OPENAI_API_KEY"));
    wchar_t key[8192]{};
    DWORD count = GetEnvironmentVariableW(name.c_str(), key, DWORD(std::size(key)));
    if (count && count < std::size(key)) provider.key = key;
    SecureZeroMemory(key, sizeof(key));
    return provider;
}
Crop configuredCrop(const Json& data) {
    Crop crop{data.at("x"), data.at("y"), data.at("width"), data.at("height")};
    if (!std::isfinite(crop.x) || !std::isfinite(crop.y) || !std::isfinite(crop.width) ||
        !std::isfinite(crop.height) || crop.x < 0 || crop.y < 0 || crop.width <= 0 || crop.height <= 0 ||
        crop.x + crop.width > 1.0000001 || crop.y + crop.height > 1.0000001)
        throw Error("Crop coordinates must describe a non-empty region within the image (0 to 1)");
    return crop;
}
Job configuredJob(const Json& config, std::shared_ptr<Plugins> plugins) {
    Job job;
    auto mode = config.value("mode", "replace");
    if (mode != "replace" && mode != "refine" && mode != "generate") throw Error("Invalid job mode");
    job.refine = mode == "refine"; job.generate = mode == "generate";
    job.plugins = std::move(plugins);
    if (job.generate && !hasPlugin(config, "memestudio.prompt-draw"))
        throw Error("请先启用无图绘制插件");
    if (config.contains("upscale")) job.upscale = upscaleOptions(config["upscale"]);
    job.source = wide(config.value("source", ""));
    job.character = wide(config.value("character", ""));
    job.output = wide(config.at("output").get<std::string>());
    if (job.output.empty()) throw Error("Specify a non-empty output path");
    job.provider = configuredProvider(config);
    job.stride = config.value("stride", 1);
    job.maxFrames = config.value("max_frames", job.refine ? 0 : 24);
    job.generationCount = config.value("count", 1);
    job.consistency = config.value("consistency", true);
    job.preserveText = config.value("preserve_text", true);
    job.resume = config.value("resume", true);
    job.instruction = wide(config.value("instruction", ""));
    for (const auto& path : config.value("styles", std::vector<std::string>{})) job.styles.emplace_back(wide(path));
    if (config.contains("crop")) job.crop = configuredCrop(config["crop"]);
    if (config.contains("region")) job.region = configuredCrop(config["region"]);
    if (config.contains("api_routing")) {
        auto options = routingOptions(config["api_routing"], false);
        if (options.enabled) {
            if (!hasPlugin(config, "memestudio.api-load-balancing"))
                throw Error("请先加载 API 负载均衡与灾备插件");
            auto router = std::make_shared<ApiRouter>(options, job.provider);
            job.provider.baseUrl = L"memestudio://local-gateway/v1";
            job.provider.key = L"local-gateway";
            job.provider.gateway = std::make_shared<AggregatedGateway>(router);
            job.provider.router = std::move(router);
        }
    }
    return job;
}
std::vector<fs::path> runConfiguredJob(const Json& config, Job job, Cancel& cancel,
    const Reporter& progress, const std::function<void(const fs::path&)>& checkOutput) {
    for (auto key : {"preview_file", "progress_file", "routing_report"})
        if (checkOutput && config.contains(key)) checkOutput(wide(config.at(key).get<std::string>()));
    auto reporter = [&](const Progress& update) {
        if (progress) progress(update);
        if (update.preview && config.contains("preview_file"))
            savePng(wide(config["preview_file"].get<std::string>()), *update.preview);
        if (config.contains("progress_file"))
            writeJson(wide(config["progress_file"].get<std::string>()),
                {{"done", update.done}, {"total", update.total}, {"message", utf8(update.message)},
                 {"frame", update.frame}, {"partial_image_index", update.partial},
                 {"copy_index", update.copy}, {"count", update.copies}});
    };
    std::vector<fs::path> completed;
    auto run = [&] {
        if (!config.contains("sources")) {
            auto outputs = outputPaths(job);
            if (checkOutput) for (const auto& path : outputs) checkOutput(path);
            runJob(job, cancel, reporter);
            completed = std::move(outputs);
            return;
        }
        if (job.refine || job.generate) throw Error("源素材队列仅支持人物替换模式");
        if (!hasPlugin(config, "memestudio.batch-convert")) throw Error("请先启用批量转换插件");
        std::vector<fs::path> inputs;
        for (const auto& path : config.at("sources").get<std::vector<std::string>>()) inputs.emplace_back(wide(path));
        auto sources = collectSources(inputs);
        auto jobs = sourceJobs(sources, job, job.output);
        if (checkOutput) for (const auto& item : jobs)
            for (const auto& path : outputPaths(item)) checkOutput(path);
        runSourceJobs(sources, jobs, cancel, reporter, completed);
    };
    auto reportRouting = [&] {
        if (job.provider.router && config.contains("routing_report"))
            writeJson(wide(config["routing_report"].get<std::string>()), job.provider.router->statistics());
    };
    try { run(); }
    catch (...) { reportRouting(); throw; }
    reportRouting();
    return completed;
}
}
