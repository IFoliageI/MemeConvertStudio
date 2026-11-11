#include "studio.hpp"
#include "plugins.hpp"
#include "batch.hpp"
#include <shellapi.h>
#include <iostream>
#include <algorithm>

int wmain(int argc, wchar_t** argv) {
    using namespace studio;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 0;
    try {
        check(initialized, "Initialize COM");
        if (argc >= 3 && std::wstring(argv[1]) == L"--self-test") {
            exitCode = selfTest(argv[2]);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--network-test") {
            exitCode = networkTest(argv[2], argv[3]);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--inspect") {
            auto animation = decode(fs::path(argv[2]));
            Json info{{"width", animation.frames.front().image.width}, {"height", animation.frames.front().image.height},
                {"animated", animation.animated}, {"loop", animation.loop}, {"delays", Json::array()}};
            for (const auto& frame : animation.frames) info["delays"].push_back(frame.delayMs);
            writeJson(argv[3], info);
        } else if (argc >= 3 && std::wstring(argv[1]) == L"--ui-check") {
            exitCode = runGui(GetModuleHandleW(nullptr), argv[2], true);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--list-models") {
            auto config = readJson(argv[2]);
            Provider provider;
            provider.baseUrl = wide(config.at("base_url").get<std::string>());
            wchar_t key[8192]{};
            auto name = wide(config.value("api_key_env", "OPENAI_API_KEY"));
            DWORD count = GetEnvironmentVariableW(name.c_str(), key, 8192);
            if (count && count < 8192) provider.key = key;
            SecureZeroMemory(key, sizeof(key));
            Cancel cancel;
            writeJson(argv[3], listModels(provider, cancel));
        } else if (argc >= 3 && std::wstring(argv[1]) == L"--job") {
            Json config = readJson(argv[2]);
            Job job;
            job.source = wide(config.value("source", ""));
            job.refine = config.value("mode", "replace") == "refine";
            job.generate = config.value("mode", "replace") == "generate";
            if (job.generate) {
                const auto enabled = config.value("plugins", std::vector<std::string>{});
                if (std::find(enabled.begin(), enabled.end(), "memestudio.prompt-draw") == enabled.end())
                    throw Error("请先启用无图绘制插件");
            }
            if (config.contains("upscale")) job.upscale = upscaleOptions(config["upscale"]);
            job.character = wide(config.value("character", ""));
            job.output = wide(config.at("output").get<std::string>());
            job.provider.baseUrl = wide(config.value("base_url", "https://api.openai.com/v1"));
            job.provider.model = config.value("model", "gpt-image-1");
            job.provider.quality = config.value("quality", "auto");
            job.provider.size = config.value("size", "auto");
            job.provider.complexity = config.value("complexity", "");
            job.provider.outputFormat = config.value("output_format", "");
            job.provider.compression = config.value("output_compression", -1);
            job.provider.stream = config.value("stream", false);
            job.provider.partialImages = config.value("partial_images", 2);
            job.provider.imageField = config.value("image_field", "image[]");
            job.provider.extra = config.value("extra", Json::object());
            job.provider.timeoutSeconds = config.value("timeout_seconds", 600);
            auto environmentName = wide(config.value("api_key_env", "OPENAI_API_KEY"));
            wchar_t key[8192]{}; DWORD count = GetEnvironmentVariableW(environmentName.c_str(), key, 8192);
            if (count && count < 8192) job.provider.key = key;
            SecureZeroMemory(key, sizeof(key));
            job.stride = config.value("stride", 1); job.maxFrames = config.value("max_frames", job.refine ? 0 : 24);
            if (config.contains("region")) {
                const auto& region = config["region"];
                job.region = {region.at("x"), region.at("y"), region.at("width"), region.at("height")};
            }
            job.generationCount = config.value("count", 1);
            if (config.contains("plugins"))
                job.plugins = std::make_shared<Plugins>(
                    discoverPlugins(wide(config.value("plugin_directory", utf8(pluginDirectory().wstring())))),
                    config["plugins"].get<std::vector<std::string>>());
            job.consistency = config.value("consistency", true);
            job.preserveText = config.value("preserve_text", true); job.resume = config.value("resume", true);
            job.instruction = wide(config.value("instruction", ""));
            if (config.contains("styles"))
                for (const auto& path : config["styles"]) job.styles.emplace_back(wide(path.get<std::string>()));
            if (config.contains("crop")) {
                const auto& crop = config["crop"];
                job.crop = {crop.at("x"), crop.at("y"), crop.at("width"), crop.at("height")};
            }
            Cancel cancel;
            auto reporter = [&](const Progress& progress) {
                if (progress.preview && config.contains("preview_file"))
                    savePng(wide(config["preview_file"].get<std::string>()), *progress.preview);
                if (config.contains("progress_file"))
                    writeJson(wide(config["progress_file"].get<std::string>()),
                        {{"done", progress.done}, {"total", progress.total}, {"message", utf8(progress.message)},
                         {"frame", progress.frame}, {"partial_image_index", progress.partial},
                         {"copy_index", progress.copy}, {"count", progress.copies}});
            };
            if (config.contains("sources")) {
                if (job.refine || job.generate) throw Error("源素材队列仅支持人物替换模式");
                const auto enabled = config.value("plugins", std::vector<std::string>{});
                if (std::find(enabled.begin(), enabled.end(), "memestudio.batch-convert") == enabled.end())
                    throw Error("请先启用批量转换插件");
                std::vector<fs::path> inputs;
                for (const auto& path : config.at("sources")) inputs.emplace_back(wide(path.get<std::string>()));
                auto sources = collectSources(inputs);
                auto jobs = sourceJobs(sources, job, job.output);
                std::vector<fs::path> completed;
                runSourceJobs(sources, jobs, cancel, reporter, completed);
            } else runJob(job, cancel, reporter);
        } else {
            exitCode = runGui(GetModuleHandleW(nullptr), argc >= 3 && std::wstring(argv[1]) == L"--demo" ? argv[2] : fs::path{});
        }
    } catch (const std::exception& error) {
        if (argc > 1) {
            fs::path errorPath = fs::path(argv[argc - 1]).parent_path() / L"MemeStudio-error.txt";
            try {
                auto message = std::string(error.what()); writeFile(errorPath, Bytes(message.begin(), message.end()));
            } catch (...) {}
        } else MessageBoxW(nullptr, wide(error.what()).c_str(), L"Meme Studio", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return exitCode;
}
