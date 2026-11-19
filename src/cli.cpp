#include "cli_config.hpp"
#include "job_config.hpp"
#include "api_probe.hpp"
#include "api_routing.hpp"
#include <algorithm>
#include <cstdio>

namespace studio {
namespace {
constexpr auto help = R"(MemeStudio CLI
  MemeStudio-cli replace --source meme.gif --character character.png --output edited.gif
  MemeStudio-cli draw --prompt "An ink drawing" --style style.png --output result.png
  MemeStudio-cli refine --source result.png --prompt "Fix the outline" --output revised.png
  MemeStudio-cli batch --input images --input another.gif --character character.png --output results
  MemeStudio-cli run --config task.jsonc --model image-model
  MemeStudio-cli models [--config task.ini] [--channel N]
  MemeStudio-cli inspect --source image.gif
  MemeStudio-cli plugins [--plugin-directory PATH] [--load PLUGIN_ID]
  MemeStudio-cli plugin PLUGIN_ID COMMAND_ID [--config task.json] [--context JSON]
  MemeStudio-cli probe [--text-only] [--probe-run --cases "[1,2]"]

Provider: --base-url URL --api-key-env NAME --model ID --quality VALUE --size VALUE
          --complexity VALUE --format png|jpeg|webp --compression N --timeout-seconds N
Output:   --count N --max-frames N --stride N --stream --partial-images N --no-resume
          --progress-file PATH --preview-file PATH --routing-report PATH
Plugins:  --load ID (repeatable), --api-routing JSON, --upscale JSON, --plugin-directory PATH
Other:    --style PATH (repeatable), --crop JSON, --region JSON, --extra JSON, --quiet

JSON, JSONC and INI task files are read-only. Flags override file values. API keys come
from the named environment variable (default OPENAI_API_KEY); no GUI settings are saved.
Results go to stdout as JSON; progress and errors go to stderr. Ctrl+C cancels local work.
)";
Cancel cancellation;
BOOL WINAPI onControl(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    cancellation.stop();
    return TRUE;
}
void print(DWORD stream, FILE* file, const std::string& message) {
    auto handle = GetStdHandle(stream);
    DWORD mode{}, written{};
    if (handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        auto unicode = wide(message);
        WriteConsoleW(handle, unicode.data(), DWORD(unicode.size()), &written, nullptr);
    } else {
        std::fwrite(message.data(), 1, message.size(), file);
        std::fflush(file);
    }
}
void output(const std::string& message) { print(STD_OUTPUT_HANDLE, stdout, message); }
void diagnostic(const std::string& message) { print(STD_ERROR_HANDLE, stderr, message); }
void loadFor(Json& config, const std::string& id) {
    auto& list = config["plugins"];
    if (list.is_null()) list = Json::array();
    if (std::find(list.begin(), list.end(), id) == list.end()) list.push_back(id);
}
std::string pathText(const fs::path& path) { return utf8(path.wstring()); }
bool samePath(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(fs::weakly_canonical(a).c_str(), fs::weakly_canonical(b).c_str()) == 0 ||
        (fs::exists(a) && fs::exists(b) && fs::equivalent(a, b));
}
RoutingOptions routing(const Json& config) {
    return config.contains("api_routing") ? routingOptions(config["api_routing"], false) : RoutingOptions{};
}
Provider selectedChannel(const Json& config, const RoutingOptions& options, Provider provider, bool checkModel) {
    if (!options.enabled) {
        if (config.contains("channel")) throw Error("--channel requires an enabled supplier pool");
        return provider;
    }
    if (!config.contains("channel")) throw Error("Select a supplier pool channel with --channel N");
    auto channel = config.at("channel").get<int>();
    if (channel < 0 || size_t(channel) >= options.channels.size() || !options.channels[channel].enabled)
        throw Error("Supplier pool channel is unavailable");
    if (checkModel && !channelSupports(options.channels[channel], provider.model))
        throw Error("The selected supplier channel does not enable this model");
    return channelProvider(provider, options.channels[channel]);
}
Json probe(const Json& config, Cancel& cancel) {
    auto provider = selectedChannel(config, routing(config), configuredProvider(config), true);
    const bool generation = config.value("text_only", false);
    auto cases = parameterProbeCases(provider, generation);
    const auto selected = config.value("cases", std::vector<int>{});
    if (selected.size() > 7) throw Error("Choose at most seven additional probe cases");
    for (auto index : selected)
        if (index < 1 || size_t(index) >= cases.size()) throw Error("Unknown probe case index");
    auto catalog = Json::array();
    for (size_t i = 0; i < cases.size(); ++i) {
        auto& item = cases[i];
        item.selected = i == 0 || std::find(selected.begin(), selected.end(), int(i)) != selected.end();
        catalog.push_back({{"index", i}, {"name", item.name}, {"selected", item.selected},
            {"estimated_tokens", estimateProbeUsage(item.provider)}});
    }
    if (!config.value("probe_run", false)) return {{"cases", catalog}, {"sent_requests", 0}};
    auto images = generation ? std::vector<Upload>{} : probeImages(wide(config.value("source", "")),
        wide(config.value("character", "")), config.contains("crop") ? configuredCrop(config.at("crop")) : Crop{});
    return runParameterProbe({provider, generation, std::move(images), std::move(cases)}, cancel);
}
Json pluginCatalogue(const std::vector<PluginInfo>& available, const std::vector<std::string>& enabled) {
    auto entries = Json::array();
    for (const auto& plugin : available) {
        auto commands = Json::array();
        for (const auto& command : plugin.commands) commands.push_back({{"id", command.id}, {"title", utf8(command.title)}});
        entries.push_back({{"id", plugin.id}, {"name", utf8(plugin.name)}, {"version", plugin.version},
            {"loaded", std::find(enabled.begin(), enabled.end(), plugin.id) != enabled.end()},
            {"error", plugin.error}, {"commands", commands}});
    }
    return {{"plugins", entries}};
}
std::string pluginWorkflow(const CliOptions& options, const Json& result) {
    auto action = result.value("action", "");
    const auto matches = [&](const char* id, const char* name) { return options.plugin == id && action == name; };
    if (matches("memestudio.prompt-draw", "prompt_draw")) return "draw";
    if (matches("memestudio.local-redraw", "refine")) return "refine";
    if (matches("memestudio.batch-convert", "batch_import") || matches("memestudio.batch-convert", "batch_folder"))
        return "batch";
    if (matches("memestudio.api-parameter-test", "api_parameter_test")) return "probe";
    if (matches("memestudio.waifu2x", "configure_upscale")) return "upscale";
    if (matches("memestudio.api-load-balancing", "api_routing")) return "routing";
    return {};
}
Json execute(CliOptions options, Cancel& cancel) {
    auto& config = options.config;
    auto& command = options.command;
    const bool pluginCall = command == "plugin";
    if (pluginCall && (options.plugin == "memestudio.deepseek-harness" || options.plugin == "memestudio.qwen-image"))
        throw Error("This plugin command requires the GUI. Use a running Qwen endpoint via --base-url.");
    if (pluginCall) loadFor(config, options.plugin);
    if (command == "generate" || command == "draw") { command = "draw"; loadFor(config, "memestudio.prompt-draw"); }
    if (command == "batch") loadFor(config, "memestudio.batch-convert");
    if (command == "refine") loadFor(config, "memestudio.local-redraw");
    if (command == "probe") loadFor(config, "memestudio.api-parameter-test");
    if (config.contains("upscale") && (config["upscale"].value("input", false) || config["upscale"].value("output", false)))
        loadFor(config, "memestudio.waifu2x");
    if (config.contains("api_routing") && config["api_routing"].value("enabled", false))
        loadFor(config, "memestudio.api-load-balancing");
    if (!pluginCall && command != "replace" && command != "draw" && command != "refine" && command != "batch" &&
        command != "probe" && command != "inspect" && command != "plugins" && command != "models")
        throw Error("Unknown CLI command; see --help");
    auto enabled = configuredPlugins(config);
    std::vector<PluginInfo> available;
    if (command == "plugins" || !enabled.empty()) available = discoverPlugins(configuredPluginDirectory(config));
    auto log = [&](const std::wstring& message) {
        cancel.verify();
        if (!options.quiet) diagnostic(utf8(message) + '\n');
    };
    std::shared_ptr<Plugins> plugins;
    if (!enabled.empty()) plugins = std::make_shared<Plugins>(available, enabled, log);
    if (pluginCall) {
        auto context = config.value("context", Json::object());
        for (auto key : {"source", "character", "output", "instruction"})
            if (config.contains(key) && !context.contains(key)) context[key] = config.at(key);
        auto result = plugins->command(options.plugin, options.action, context, nullptr);
        command = pluginWorkflow(options, result);
        if (command.empty()) return {{"plugin", options.plugin}, {"command", options.action}, {"result", result}};
        if (command == "upscale" || command == "routing") {
            auto key = command == "upscale" ? "upscale" : "api_routing";
            if (!config.contains(key) || (command == "upscale" ?
                !config[key].value("input", false) && !config[key].value("output", false) :
                !config[key].value("enabled", false)))
                throw Error(std::string("Supply an enabled task configuration for ") + key);
            command = config.contains("sources") ? "batch" : config.value("mode", "replace");
            if (command == "generate") command = "draw";
        }
    }
    if (command == "plugins") return pluginCatalogue(available, enabled);
    if (command == "models") {
        auto pool = routing(config);
        auto provider = configuredProvider(config);
        if (pool.enabled && !config.contains("channel")) {
            auto models = Json::array(), channels = Json::array();
            for (const auto& model : poolModels(pool, provider.model))
                models.push_back({{"id", model.id}, {"label", utf8(model.label)}});
            for (size_t i = 0; i < pool.channels.size(); ++i) if (pool.channels[i].enabled)
                channels.push_back({{"index", i}, {"name", pool.channels[i].name}});
            return {{"models", models}, {"channels", channels}};
        }
        return {{"models", listModels(selectedChannel(config, pool, provider, false), cancel)}};
    }
    if (command == "inspect") {
        auto animation = decode(fs::path(wide(config.at("source").get<std::string>())));
        Json info{{"width", animation.frames.front().image.width}, {"height", animation.frames.front().image.height},
            {"animated", animation.animated}, {"frames", animation.frames.size()}, {"loop", animation.loop}, {"delays", Json::array()}};
        for (const auto& frame : animation.frames) info["delays"].push_back(frame.delayMs);
        return info;
    }
    if (command == "probe") return probe(config, cancel);
    if (command != "draw" && command != "refine" && command != "replace" && command != "batch")
        throw Error("Unsupported image workflow");
    if (config.contains("sources") != (command == "batch"))
        throw Error("Use batch with --input or a sources list; other operations take --source");
    if (config.contains("channel")) throw Error("--channel is only for models and probe");
    config["mode"] = command == "draw" ? "generate" : command == "refine" ? "refine" : "replace";
    auto protectedPaths = std::vector<fs::path>{settingsPath(), options.configFile};
    auto checkOutput = [&](const fs::path& target) {
        for (const auto& path : protectedPaths)
            if (samePath(target, path) || samePath(fs::path(target).concat(L".part"), path))
                throw Error("Output paths cannot overwrite a settings or input task file");
    };
    auto job = configuredJob(config, std::move(plugins));
    auto completed = runConfiguredJob(config, std::move(job), cancel, [&](const Progress& progress) {
        if (!progress.message.empty()) log(progress.message);
    }, checkOutput);
    auto results = Json::array();
    for (const auto& path : completed) results.push_back(pathText(fs::absolute(path).lexically_normal()));
    return {{"outputs", results}};
}
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace studio;
    auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = 0;
    SetConsoleCtrlHandler(onControl, TRUE);
    try {
        check(init, "Initialize COM");
        auto options = cliOptions(argc, argv);
        if (options.command == "help") output(help);
        else {
            const auto result = execute(std::move(options), cancellation);
            cancellation.verify();
            output(result.dump(2, ' ', false, Json::error_handler_t::replace) + '\n');
        }
    } catch (const std::exception& error) {
        diagnostic(std::string("MemeStudio: ") + error.what() + '\n');
        code = cancellation.stopped ? 130 : 1;
    }
    SetConsoleCtrlHandler(onControl, FALSE);
    if (SUCCEEDED(init)) CoUninitialize();
    return code;
}
