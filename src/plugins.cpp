#include "plugins.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <cstring>

namespace studio {
static bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 80 && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}
fs::path pluginDirectory() {
    wchar_t path[32768]{};
    DWORD size = GetModuleFileNameW(nullptr, path, 32768);
    if (!size || size == 32768) throw Error("无法定位插件目录");
    return fs::path(path).parent_path() / L"plugins";
}
std::vector<PluginInfo> discoverPlugins(const fs::path& directory) {
    std::vector<PluginInfo> result;
    if (!fs::exists(directory)) return result;
    std::set<std::string> ids;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_directory() || !fs::is_regular_file(entry.path() / L"plugin.json")) continue;
        PluginInfo info; info.directory = entry.path(); info.name = entry.path().filename().wstring();
        try {
            auto json = readJson(entry.path() / L"plugin.json");
            if (json.at("api_version").get<int>() != 1) throw Error("不支持的插件 API 版本");
            info.id = json.at("id").get<std::string>();
            if (!identifier(info.id) || !ids.insert(info.id).second) throw Error("插件 ID 无效或重复");
            info.name = wide(json.at("name").get<std::string>());
            info.version = json.at("version").get<std::string>();
            info.description = wide(json.value("description", ""));
            auto library = fs::path(wide(json.at("library").get<std::string>()));
            if (library != library.filename() || library.extension() != L".dll")
                throw Error("library 必须是插件目录内的 DLL 文件名");
            info.library = entry.path() / library;
            if (!fs::is_regular_file(info.library)) throw Error("找不到插件 DLL");
            std::set<std::string> commands;
            for (const auto& command : json.value("commands", Json::array())) {
                auto id = command.at("id").get<std::string>();
                if (!identifier(id) || !commands.insert(id).second) throw Error("插件命令 ID 无效或重复");
                info.commands.push_back({id, wide(command.at("title").get<std::string>())});
            }
        } catch (const std::exception& error) { info.error = error.what(); }
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}
struct Plugins::Module {
    HMODULE handle = nullptr;
    const MemeStudioPluginV1* api = nullptr;
    PluginInfo info;
    std::string hash;
    bool initialized = false;
    ~Module() {
        if (initialized && api && api->shutdown) api->shutdown();
        if (handle) FreeLibrary(handle);
    }
};
Plugins::Plugins(const std::vector<PluginInfo>& available, const std::vector<std::string>& enabled,
    std::function<void(const std::wstring&)> log) : logger(std::move(log)) {
    host = {sizeof(host), MEMESTUDIO_PLUGIN_ABI, this,
        [](void* context, const char* message) {
            try {
                auto& self = *static_cast<Plugins*>(context);
                if (self.logger && message) self.logger(wide(std::string(message, strnlen_s(message, 4096))));
            } catch (...) {}
        }};
    std::set<std::string> loaded;
    try { for (const auto& id : enabled) {
        if (!loaded.insert(id).second) continue;
        auto found = std::find_if(available.begin(), available.end(), [&](const auto& info) { return info.id == id; });
        if (found == available.end() || !found->error.empty()) throw Error("插件不可用：" + id);
        auto module = std::make_unique<Module>(); module->info = *found;
        module->hash = digest(readFile(module->info.library));
        module->handle = LoadLibraryExW(fs::absolute(found->library).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module->handle) throw Error("无法加载插件 " + id + "，Windows 错误 " + std::to_string(GetLastError()));
        auto entry = reinterpret_cast<MemeStudioPluginEntryV1>(GetProcAddress(module->handle, MEMESTUDIO_PLUGIN_ENTRY));
        if (!entry) throw Error("插件缺少 MemeStudioGetPluginV1 入口：" + id);
        module->api = entry();
        if (!module->api || module->api->struct_size < sizeof(MemeStudioPluginV1) ||
            module->api->abi_version != MEMESTUDIO_PLUGIN_ABI || !module->api->id || module->api->id != id)
            throw Error("插件 ABI 或 ID 不匹配：" + id);
        module->initialized = true;
        if (module->api->initialize && module->api->initialize(&host) != 0)
            throw Error("插件初始化失败：" + id);
        modules.push_back(std::move(module));
    } } catch (...) { modules.clear(); throw; }
}
Plugins::~Plugins() { modules.clear(); }
Json Plugins::fingerprint() const {
    Json result = Json::array();
    for (const auto& module : modules)
        result.push_back({{"id", module->info.id}, {"version", module->info.version}, {"sha256", module->hash}});
    return result;
}
std::string Plugins::transform(const Json& context, std::string prompt) const {
    auto serialized = context.dump();
    for (const auto& module : modules) if (module->api->transform_prompt) {
        auto changed = module->api->transform_prompt(serialized.c_str(), prompt.c_str());
        if (changed) {
            size_t size = strnlen_s(changed, 256 * 1024 + 1);
            if (size > 256 * 1024) throw Error("插件提示词超过 256 KB：" + module->info.id);
            prompt.assign(changed, size);
        }
    }
    return prompt;
}
void Plugins::process(const Json& context, Image& image) const {
    auto serialized = context.dump();
    for (const auto& module : modules) if (module->api->process_frame) {
        MemeStudioImageV1 view{sizeof(view), image.width, image.height, image.width * 4, image.pixels.data()};
        if (module->api->process_frame(serialized.c_str(), &view) != 0)
            throw Error("插件图像处理失败：" + module->info.id);
    }
}
Json Plugins::command(const std::string& plugin, const std::string& command, const Json& context, HWND owner) const {
    for (const auto& module : modules) if (module->info.id == plugin) {
        if (std::none_of(module->info.commands.begin(), module->info.commands.end(),
            [&](const auto& item) { return item.id == command; })) throw Error("插件命令未声明");
        if (!module->api->run_command) throw Error("插件没有实现命令回调");
        auto input = context.dump();
        auto output = module->api->run_command(command.c_str(), input.c_str(), owner);
        if (!output) return Json::object();
        size_t size = strnlen_s(output, 256 * 1024 + 1);
        if (size > 256 * 1024) throw Error("插件命令结果超过 256 KB");
        auto result = Json::parse(output, output + size);
        if (!result.is_object()) throw Error("插件命令必须返回 JSON 对象");
        return result;
    }
    throw Error("请先启用插件并应用设置");
}
}
