#include "plugins.hpp"
#include <wintrust.h>
#include <softpub.h>
#include <algorithm>
#include <cctype>
#include <set>
#include <cstring>

namespace studio {
namespace {
struct SignatureFile {
    fs::path path;
    HANDLE handle;
    explicit SignatureFile(const fs::path& file) : path(fs::canonical(file)),
        handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle == INVALID_HANDLE_VALUE) {
            auto error = GetLastError();
            throw Error("无法打开文件进行签名验证：" + utf8(path.wstring()) +
                "，Windows 错误 " + std::to_string(error));
        }
    }
    ~SignatureFile() { CloseHandle(handle); }
    SignatureFile(const SignatureFile&) = delete;
    SignatureFile& operator=(const SignatureFile&) = delete;
};

struct TrustCheck {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    ~TrustCheck() {
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    }
};

std::vector<Bytes> trustedSigners(const SignatureFile& file, const std::string& label) {
    WINTRUST_FILE_INFO info{};
    info.cbStruct = sizeof(info);
    info.pcwszFilePath = file.path.c_str();
    info.hFile = file.handle;
    std::vector<Bytes> signers;
    DWORD secondaryCount = 0;
    LONG firstStatus = TRUST_E_NOSIGNATURE;
    for (DWORD index = 0; index <= secondaryCount; ++index) {
        WINTRUST_SIGNATURE_SETTINGS signature{};
        signature.cbStruct = sizeof(signature);
        signature.dwIndex = index;
        signature.dwFlags = WSS_VERIFY_SPECIFIC | (index == 0 ? WSS_GET_SECONDARY_SIG_COUNT : 0);
        TrustCheck check;
        auto& data = check.data;
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_DISABLE_MD2_MD4;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &info;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        auto status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &check.action, &data);
        if (index == 0) {
            firstStatus = status;
            secondaryCount = signature.cSecondarySigs;
        }
        if (status != ERROR_SUCCESS) continue;
        auto provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        // Use the verified publisher's leaf certificate, not its display name or timestamp signer.
        if (signer && signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
            auto cert = signer->pasCertChain[0].pCert;
            signers.emplace_back(cert->pbCertEncoded, cert->pbCertEncoded + cert->cbCertEncoded);
        }
    }
    if (signers.empty()) {
        if (firstStatus == TRUST_E_NOSIGNATURE) throw Error(label + "没有有效的数字签名");
        throw Error(label + "签名验证失败（Windows 状态 " + std::to_string(firstStatus) + "）");
    }
    return signers;
}

fs::path executablePath() {
    wchar_t path[32768]{};
    DWORD size = GetModuleFileNameW(nullptr, path, 32768);
    if (!size || size == 32768) throw Error("无法定位主程序");
    return fs::path(path);
}
}

static bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 80 && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}
fs::path pluginDirectory() {
    return executablePath().parent_path() / L"plugins";
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
    std::function<void(const std::wstring&)> log,
    std::function<void(const std::string&, const Json&)> onAction,
    PluginSignaturePolicy signaturePolicy) : logger(std::move(log)), action(std::move(onAction)) {
    host = {sizeof(host), MEMESTUDIO_PLUGIN_ABI, this,
        [](void* context, const char* message) {
            try {
                auto& self = *static_cast<Plugins*>(context);
                if (self.logger && message) self.logger(wide(std::string(message, strnlen_s(message, 4096))));
            } catch (...) {}
        },
        [](void* context, const char* plugin, const char* payload) {
            try {
                auto& self = *static_cast<Plugins*>(context);
                if (!self.action || !plugin || !payload) return;
                size_t length = strnlen_s(payload, 256 * 1024 + 1);
                if (length > 256 * 1024) return;
                auto result = Json::parse(payload, payload + length);
                if (result.is_object()) self.action(std::string(plugin, strnlen_s(plugin, 128)), result);
            } catch (...) {}
        }};
    if (enabled.empty()) return;
    const bool verify = signaturePolicy != PluginSignaturePolicy::Disabled;
    const bool sameSigner = signaturePolicy == PluginSignaturePolicy::SameSigner;
    std::vector<Bytes> hostSigners;
    if (sameSigner) hostSigners = trustedSigners(SignatureFile(executablePath()), "主程序");
    struct Candidate {
        const PluginInfo& info;
        std::unique_ptr<SignatureFile> file;
    };
    std::vector<Candidate> candidates;
    std::set<std::string> seen;
    for (const auto& id : enabled) {
        if (!seen.insert(id).second) continue;
        auto found = std::find_if(available.begin(), available.end(), [&](const auto& info) { return info.id == id; });
        if (found == available.end() || !found->error.empty()) throw Error("插件不可用：" + id);
        Candidate candidate{*found, nullptr};
        if (verify) {
            candidate.file = std::make_unique<SignatureFile>(found->library);
            auto signers = trustedSigners(*candidate.file, "插件 " + id + "：");
            if (sameSigner && std::none_of(signers.begin(), signers.end(), [&](const auto& signer) {
                return std::find(hostSigners.begin(), hostSigners.end(), signer) != hostSigners.end();
            })) throw Error("插件 " + id + " 的签名者证书与主程序不一致");
        }
        candidates.push_back(std::move(candidate));
    }
    // Check every requested DLL before executing any plugin code, and retain the read handles through loading.
    try { for (const auto& candidate : candidates) {
        const auto& info = candidate.info;
        const auto& id = info.id;
        auto path = candidate.file ? candidate.file->path : fs::absolute(info.library);
        if (verify && GetModuleHandleW(path.c_str()))
            throw Error("插件 " + id + " 的 DLL 已在进程中加载，请重启主程序后再试");
        auto module = std::make_unique<Module>(); module->info = info;
        module->hash = digest(readFile(path));
        module->handle = LoadLibraryExW(path.c_str(), nullptr,
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
Json Plugins::fingerprint(bool frameOnly) const {
    Json result = Json::array();
    for (const auto& module : modules) {
        if (frameOnly && !module->api->transform_prompt && !module->api->process_frame) continue;
        result.push_back({{"id", module->info.id}, {"version", module->info.version}, {"sha256", module->hash}});
    }
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
