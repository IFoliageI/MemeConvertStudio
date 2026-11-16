#include "../../sdk/memestudio_plugin.h"
#pragma warning(push)
#pragma warning(disable: 4459)
#include "../../vendor/nlohmann/json.hpp"
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
HMODULE module{};
const MemeStudioHostV1* host{};
std::string response;
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n);
    return out;
}
std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') ++slashes;
        else { result.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\'); slashes = 0; result += c; }
    }
    result.append(slashes * 2, L'\\'); return result + L'"';
}
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE v) : value(v) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE v = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = v; }
};
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
fs::path directory() {
    wchar_t path[32768]{};
    require(GetModuleFileNameW(module, path, 32768) != 0, "Cannot locate plugin directory");
    return fs::path(path).parent_path();
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text; require(bool(output), "Cannot write plugin settings");
}
std::string encrypt(const std::string& text) {
    DATA_BLOB in{DWORD(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))}, out{};
    require(CryptProtectData(&in, L"Qwen local API", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) != FALSE,
        "Cannot encrypt local API key");
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (DWORD i = 0; i < out.cbData; ++i) { result += hex[out.pbData[i] >> 4]; result += hex[out.pbData[i] & 15]; }
    SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); return result;
}
std::string decrypt(const std::string& text) {
    std::vector<BYTE> bytes;
    require(text.size() % 2 == 0, "Invalid protected API key");
    for (size_t i = 0; i < text.size(); i += 2) bytes.push_back(BYTE(std::stoul(text.substr(i, 2), nullptr, 16)));
    DATA_BLOB in{DWORD(bytes.size()), bytes.data()}, out{};
    require(CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) != FALSE,
        "Cannot decrypt API key for this Windows user");
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); return result;
}
std::string randomKey() {
    BYTE bytes[24]{};
    require(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "Cannot generate API key");
    static const char hex[] = "0123456789abcdef";
    std::string result = "qwen-";
    for (auto c : bytes) { result += hex[c >> 4]; result += hex[c & 15]; }
    return result;
}
struct Child {
    Handle job, process;
    fs::path log, ready;
    Json config;
    bool serving = false;
    ~Child() { stop(); }
    bool running() const { return process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT; }
    void stop() {
        job.reset(); // KILL_ON_JOB_CLOSE releases the Python process tree and GPU allocations.
        if (process.value) WaitForSingleObject(process.value, 3000);
        process.reset();
        if (!ready.empty()) { std::error_code error; fs::remove(ready, error); }
        config = Json(); serving = false;
    }
    void launch(const fs::path& python, const std::wstring& script, const std::wstring& flag,
        Json settings, bool server) {
        require(!running(), "A service or environment operation is already running");
        stop();
        require(fs::is_regular_file(python), "Python executable does not exist");
        log = directory() / L"qwen-runtime.log";
        ready = directory() / (L"qwen-ready-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        settings["parent_pid"] = GetCurrentProcessId();
        settings["ready_file"] = utf8(ready.wstring());
        auto data = settings.dump();
        require(data.size() < 4096, "Plugin configuration is too large");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        Handle reader, writer;
        HANDLE r{}, w{};
        require(CreatePipe(&r, &w, &attributes, 0) != FALSE, "Cannot create worker input");
        reader.reset(r); writer.reset(w);
        SetHandleInformation(writer.value, HANDLE_FLAG_INHERIT, 0);
        Handle logs(CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(logs.value != INVALID_HANDLE_VALUE, "Cannot open runtime log");
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
            "Cannot configure worker crash cleanup");
        auto command = quote(python.wstring()) + L" -I -u " + quote((directory() / script).wstring()) + L" " + flag;
        STARTUPINFOW start{sizeof(start)}; start.dwFlags = STARTF_USESTDHANDLES;
        start.hStdInput = reader.value; start.hStdOutput = start.hStdError = logs.value;
        PROCESS_INFORMATION info{};
        require(CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory().c_str(), &start, &info) != FALSE,
            "Cannot start Python process");
        process.reset(info.hProcess); Handle thread(info.hThread);
        if (!AssignProcessToJobObject(job.value, process.value)) {
            TerminateProcess(process.value, 1); stop(); throw std::runtime_error("Cannot attach Python to cleanup job");
        }
        if (ResumeThread(thread.value) == DWORD(-1)) { stop(); throw std::runtime_error("Cannot resume Python"); }
        DWORD count{};
        if (!WriteFile(writer.value, data.data(), DWORD(data.size()), &count, nullptr) || count != data.size()) {
            stop(); throw std::runtime_error("Cannot send Python configuration");
        }
        config = std::move(settings); serving = server;
    }
};
struct State {
    Json config{{"model_directory", ""}, {"python", ""}, {"env_name", ".env"}, {"environment", ""},
        {"model_id", "qwen-image-local"}, {"port", 8189}, {"lan", false}, {"retain", true},
        {"offload", false}, {"backend", "auto"}, {"device", "auto"}, {"steps", 40}, {"cfg", 4.0},
        {"engine", "auto"}, {"sd_executable", ""}, {"diffusion_model", ""}, {"vae", ""},
        {"llm", ""}, {"llm_vision", ""}, {"turbo", true}, {"max_vram", 0.0}};
    Child child;
    HFONT font{};
    ~State() { if (font) DeleteObject(font); }
};
std::unique_ptr<State> state;
fs::path pythonPath(fs::path value) { return fs::is_directory(value) ? value / L"python.exe" : value; }
fs::path envPython(const Json& value) {
    auto existing = value.at("environment").get<std::string>();
    auto root = existing.empty() ? directory() / wide(value.at("env_name").get<std::string>()) : fs::path(wide(existing));
    if (fs::is_regular_file(root / L"Scripts/python.exe")) return root / L"Scripts/python.exe";
    return root / L"python.exe";
}
enum { ModelDir = 5100, Python, EnvName, Environment, ModelId, Port, Key, Lan, Retain, Offload,
    Backend, Device, Steps, Cfg, Install, Probe, Start, Stop, Unload, Apply, Logs, Status,
    BrowseModel, BrowsePython, BrowseEnvironment, InspectModel, GGUFOptions };
std::wstring text(HWND window, int id) {
    auto item = GetDlgItem(window, id); std::wstring result(GetWindowTextLengthW(item) + 1, 0);
    GetWindowTextW(item, result.data(), int(result.size())); result.pop_back(); return result;
}
void fill(HWND window) {
    for (const auto& [id, name] : {std::pair{ModelDir,"model_directory"}, {Python,"python"}, {EnvName,"env_name"},
        {Environment,"environment"}, {ModelId,"model_id"}, {Key,"key"}})
        SetDlgItemTextW(window, id, wide(state->config.value(name, "")).c_str());
    SetDlgItemInt(window, Port, state->config["port"].get<int>(), FALSE);
    SetDlgItemInt(window, Steps, state->config["steps"].get<int>(), FALSE);
    SetDlgItemTextW(window, Cfg, wide(state->config["cfg"].dump()).c_str());
    for (const auto& [id, name] : {std::pair{Lan,"lan"}, {Retain,"retain"}, {Offload,"offload"}})
        CheckDlgButton(window, id, state->config.value(name, false) ? BST_CHECKED : BST_UNCHECKED);
    for (const auto& [id, name] : {std::pair{Backend,"backend"}, {Device,"device"}}) {
        auto value = wide(state->config.value(name, "auto"));
        auto index = SendDlgItemMessageW(window, id, CB_FINDSTRINGEXACT, WPARAM(-1), LPARAM(value.c_str()));
        SendDlgItemMessageW(window, id, CB_SETCURSEL, index == CB_ERR ? 0 : index, 0);
    }
}
Json controls(HWND window) {
    auto value = state->config;
    for (const auto& [id, name] : {std::pair{ModelDir,"model_directory"}, {Python,"python"}, {EnvName,"env_name"},
        {Environment,"environment"}, {ModelId,"model_id"}, {Key,"key"}, {Backend,"backend"}, {Device,"device"}})
        value[name] = utf8(text(window, id));
    BOOL ok{};
    int port = GetDlgItemInt(window, Port, &ok, FALSE);
    require(ok && port >= 1024 && port <= 65535, "Port must be 1024..65535"); value["port"] = port;
    int steps = GetDlgItemInt(window, Steps, &ok, FALSE);
    require(ok && steps >= 1 && steps <= 100, "Steps must be 1..100"); value["steps"] = steps;
    size_t end{}; auto cfgText = utf8(text(window, Cfg)); double cfg = std::stod(cfgText, &end);
    require(end == cfgText.size() && cfg >= 1 && cfg <= 30, "CFG must be 1..30"); value["cfg"] = cfg;
    auto env = value["env_name"].get<std::string>();
    require(!env.empty() && env != "." && env != ".." && env.size() <= 80 &&
        env.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") == std::string::npos,
        "Environment name must be a simple directory name (default .env)");
    for (const auto& [id, name] : {std::pair{Lan,"lan"}, {Retain,"retain"}, {Offload,"offload"}})
        value[name] = IsDlgButtonChecked(window, id) == BST_CHECKED;
    auto key = value["key"].get<std::string>(), model = value["model_id"].get<std::string>();
    require(key.size() >= 8 && key.size() <= 256 &&
        std::all_of(key.begin(), key.end(), [](unsigned char c) { return c >= 33 && c <= 126; }),
        "Key must be 8..256 printable ASCII characters");
    require(!model.empty() && model.size() <= 120 && model.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") == std::string::npos, "Invalid model alias");
    return value;
}
void save(const Json& config) {
    auto stored = config;
    stored["encrypted_key"] = encrypt(stored.at("key").get<std::string>());
    stored.erase("key");
    auto path = directory() / L"qwen-settings.json";
    auto temporary = directory() / L"qwen-settings.tmp";
    write(temporary, stored.dump(2));
    require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
        "Cannot save Qwen settings");
    state->config = config;
}
fs::path folder(HWND window, bool directories = true) {
    IFileOpenDialog* dialog{};
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options{}; dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (directories ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    fs::path result;
    if (SUCCEEDED(dialog->Show(window))) {
        IShellItem* item{};
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path{};
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dialog->Release(); return result;
}
enum { EngineType = 5200, NativeExe, DiffusionFile, VaeFile, LlmFile, VisionFile, Turbo, MaxVram,
       BrowseNative = 5300, BrowseDiffusion, BrowseVae, BrowseLlm, BrowseVision };
INT_PTR CALLBACK nativeOptions(HWND window, UINT message, WPARAM wparam, LPARAM) {
    try {
        if (message == WM_INITDIALOG) {
            SetWindowTextW(window, L"推理引擎 / GGUF 组件");
            SetWindowPos(window, nullptr, 80, 80, 850, 440, SWP_NOZORDER);
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
                auto item = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
                    type, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                    reinterpret_cast<HMENU>(INT_PTR(id)), module, nullptr);
                SendMessageW(item, WM_SETFONT, WPARAM(state->font), TRUE); return item;
            };
            add(L"STATIC", L"推理引擎", 0, 0, 16, 20, 170, 24);
            auto combo = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, EngineType, 194, 16, 620, 160);
            for (auto name : {L"auto", L"diffusers", L"sdcpp"}) SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(name));
            auto engine = wide(state->config.value("engine", "auto"));
            SendMessageW(combo, CB_SETCURSEL, SendMessageW(combo, CB_FINDSTRINGEXACT, WPARAM(-1), LPARAM(engine.c_str())), 0);
            int y = 60, id = NativeExe, browse = BrowseNative;
            for (const auto& [name, label] : {std::pair{"sd_executable",L"sd-server.exe（可选）"},
                {"diffusion_model",L"扩散模型"}, {"vae",L"VAE"}, {"llm",L"文本编码器"},
                {"llm_vision",L"视觉投影 / mmproj"}}) {
                add(L"STATIC", label, SS_ENDELLIPSIS, 0, 16, y + 3, 174, 24);
                add(L"EDIT", wide(state->config.value(name, "")).c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                    id++, 194, y, 562, 28);
                add(L"BUTTON", L"…", WS_TABSTOP, browse++, 766, y, 48, 28); y += 40;
            }
            auto check = add(L"BUTTON", L"Qwen-Image 2.1 Turbo（固定 8 步 / CFG 1）",
                WS_TABSTOP | BS_AUTOCHECKBOX, Turbo, 16, 274, 790, 28);
            SendMessageW(check, BM_SETCHECK, state->config.value("turbo", true) ? BST_CHECKED : BST_UNCHECKED, 0);
            add(L"STATIC", L"显存预算 GiB（0 = 自动）", 0, 0, 16, 312, 256, 24);
            add(L"EDIT", wide(state->config.value("max_vram", 0.0) == 0 ? "0" :
                state->config["max_vram"].dump()).c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                MaxVram, 280, 308, 120, 28);
            add(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 554, 344, 120, 32);
            add(L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL, 694, 344, 120, 32);
            return TRUE;
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        int id = LOWORD(wparam);
        if (id >= BrowseNative && id <= BrowseVision) {
            auto path = folder(window, false);
            if (!path.empty()) SetDlgItemTextW(window, NativeExe + id - BrowseNative, path.c_str());
            return TRUE;
        }
        if (id == IDOK) {
            auto value = utf8(text(window, MaxVram)); size_t end{};
            auto budget = std::stod(value, &end);
            require(end == value.size() && budget >= 0 && budget <= 256, "VRAM budget must be 0..256 GiB");
            state->config["engine"] = utf8(text(window, EngineType));
            int edit = NativeExe;
            for (const char* key : {"sd_executable", "diffusion_model", "vae", "llm", "llm_vision"})
                state->config[key] = utf8(text(window, edit++));
            state->config["turbo"] = IsDlgButtonChecked(window, Turbo) == BST_CHECKED;
            state->config["max_vram"] = budget;
            EndDialog(window, IDOK); return TRUE;
        }
        if (id == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
    } catch (const std::exception& error) {
        MessageBoxW(window, wide(error.what()).c_str(), L"Qwen-Image", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
void unload() {
    require(state->child.running() && state->child.serving, "Local service is not running");
    auto& c = state->child.config;
    auto session = WinHttpOpen(L"MemeStudio-Qwen", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    require(session != nullptr, "Cannot open local HTTP session");
    WinHttpSetTimeouts(session, 1000, 1000, 2000, 2000);
    auto connection = WinHttpConnect(session, L"127.0.0.1", INTERNET_PORT(c["port"].get<int>()), 0);
    auto request = connection ? WinHttpOpenRequest(connection, L"POST", L"/admin/unload", nullptr, nullptr, nullptr, 0) : nullptr;
    auto headers = L"Authorization: Bearer " + wide(c["key"].get<std::string>());
    bool success = request && WinHttpSendRequest(request, headers.c_str(), DWORD(-1), nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    DWORD status{}, length = sizeof(status);
    success = success && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &status, &length, nullptr) && status == 200;
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    require(success, "Cannot unload model (service busy or unavailable). Stop service to force release.");
}
void tick(HWND window) {
    bool running = state->child.running(), serving = running && state->child.serving;
    bool ready = serving && fs::exists(state->child.ready);
    auto status = ready ? L"本地服务就绪 · http://127.0.0.1:" +
        std::to_wstring(state->child.config["port"].get<int>()) + L"/v1" :
        running ? serving ? L"正在启动本地服务…" : L"正在检查或安装环境…" : L"本地服务已停止";
    SetDlgItemTextW(window, Status, status.c_str());
    for (int id : {Install, Probe, Start, InspectModel}) EnableWindow(GetDlgItem(window, id), !running);
    EnableWindow(GetDlgItem(window, Stop), running);
    EnableWindow(GetDlgItem(window, Apply), ready); EnableWindow(GetDlgItem(window, Unload), ready);
    auto log = read(state->child.log);
    if (log.size() > 24000) {
        size_t start = log.size() - 24000;
        while (start < log.size() && (static_cast<unsigned char>(log[start]) & 0xc0) == 0x80) ++start;
        log.erase(0, start);
    }
    if (!log.empty()) {
        for (const auto& key : {state->config.value("key", ""),
            state->child.config.is_object() ? state->child.config.value("key", "") : std::string{}})
            for (size_t pos = 0; !key.empty() && (pos = log.find(key, pos)) != std::string::npos; pos += 10)
                log.replace(pos, key.size(), "[REDACTED]");
        auto value = wide(log);
        if (text(window, Logs) != value) {
            SetDlgItemTextW(window, Logs, value.c_str());
            SendDlgItemMessageW(window, Logs, EM_SETSEL, WPARAM(-1), LPARAM(-1));
            SendDlgItemMessageW(window, Logs, EM_SCROLLCARET, 0, 0);
        }
    }
    if (!running && state->child.process.value) {
        DWORD code{}; GetExitCodeProcess(state->child.process.value, &code);
        SetDlgItemTextW(window, Status, code == 0 ? L"操作完成；检查日志后可启动服务" : L"操作失败或服务退出；请查看日志");
    }
}
INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM) {
    try {
        if (message == WM_INITDIALOG) {
            SetWindowTextW(window, L"本地模型支持 - Qwen-Image");
            RECT rect{0, 0, 860, 710};
            AdjustWindowRectEx(&rect, DWORD(GetWindowLongW(window, GWL_STYLE)), FALSE, 0);
            RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            SetWindowPos(window, nullptr, std::max(0L, (area.right - rect.right) / 2),
                std::max(0L, (area.bottom - rect.bottom) / 2), rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
                auto control = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, label,
                    WS_CHILD | WS_VISIBLE | style, x, y, w, h, window, reinterpret_cast<HMENU>(INT_PTR(id)), module, nullptr);
                SendMessageW(control, WM_SETFONT, WPARAM(state->font), TRUE); return control;
            };
            auto field = [&](const wchar_t* label, int id, int y, int browse = 0) {
                add(L"STATIC", label, SS_ENDELLIPSIS, 0, 16, y + 3, 174, 24);
                add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, id, 194, y, browse ? 590 : 646, 28);
                if (browse) add(L"BUTTON", L"…", WS_TABSTOP, browse, 794, y, 46, 28);
            };
            field(L"本地模型目录", ModelDir, 16, BrowseModel);
            MoveWindow(GetDlgItem(window, ModelDir), 194, 16, 440, 28, FALSE);
            add(L"BUTTON", L"检查", WS_TABSTOP, InspectModel, 644, 16, 60, 28);
            add(L"BUTTON", L"GGUF…", WS_TABSTOP, GGUFOptions, 714, 16, 70, 28);
            field(L"Python 程序 / 目录", Python, 54, BrowsePython);
            field(L"新虚拟环境名称", EnvName, 92);
            field(L"已有虚拟环境目录", Environment, 130, BrowseEnvironment);
            field(L"API 模型别名", ModelId, 168);
            field(L"API Key", Key, 206);
            SendDlgItemMessageW(window, Key, EM_SETPASSWORDCHAR, L'*', 0);
            add(L"STATIC", L"端口", 0, 0, 16, 248, 70, 22);
            add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, Port, 90, 244, 90, 28);
            add(L"STATIC", L"推理步数", 0, 0, 204, 248, 80, 22);
            add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, Steps, 290, 244, 80, 28);
            add(L"STATIC", L"True CFG", 0, 0, 400, 248, 90, 22);
            add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Cfg, 500, 244, 80, 28);
            add(L"STATIC", L"安装后端", 0, 0, 16, 286, 90, 22);
            auto backend = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, Backend, 110, 282, 260, 210);
            for (auto item : {L"auto", L"existing", L"cpu", L"cuda", L"xpu", L"vulkan"})
                SendMessageW(backend, CB_ADDSTRING, 0, LPARAM(item));
            add(L"STATIC", L"推理设备", 0, 0, 400, 286, 90, 22);
            auto device = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, Device, 500, 282, 340, 180);
            for (auto item : {L"auto", L"cpu", L"cuda", L"xpu", L"vulkan"}) SendMessageW(device, CB_ADDSTRING, 0, LPARAM(item));
            add(L"BUTTON", L"允许局域网访问", WS_TABSTOP | BS_AUTOCHECKBOX, Lan, 16, 322, 200, 26);
            add(L"BUTTON", L"保留模型（手动卸载）", WS_TABSTOP | BS_AUTOCHECKBOX, Retain, 230, 322, 260, 26);
            add(L"BUTTON", L"GPU / CPU 内存卸载", WS_TABSTOP | BS_AUTOCHECKBOX, Offload, 520, 322, 320, 26);
            int x = 16;
            for (const auto& [id, title] : {std::pair{Install,L"一键配置环境"}, {Probe,L"测试环境"}, {Start,L"启动服务"},
                {Stop,L"停止服务"}, {Unload,L"卸载模型"}}) {
                add(L"BUTTON", title, WS_TABSTOP, id, x, 364, 156, 32); x += 167;
            }
            add(L"STATIC", L"", SS_ENDELLIPSIS, Status, 16, 408, 824, 28);
            add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                Logs, 16, 444, 824, 196);
            add(L"BUTTON", L"应用端点到主界面", WS_TABSTOP | BS_DEFPUSHBUTTON, Apply, 16, 662, 244, 32);
            add(L"BUTTON", L"保存设置", WS_TABSTOP, IDOK, 590, 662, 120, 32);
            add(L"BUTTON", L"关闭", WS_TABSTOP, IDCANCEL, 720, 662, 120, 32);
            fill(window); tick(window); SetTimer(window, 1, 600, nullptr); return TRUE;
        }
        if (message == WM_TIMER) { tick(window); return TRUE; }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message == WM_DESTROY) { KillTimer(window, 1); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case BrowseModel: case BrowsePython: case BrowseEnvironment: {
            auto path = folder(window);
            if (!path.empty()) SetDlgItemTextW(window, LOWORD(wparam) == BrowseModel ? ModelDir :
                LOWORD(wparam) == BrowsePython ? Python : Environment, path.c_str());
            return TRUE;
        }
        case GGUFOptions: {
            state->config = controls(window);
            struct Template { DLGTEMPLATE dialog{}; WORD menu = 0, type = 0, title = 0; } resource;
            resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
            DialogBoxIndirectParamW(module, &resource.dialog, window, nativeOptions, 0);
            return TRUE;
        }
        case Install: case Probe: case Start: case InspectModel: {
            auto config = controls(window);
            if (LOWORD(wparam) == Start) {
                require(fs::is_directory(fs::path(wide(config["model_directory"].get<std::string>()))),
                    "Select a local model directory");
                save(config);
                state->child.launch(envPython(config), L"server.py", L"", config, true);
            } else if (LOWORD(wparam) == InspectModel) {
                auto python = envPython(config);
                if (!fs::is_regular_file(python)) python = pythonPath(wide(config["python"].get<std::string>()));
                state->child.launch(python, L"environment.py", L"--model", config, false);
            } else if (LOWORD(wparam) == Install) {
                save(config);
                auto python = config["environment"].get<std::string>().empty() ?
                    pythonPath(wide(config["python"].get<std::string>())) : envPython(config);
                state->child.launch(python,
                    L"environment.py", L"--install", config, false);
            } else {
                save(config);
                state->child.launch(envPython(config), L"environment.py", L"--verify", config, false);
            }
            tick(window); return TRUE;
        }
        case Stop: state->child.stop(); tick(window); return TRUE;
        case Unload: unload(); SetDlgItemTextW(window, Status, L"模型已卸载；下次生成将重新加载"); return TRUE;
        case Apply:
            require(state->child.running() && state->child.serving && fs::exists(state->child.ready), "Start service first");
            response = Json{{"action","qwen_endpoint"}, {"base_url", "http://127.0.0.1:" +
                std::to_string(state->child.config["port"].get<int>()) + "/v1"},
                {"key",state->child.config["key"]}, {"model",state->child.config["model_id"]}}.dump();
            EndDialog(window, IDOK); return TRUE;
        case IDOK: save(controls(window)); EndDialog(window, IDOK); return TRUE;
        case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
        }
    } catch (const std::exception& error) {
        MessageBoxW(window, wide(error.what()).c_str(), L"Qwen-Image", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
int MEMESTUDIO_CALL initialize(const MemeStudioHostV1* value) {
    try {
        host = value; state = std::make_unique<State>();
        state->config["key"] = randomKey();
        auto path = directory() / L"qwen-settings.json";
        if (fs::is_regular_file(path)) {
            auto saved = Json::parse(read(path));
            auto key = decrypt(saved.at("encrypted_key").get<std::string>());
            saved.erase("encrypted_key"); state->config.update(saved); state->config["key"] = key;
        }
        state->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        return 0;
    } catch (...) { state.reset(); return 1; }
}
const char* MEMESTUDIO_CALL command(const char* id, const char*, void* owner) {
    try {
        response.clear();
        if (!state) return nullptr;
        if (std::string(id) == "stop") { state->child.stop(); return nullptr; }
        if (std::string(id) != "configure") return nullptr;
        struct Template { DLGTEMPLATE dialog{}; WORD menu = 0, type = 0, title = 0; } resource;
        resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
        DialogBoxIndirectParamW(module, &resource.dialog, static_cast<HWND>(owner), procedure, 0);
        return response.empty() ? nullptr : response.c_str();
    } catch (...) { return nullptr; }
}
void MEMESTUDIO_CALL shutdown() { state.reset(); host = nullptr; response.clear(); }
const MemeStudioPluginV1 plugin{sizeof(MemeStudioPluginV1), MEMESTUDIO_PLUGIN_ABI, "memestudio.qwen-image",
    initialize, nullptr, nullptr, command, shutdown};
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { module = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV1* MEMESTUDIO_CALL MemeStudioGetPluginV1(void) { return &plugin; }
