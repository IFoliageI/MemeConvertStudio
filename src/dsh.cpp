#include "dsh.hpp"
#include "api_probe.hpp"
#include "batch.hpp"
#include <winhttp.h>
#include <sddl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <optional>

namespace studio {
namespace {
using namespace std::chrono_literals;
constexpr size_t maxRequest = 64 * 1024, maxResponse = 1024 * 1024;
constexpr auto pipeName = LR"(\\.\pipe\MemeConvertStudio.DeepSeekHarness.v1)";
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct LocalMemory {
    HLOCAL value{};
    ~LocalMemory() { if (value) LocalFree(value); }
};
HANDLE createPipe() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) throw Error("Cannot read Windows user");
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    Bytes bytes(size);
    if (!size || !GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size))
        throw Error("Cannot read Windows user SID");
    PWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
        throw Error("Cannot encode Windows user SID");
    LocalMemory sidMemory{sid};
    // Node opens duplex pipes with generic read/write. The server owns the sole instance.
    auto acl = std::wstring(L"D:P(A;;GRGW;;;") + sid + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw Error("Cannot secure DeepSeek Harness pipe");
    LocalMemory securityMemory{descriptor};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    auto pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) throw Error("Cannot start DeepSeek Harness bridge; close any other enabled instance");
    return pipe;
}
template<class Begin>
bool transfer(HANDLE pipe, std::stop_token stop, DWORD milliseconds, DWORD& count, Begin begin) {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value || stop.stop_requested()) return false;
    OVERLAPPED op{}; op.hEvent = event.value;
    if (begin(op, count)) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        auto wait = WaitForSingleObject(event.value, 25);
        if (wait == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &op, &count, FALSE) != FALSE;
        if (wait == WAIT_FAILED) break;
    }
    CancelIoEx(pipe, &op);
    GetOverlappedResult(pipe, &op, &count, TRUE);
    return false;
}
Json failure(const char* message) { return {{"ok", false}, {"error", message}}; }
constexpr std::pair<const char*, bool DshPolicy::*> permissions[]{
    {"generate", &DshPolicy::generate}, {"batch", &DshPolicy::batch}, {"draw", &DshPolicy::draw},
    {"refine", &DshPolicy::refine}, {"probe", &DshPolicy::probe}, {"upscale", &DshPolicy::upscale},
    {"gateway", &DshPolicy::gateway}, {"local_api", &DshPolicy::localApi}, {"frame_hooks", &DshPolicy::frameHooks},
    {"plugin_settings", &DshPolicy::pluginSettings}, {"switch_model", &DshPolicy::switchModel}
};
int boundedInt(const Json& value, int minimum, int maximum, const char* error) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(maximum)))
        throw Error(error);
    auto number = value.get<int64_t>();
    if (number < minimum || number > maximum) throw Error(error);
    return int(number);
}
std::string usageDay() {
    SYSTEMTIME time{}; GetSystemTime(&time);
    char day[16]{};
    sprintf_s(day, "%04u-%02u-%02u", unsigned(time.wYear), unsigned(time.wMonth), unsigned(time.wDay));
    return day;
}
}

Json dshPolicyJson(const DshPolicy& policy) {
    Json data{{"models", policy.models}, {"per_task", policy.perTask}, {"per_day", policy.perDay}};
    for (const auto& [name, member] : permissions) data[name] = policy.*member;
    return data;
}
DshPolicy dshPolicyFromJson(const Json& data) {
    if (!data.is_object()) throw Error("Harness 权限必须是 JSON 对象");
    DshPolicy policy;
    for (const auto& [name, member] : permissions) policy.*member = data.value(name, false);
    policy.perTask = boundedInt(data.value("per_task", Json(8)), 0, 10000, "单任务限额须为 0 到 10000");
    policy.perDay = boundedInt(data.value("per_day", Json(32)), 0, 10000, "每日限额须为 0 到 10000");
    policy.models = data.value("models", std::vector<std::string>{});
    if (policy.models.size() > 64) throw Error("最多允许填写 64 个模型 ID");
    for (const auto& id : policy.models) {
        if (id.empty() || id.size() > 256 || std::any_of(id.begin(), id.end(),
            [](unsigned char c) { return c <= 32 || c == 127; }))
            throw Error("每个模型 ID 须为 1 到 256 字节，且不含空白或控制字符");
    }
    return policy;
}
int DshUsage::usedToday(const std::string& day) const {
    if (!fs::exists(path)) return 0;
    auto ledger = readJson(path);
    auto recorded = ledger.at("day").get<std::string>();
    if (recorded.size() != 10 || recorded > day) throw Error("Invalid Harness quota ledger date");
    auto sent = boundedInt(ledger.at("used"), 0, 10000, "Invalid Harness quota ledger usage");
    return recorded == day ? sent : 0;
}
int DshUsage::today() {
    std::lock_guard lock(mutex);
    return usedToday(usageDay());
}
void DshUsage::charge(int dayLimit) {
    std::lock_guard lock(mutex);
    try {
        auto day = usageDay();
        auto used = usedToday(day);
        if (used >= dayLimit) throw ApiRequestBlocked("Harness daily generation quota exceeded");
        writeJson(path, {{"day", day}, {"used", used + 1}});
    } catch (const ApiRequestBlocked&) { throw; }
    catch (const std::exception&) { throw ApiRequestBlocked("Harness quota ledger unavailable; no request sent"); }
}

struct DshBridge::Request {
    Json input;
    std::optional<DshWork> work;
};
DshBridge::DshBridge(HWND window) : owner(window), pipe(createPipe()) {
    try { server = std::jthread([this](std::stop_token stop) { serve(stop); }); }
    catch (...) { CloseHandle(pipe); throw; }
}
DshBridge::~DshBridge() {
    server.request_stop();
    cancel.stop();
    server.join();
    CloseHandle(pipe);
}
void DshBridge::dispatch(const std::function<DshWork(const Json&)>& prepare) {
    std::lock_guard lock(mutex);
    if (!pending || pending->work) return;
    try { pending->work = prepare(pending->input); }
    catch (const std::exception& e) {
        auto response = failure(e.what());
        pending->work = [response = std::move(response)](Cancel&) { return response; };
    }
    running = true;
}
void DshBridge::serve(std::stop_token stop) {
    auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stop.stop_requested()) {
        DWORD count{};
        if (!transfer(pipe, stop, 60000, count, [&](OVERLAPPED& op, DWORD&) {
            return ConnectNamedPipe(pipe, &op) || GetLastError() == ERROR_PIPE_CONNECTED;
        })) continue;
        std::string line;
        Json answer;
        try {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            char chunk[4096];
            while (line.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline &&
                transfer(pipe, stop, 1000, count, [&](OVERLAPPED& op, DWORD& n) {
                    return ReadFile(pipe, chunk, sizeof(chunk), &n, &op);
                }) && count) {
                line.append(chunk, count);
                if (line.size() > maxRequest) throw Error("DeepSeek Harness request is too large");
            }
            if (line.empty() || line.find('\n') != line.size() - 1) throw Error("Expected one JSON request line");
            auto request = std::make_shared<Request>();
            request->input = Json::parse(line, nullptr, false);
            if (!request->input.is_object()) throw Error("Invalid DeepSeek Harness request");
            cancel.reset();
            {
                std::lock_guard lock(mutex);
                pending = request;
            }
            if (!PostMessageW(owner, dshDispatch, 0, 0)) throw Error("MemeConvertStudio is closing");
            DshWork work;
            auto dispatchDeadline = std::chrono::steady_clock::now() + 10s;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < dispatchDeadline &&
                PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr)) {
                {
                    std::lock_guard lock(mutex);
                    if (request->work) { work = std::move(*request->work); pending.reset(); break; }
                }
                std::this_thread::sleep_for(20ms);
            }
            if (!work) throw Error("MemeConvertStudio is unavailable");
            std::jthread monitor([&](std::stop_token done) {
                while (!done.stop_requested()) {
                    if (stop.stop_requested() || !PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr)) {
                        cancel.stop();
                        return;
                    }
                    std::this_thread::sleep_for(25ms);
                }
            });
            if (stop.stop_requested()) cancel.stop();
            cancel.verify();
            check(com, "DeepSeek Harness worker COM");
            auto result = work(cancel);
            answer = result.contains("ok") && result["ok"] == false
                ? std::move(result) : Json{{"ok", true}, {"result", std::move(result)}};
        } catch (const std::exception& error) { answer = failure(error.what()); }
        {
            std::lock_guard lock(mutex);
            pending.reset();
            running = false;
        }
        auto response = answer.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
        if (response.size() > maxResponse) response = "{\"ok\":false,\"error\":\"Response too large\"}\n";
        if (transfer(pipe, stop, 5000, count, [&](OVERLAPPED& op, DWORD& n) {
            return WriteFile(pipe, response.data(), DWORD(response.size()), &n, &op);
        })) {
            // Wait briefly for the reader to consume the reply and close its connection.
            char ack{};
            transfer(pipe, stop, 2000, count, [&](OVERLAPPED& op, DWORD& n) { return ReadFile(pipe, &ack, 1, &n, &op); });
        }
        DisconnectNamedPipe(pipe);
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

namespace {
fs::path localPath(const std::string& value) {
    auto path = wide(value);
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || (path[2] != L'\\' && path[2] != L'/') ||
        path.find_first_of(L":*?\"<>|", 2) != std::wstring::npos ||
        std::any_of(path.begin(), path.end(), [](wchar_t c) { return c < 32; }))
        throw Error("Use an absolute local Windows path (for example D:/images/input.png)");
    auto file = fs::path(path).lexically_normal();
    auto root = GetDriveTypeW(file.root_path().c_str());
    if (root != DRIVE_FIXED && root != DRIVE_REMOVABLE && root != DRIVE_RAMDISK)
        throw Error("Only local drives are supported");
    return file;
}
fs::path fileArg(const Json& args, const char* key) {
    auto path = localPath(args.at(key).get<std::string>());
    if (!fs::is_regular_file(path)) throw Error("Expected an existing image file");
    return path;
}
void permit(bool allowed, const char* permission) {
    if (!allowed) throw Error(std::string("Harness permission denied: ") + permission);
}
void requirePlugin(const DshSettings& state, const char* id, bool allowed) {
    permit(allowed, id);
    if (std::find(state.enabled.begin(), state.enabled.end(), id) == state.enabled.end())
        throw Error(std::string("Enable the MemeConvertStudio plugin first: ") + id);
}
void checkLocalApi(const DshSettings& state, const std::wstring& baseUrl) {
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = DWORD(-1);
    auto url = modelsUrl(baseUrl);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) throw Error("Invalid provider URL");
    auto host = std::wstring(parts.lpszHostName, parts.dwHostNameLength);
    for (auto& c : host) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    if (host.ends_with(L'.')) host.pop_back();
    if (host == L"localhost" || host.starts_with(L"127.") || host == L"[::1]" || host == L"::1")
        permit(state.policy.localApi, "local_api");
}
void authorizeRoute(const DshSettings& state) {
    if (!state.routing.enabled) return checkLocalApi(state, state.job.provider.baseUrl);
    permit(state.policy.gateway, "gateway");
    for (const auto& channel : state.routing.channels)
        if (channel.enabled && channelSupports(channel, state.job.provider.model)) checkLocalApi(state, channel.baseUrl);
}
void taskPluginSettings(DshSettings& state, const Json& args) {
    if (!args.contains("plugin_settings")) return;
    permit(state.policy.pluginSettings, "plugin_settings");
    const auto& settings = args.at("plugin_settings");
    if (!settings.is_object()) throw Error("Expected a plugin_settings object");
    for (const auto& [plugin, options] : settings.items()) {
        if (!options.is_object()) throw Error("Expected a plugin settings object");
        if (plugin == "waifu2x") {
            requirePlugin(state, "memestudio.waifu2x", state.policy.upscale);
            for (const auto& [name, value] : options.items()) {
                if (name == "input") state.job.upscale.input = value.get<bool>();
                else if (name == "output") state.job.upscale.output = value.get<bool>();
                else if (name == "scale") {
                    auto scale = boundedInt(value, 1, 32, "Invalid Waifu2x scale");
                    if (scale & (scale - 1)) throw Error("Waifu2x scale must be 1, 2, 4, 8, 16 or 32");
                    state.job.upscale.scale = scale;
                }
                else if (name == "noise") state.job.upscale.noise = boundedInt(value, -1, 3, "Waifu2x noise must be -1 to 3");
                else throw Error("Unsupported Waifu2x setting");
            }
        } else if (plugin == "gateway") {
            requirePlugin(state, "memestudio.api-load-balancing", state.policy.gateway);
            if (!state.routing.enabled) throw Error("The supplier pool is not enabled");
            for (const auto& [name, value] : options.items()) {
                if (name != "failover") throw Error("Unsupported gateway setting");
                state.routing.failover = value.get<bool>();
            }
        } else throw Error("Unsupported plugin settings target");
    }
}
struct TaskQuota {
    DshPolicy policy;
    std::shared_ptr<DshUsage> usage;
    int used = 0;

    void verifyPlanned(int calls) const {
        if (calls > policy.perTask) throw ApiRequestBlocked("Harness per-task generation quota exceeded");
        if (!usage) throw ApiRequestBlocked("Harness quota ledger unavailable; no request sent");
        int today;
        try { today = usage->today(); }
        catch (const std::exception&) { throw ApiRequestBlocked("Harness quota ledger unavailable; no request sent"); }
        if (calls > policy.perDay - today) throw ApiRequestBlocked("Harness daily generation quota exceeded");
    }
    void charge() {
        if (used >= policy.perTask) throw ApiRequestBlocked("Harness per-task generation quota exceeded");
        usage->charge(policy.perDay);
        ++used;
    }
};
void route(Job& job, const RoutingOptions& options) {
    if (!options.enabled) return;
    job.provider.router = std::make_shared<ApiRouter>(options, job.provider);
    job.provider.gateway = std::make_shared<AggregatedGateway>(job.provider.router);
    job.provider.baseUrl = L"memestudio://local-gateway/v1";
    job.provider.key = L"local-gateway";
}
Provider fixedChannel(const DshSettings& state, const Json& args) {
    if (!state.routing.enabled) {
        checkLocalApi(state, state.job.provider.baseUrl);
        return state.job.provider;
    }
    permit(state.policy.gateway, "gateway");
    if (!args.contains("channel")) throw Error("Specify a channel from memestudio_status when the gateway is active");
    auto index = boundedInt(args.at("channel"), 0, 63, "Invalid channel number");
    if (size_t(index) >= state.routing.channels.size() || !state.routing.channels[index].enabled)
        throw Error("Channel is not enabled");
    auto channel = state.routing.channels[index];
    if (!state.job.provider.model.empty() && !channelSupports(channel, state.job.provider.model) &&
        args.value("operation", "") != "models")
        throw Error("The selected channel does not enable this model");
    checkLocalApi(state, channel.baseUrl);
    return channelProvider(state.job.provider, channel);
}
Crop regionArg(const Json& value) {
    Crop result{value.at("x"), value.at("y"), value.at("width"), value.at("height")};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.width) ||
        !std::isfinite(result.height) || result.x < 0 || result.y < 0 || result.width <= 0 ||
        result.height <= 0 || result.x + result.width > 1.0000001 || result.y + result.height > 1.0000001)
        throw Error("Invalid normalized crop");
    return result;
}
fs::path outputDirectory(const Json& args) {
    if (args.contains("output_parent")) {
        auto path = localPath(args.at("output_parent").get<std::string>());
        if (!fs::is_directory(path)) throw Error("Output parent directory does not exist");
        return path;
    }
    return settingsPath().parent_path() / L"dsh-output";
}
fs::path makeRunDirectory(const fs::path& parent) {
    fs::create_directories(parent);
    GUID id{};
    check(CoCreateGuid(&id), "Create task identifier");
    wchar_t name[40]{};
    StringFromGUID2(id, name, 40);
    auto path = parent / (L"MemeStudio-" + std::wstring(name + 1, 36));
    if (!fs::create_directory(path)) throw Error("Cannot reserve output directory");
    return path;
}
}

DshWork prepareDsh(const Json& request, const std::function<DshSettings(bool)>& settings, const Reporter& report) {
    if (request.value("version", 0) != 1) throw Error("Incompatible DeepSeek Harness bridge protocol");
    const auto op = request.at("operation").get<std::string>();
    auto args = request.value("arguments", Json::object());
    if (!args.is_object()) throw Error("Expected a tool argument object");
    if (op == "status") {
        auto state = settings(false);
        Json channels = Json::array();
        if (state.routing.enabled && state.policy.gateway) for (size_t i = 0; i < state.routing.channels.size(); ++i) {
            const auto& item = state.routing.channels[i];
            if (item.enabled) channels.push_back({{"channel", i}, {"name", item.name}, {"models", item.enabledModels}});
        }
        Json quota{{"period", "UTC day"}, {"used_today", nullptr}, {"remaining_today", nullptr}};
        try {
            if (state.usage) {
                auto used = state.usage->today();
                quota["used_today"] = used;
                quota["remaining_today"] = std::max(0, state.policy.perDay - used);
            }
        } catch (const std::exception&) {}
        return [value = Json{{"protocol", 1}, {"plugins", state.enabled}, {"model", state.job.provider.model},
            {"gateway", state.routing.enabled}, {"channels", channels},
            {"permissions", dshPolicyJson(state.policy)}, {"quota", quota}}](Cancel&) { return value; };
    }
    if (op == "inspect") return [path = fileArg(args, "path")](Cancel&) {
        auto animation = decode(path);
        const auto& image = animation.frames.front().image;
        return Json{{"width", image.width}, {"height", image.height}, {"animated", animation.animated},
            {"frames", animation.frames.size()}, {"duration_ms", std::accumulate(animation.frames.begin(),
                animation.frames.end(), uint64_t{0}, [](uint64_t total, const Frame& frame) { return total + frame.delayMs; })},
            {"loop", animation.loop}};
    };
    if (op == "models") {
        auto state = settings(false);
        if (state.routing.enabled && !args.contains("channel")) {
            permit(state.policy.gateway, "gateway");
            auto models = Json::array();
            for (const auto& model : poolModels(state.routing, state.job.provider.model))
                models.push_back({{"id", model.id}, {"label", utf8(model.label)}});
            return [models = std::move(models)](Cancel&) { return Json{{"models", models}}; };
        }
        if (args.contains("channel") && !state.routing.enabled) throw Error("The supplier pool is not enabled");
        args["operation"] = op;
        auto provider = fixedChannel(state, args);
        return [provider = std::move(provider)](Cancel& cancellation) {
            try { return Json{{"models", listModels(provider, cancellation)}}; }
            catch (...) { throw Error("Model query failed; check the provider in MemeConvertStudio"); }
        };
    }
    if (op != "probe" && op != "replace" && op != "batch" && op != "draw" && op != "refine")
        throw Error("Unknown MemeConvertStudio operation");
    auto state = settings(true);
    auto& job = state.job;
    if (args.contains("model")) {
        auto model = args.at("model").get<std::string>();
        if (model != job.provider.model) {
            permit(state.policy.switchModel, "switch_model");
            permit(std::find(state.policy.models.begin(), state.policy.models.end(), model) != state.policy.models.end(),
                "model is not in the allowlist");
            job.provider.model = std::move(model);
        }
    }
    auto quota = std::make_shared<TaskQuota>(TaskQuota{state.policy, state.usage});
    job.provider.beforeImageSend = [quota] { quota->charge(); };
    if (op == "probe") {
        requirePlugin(state, "memestudio.api-parameter-test", state.policy.probe);
        if (args.contains("plugin_settings")) throw Error("Probe does not accept plugin settings overrides");
        if (args.contains("channel") && !state.routing.enabled) throw Error("The supplier pool is not enabled");
        auto provider = fixedChannel(state, args);
        bool generation = args.value("text_only", false);
        auto cases = parameterProbeCases(provider, generation);
        auto selected = args.value("cases", Json::array());
        if (!selected.is_array() || selected.size() > 7) throw Error("Select at most seven additional probe cases");
        std::vector<int> requested;
        for (const auto& index : selected)
            requested.push_back(boundedInt(index, 1, int(cases.size()) - 1, "Unknown probe case index"));
        auto catalog = Json::array();
        for (size_t i = 0; i < cases.size(); ++i) {
            cases[i].selected = std::find(requested.begin(), requested.end(), int(i)) != requested.end();
            catalog.push_back({{"index", i}, {"name", cases[i].name}, {"estimated_tokens", estimateProbeUsage(cases[i].provider)}});
        }
        if (!args.value("run", false))
            return [catalog = std::move(catalog)](Cancel&) { return Json{{"cases", catalog}, {"sent_requests", 0}}; };
        permit(state.policy.generate, "generate");
        if (!args.value("confirmed", false)) throw Error("Confirm supplier charges before sending probe requests");
        quota->verifyPlanned(1 + int(std::count_if(cases.begin() + 1, cases.end(),
            [](const ProbeCase& item) { return item.selected; })));
        return [provider = std::move(provider), cases = std::move(cases), generation](Cancel& cancellation) {
            Json raw;
            try {
                raw = runParameterProbe({provider, generation, generation ? std::vector<Upload>{} : probeImages(), cases}, cancellation);
            } catch (const ApiRequestBlocked&) { throw; }
            catch (...) { throw Error("Parameter probe failed; check the supplier and parameters in MemeConvertStudio"); }
            for (auto& row : raw["results"]) {
                row.erase("detail"); row.erase("parameters");
            }
            raw.erase("provider_endpoint");
            return raw;
        };
    }
    permit(state.policy.generate, "generate");
    if (!args.value("confirmed", false)) throw Error("Confirm generation and any supplier charges first");
    if (op == "batch") requirePlugin(state, "memestudio.batch-convert", state.policy.batch);
    if (op == "draw") requirePlugin(state, "memestudio.prompt-draw", state.policy.draw);
    if (op == "refine") requirePlugin(state, "memestudio.local-redraw", state.policy.refine);
    if (!state.policy.frameHooks) job.plugins.reset();
    if (!state.policy.upscale) job.upscale = {};
    taskPluginSettings(state, args);
    authorizeRoute(state);
    job.generate = op == "draw"; job.refine = op == "refine";
    auto instruction = args.value("instruction", "");
    if (instruction.size() > 24000 || ((job.refine || job.generate) && instruction.empty()))
        throw Error("Provide an instruction of up to 24000 UTF-8 bytes");
    job.instruction = wide(instruction);
    if (auto styles = args.find("styles"); styles != args.end()) {
        if (!styles->is_array() || styles->size() > 8) throw Error("At most eight style references are supported");
        for (const auto& style : *styles) {
            auto path = localPath(style.get<std::string>());
            if (!fs::is_regular_file(path)) throw Error("Style reference not found");
            job.styles.push_back(path);
        }
    }
    std::vector<fs::path> inputs;
    if (op == "batch") {
        auto sources = args.at("sources").get<std::vector<std::string>>();
        if (sources.empty() || sources.size() > 500) throw Error("Provide between 1 and 500 source paths");
        for (const auto& path : sources) inputs.push_back(localPath(path));
    } else if (!job.generate) job.source = fileArg(args, "source");
    if (!job.generate && !job.refine) job.character = fileArg(args, "character");
    if (args.contains("crop") && !job.character.empty()) job.crop = regionArg(args["crop"]);
    if (job.refine) {
        if (args.contains("region")) job.region = regionArg(args["region"]);
        job.maxFrames = 0; job.stride = 1;
    }
    job.generationCount = boundedInt(args.value("count", Json(1)), 1, 10, "Count must be between 1 and 10");
    if (args.contains("max_frames") && !job.generate && !job.refine) {
        job.maxFrames = boundedInt(args.at("max_frames"), 0, 600, "Max frames must be between 0 and 600");
    }
    route(job, state.routing);
    if (job.provider.key.empty()) throw Error("Configure a provider API key in MemeConvertStudio");
    return [job = std::move(job), inputs = std::move(inputs), parent = outputDirectory(args), report, quota](Cancel& cancellation) mutable {
        cancellation.verify();
        std::vector<SourceItem> sources;
        bool animated = false;
        int planned = job.generationCount;
        if (!inputs.empty()) {
            sources = collectSources(inputs);
            planned = 0;
            for (const auto& source : sources) planned += sourceCalls(source, job);
        } else if (!job.generate) {
            const auto animation = decode(job.source);
            animated = animation.animated;
            planned = sourceCalls({job.source, int(animation.frames.size()), animated}, job);
        }
        quota->verifyPlanned(planned);
        cancellation.verify();
        auto directory = makeRunDirectory(parent);
        std::vector<fs::path> completed;
        try {
            const auto reporting = report ? report : Reporter([](const Progress&) {});
            if (!sources.empty()) {
                auto jobs = sourceJobs(sources, job, directory);
                runSourceJobs(sources, jobs, cancellation, reporting, completed);
            } else {
                job.output = directory / (animated ? L"result.gif" : L"result.png");
                runJob(job, cancellation, reporting);
                completed = outputPaths(job);
            }
        } catch (const ApiRequestBlocked& error) {
            if (report) report({0, 0, wide(error.what())});
            throw Error(std::string(error.what()) + ". Output directory: " + utf8(directory.wstring()));
        } catch (const std::exception&) {
            if (report) report({0, 0, L"DeepSeek Harness：生成失败"});
            if (cancellation.stopped) throw Error("MemeConvertStudio task was cancelled");
            throw Error("MemeConvertStudio task failed; see the application log. Output directory: " + utf8(directory.wstring()));
        }
        auto outputs = Json::array();
        for (const auto& path : completed) outputs.push_back(utf8(path.wstring()));
        return Json{{"outputs", outputs}, {"output_directory", utf8(directory.wstring())}};
    };
}
}
