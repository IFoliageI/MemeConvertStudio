#include "upscale.hpp"
#include <shobjidl.h>
#include <algorithm>

namespace studio {
Json upscaleJson(const UpscaleOptions& value) {
    return {{"executable", utf8(value.executable.wstring())}, {"models", utf8(value.models.wstring())},
        {"input", value.input}, {"output", value.output}, {"scale", value.scale}, {"noise", value.noise},
        {"tile", value.tile}, {"gpu", value.gpu}, {"tta", value.tta}, {"timeout", value.timeout}};
}
UpscaleOptions upscaleOptions(const Json& value) {
    UpscaleOptions result;
    result.executable = wide(value.value("executable", ""));
    result.models = wide(value.value("models", ""));
    result.input = value.value("input", false); result.output = value.value("output", false);
    result.scale = value.value("scale", 2); result.noise = value.value("noise", 0);
    result.tile = value.value("tile", 0); result.gpu = value.value("gpu", -2);
    result.tta = value.value("tta", false); result.timeout = value.value("timeout", 600);
    return result;
}
namespace {
void validate(const UpscaleOptions& value) {
    if (!value.input && !value.output) return;
    if (value.executable.empty() || !fs::is_regular_file(value.executable) ||
        _wcsicmp(value.executable.extension().c_str(), L".exe") != 0)
        throw Error("请选择有效的 waifu2x-ncnn-vulkan.exe");
    if (value.scale != 1 && value.scale != 2 && value.scale != 4 && value.scale != 8 &&
        value.scale != 16 && value.scale != 32) throw Error("超分倍率必须为 1、2、4、8、16 或 32");
    if (value.noise < -1 || value.noise > 3 || value.tile < 0 || (value.tile && value.tile < 32) ||
        value.tile > 4096 || value.gpu < -2 || value.gpu > 64 || value.timeout < 5 || value.timeout > 3600)
        throw Error("超分参数超出有效范围");
    auto models = value.models.empty() ? fs::absolute(value.executable).parent_path() / L"models-cunet" : value.models;
    if (!fs::is_directory(models)) throw Error("找不到 Waifu2x 模型目录");
    bool param = false, bin = false;
    for (const auto& file : fs::directory_iterator(models)) if (file.is_regular_file()) {
        param |= file.path().extension() == L".param"; bin |= file.path().extension() == L".bin";
    }
    if (!param || !bin) throw Error("Waifu2x 模型目录需包含 .param 和 .bin 文件");
}
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') ++slashes;
        else {
            if (c == L'"') result.append(slashes * 2 + 1, L'\\');
            else result.append(slashes, L'\\');
            slashes = 0; result += c;
        }
    }
    result.append(slashes * 2, L'\\'); return result + L'"';
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}
Upscaler::Upscaler(const UpscaleOptions& value, Cancel& cancellation, Reporter reporter) :
    options(value), cancel(cancellation), report(std::move(reporter)) {
    validate(options);
    options.executable = fs::absolute(options.executable);
    options.models = options.models.empty() ? options.executable.parent_path() / L"models-cunet" : fs::absolute(options.models);
    signature = upscaleJson(options); signature.erase("input"); signature.erase("output"); signature.erase("timeout");
    signature["executable_sha256"] = digest(readFile(options.executable));
    signature["models_sha256"] = Json::object();
    for (const auto& file : fs::directory_iterator(options.models)) if (file.is_regular_file() &&
        (file.path().extension() == L".param" || file.path().extension() == L".bin"))
        signature["models_sha256"][utf8(file.path().filename().wstring())] = digest(readFile(file.path()));
    GUID id{}; check(CoCreateGuid(&id), "Upscale temporary ID"); wchar_t buffer[40]{};
    StringFromGUID2(id, buffer, 40);
    temporary = fs::temp_directory_path() / (L"MemeStudio-upscale-" + std::wstring(buffer));
    fs::create_directories(temporary);
}
Upscaler::~Upscaler() {
    std::error_code error; if (!temporary.empty()) fs::remove_all(temporary, error);
}
Image Upscaler::run(const Image& image, bool input) {
    cancel.verify();
    auto data = png(image);
    std::string key = digest(data);
    if (input) if (auto found = inputCache.find(key); found != inputCache.end())
        return decode(found->second).frames.front().image;
    uint64_t width = uint64_t(image.width) * options.scale, height = uint64_t(image.height) * options.scale;
    if (width > 16384 || height > 16384 || width * height > 32000000)
        throw Error("超分后的图像超出尺寸上限，请降低倍率");
    auto prefix = std::to_wstring(++index);
    auto source = temporary / (prefix + L"-input.png");
    auto output = temporary / (prefix + L"-output.png");
    auto log = temporary / (prefix + L"-log.txt");
    writeFile(source, data);
    std::wstring command = quote(options.executable.wstring()) + L" -i " + quote(source.wstring()) +
        L" -o " + quote(output.wstring()) + L" -s " + std::to_wstring(options.scale) +
        L" -n " + std::to_wstring(options.noise) + L" -t " + std::to_wstring(options.tile) +
        L" -m " + quote(options.models.wstring()) + L" -f png";
    if (options.gpu != -2) command += L" -g " + std::to_wstring(options.gpu);
    if (options.tta) command += L" -x";
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    Handle logs{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle inputHandle{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (logs.value == INVALID_HANDLE_VALUE || inputHandle.value == INVALID_HANDLE_VALUE)
        throw Error("无法创建超分进程日志");
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw Error("无法设置超分进程取消控制");
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    startup.hStdInput = inputHandle.value; startup.hStdOutput = startup.hStdError = logs.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(options.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, options.executable.parent_path().c_str(), &startup, &process))
        throw Error("无法启动 Waifu2x，Windows 错误 " + std::to_string(GetLastError()));
    Handle handle{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, handle.value)) {
        TerminateProcess(handle.value, 1); WaitForSingleObject(handle.value, INFINITE);
        throw Error("无法管理 Waifu2x 子进程");
    }
    if (ResumeThread(thread.value) == DWORD(-1)) throw Error("无法运行 Waifu2x 子进程");
    if (report) report({0, 0, input ? L"Waifu2x：正在超分输入图像…" : L"Waifu2x：正在超分输出帧…"});
    ULONGLONG started = GetTickCount64();
    for (;;) {
        DWORD wait = WaitForSingleObject(handle.value, 50);
        if (wait == WAIT_OBJECT_0) break;
        if (cancel.stopped || GetTickCount64() - started > ULONGLONG(options.timeout) * 1000 || wait == WAIT_FAILED) {
            TerminateJobObject(job.value, 1); WaitForSingleObject(handle.value, INFINITE);
            cancel.verify(); throw Error("Waifu2x 处理超时或等待失败");
        }
    }
    cancel.verify();
    DWORD code{}; GetExitCodeProcess(handle.value, &code);
    if (code != 0 || !fs::is_regular_file(output))
        throw Error("Waifu2x 处理失败，退出码 " + std::to_string(code) + "；请检查可执行文件、模型及 Vulkan 环境");
    auto result = decode(output).frames.front().image;
    if (result.width != width || result.height != height) throw Error("Waifu2x 返回的尺寸与倍率不匹配");
    if (input) inputCache.emplace(key, output);
    return result;
}
namespace {
enum UpscaleControl { ExePath = 4100, ChooseExe, ModelsPath, ChooseModels, InputEnabled,
    OutputEnabled, Scale, Noise, Tile, Gpu, Tta, Timeout };
struct UpscaleDialog {
    HINSTANCE instance{}; HWND window{}; HFONT font{}; UpscaleOptions options;
    bool accepted = false, capture = false; std::string failure;
    ~UpscaleDialog() { if (font) DeleteObject(font); }
};
std::wstring field(HWND window, int id) {
    auto item = GetDlgItem(window, id);
    std::wstring result(GetWindowTextLengthW(item) + 1, L'\0');
    GetWindowTextW(item, result.data(), int(result.size())); result.pop_back(); return result;
}
int integer(HWND window, int id) {
    auto value = field(window, id); size_t end = 0;
    int parsed = std::stoi(value, &end);
    if (end != value.size()) throw Error("请输入有效的超分参数"); return parsed;
}
INT_PTR CALLBACK upscaleProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<UpscaleDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<UpscaleDialog*>(lparam); dialog->window = window;
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            SetWindowTextW(window, L"Waifu2x 输入/输出超分");
            RECT size{0, 0, 660, 518};
            AdjustWindowRectEx(&size, DWORD(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)));
            SetWindowPos(window, nullptr, 0, 0, size.right - size.left, size.bottom - size.top,
                SWP_NOMOVE | SWP_NOZORDER);
            dialog->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            auto add = [&](const wchar_t* type, const std::wstring& label, DWORD style, int id,
                int x, int y, int width = 628, int height = 28) {
                auto item = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type,
                    label.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, width, height, window,
                    reinterpret_cast<HMENU>(INT_PTR(id)), dialog->instance, nullptr);
                SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font), TRUE); return item;
            };
            add(L"STATIC", L"Waifu2x 可执行文件", 0, 0, 16, 16);
            add(L"EDIT", dialog->options.executable.wstring(), WS_TABSTOP | ES_AUTOHSCROLL, ExePath, 16, 44, 516);
            add(L"BUTTON", L"浏览…", WS_TABSTOP, ChooseExe, 544, 44, 100);
            add(L"STATIC", L"模型目录（留空使用程序旁 models-cunet）", 0, 0, 16, 88);
            add(L"EDIT", dialog->options.models.wstring(), WS_TABSTOP | ES_AUTOHSCROLL, ModelsPath, 16, 116, 516);
            add(L"BUTTON", L"浏览…", WS_TABSTOP, ChooseModels, 544, 116, 100);
            add(L"BUTTON", L"输入图像超分", WS_TABSTOP | BS_AUTOCHECKBOX, InputEnabled, 16, 168, 300);
            add(L"BUTTON", L"输出成品超分", WS_TABSTOP | BS_AUTOCHECKBOX, OutputEnabled, 336, 168, 300);
            SendDlgItemMessageW(window, InputEnabled, BM_SETCHECK, dialog->options.input ? BST_CHECKED : BST_UNCHECKED, 0);
            SendDlgItemMessageW(window, OutputEnabled, BM_SETCHECK, dialog->options.output ? BST_CHECKED : BST_UNCHECKED, 0);
            auto combo = [&](int id, int x, int y, const std::vector<std::wstring>& values, int selected) {
                auto item = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, id, x, y, 300, 200);
                for (auto& value : values) SendMessageW(item, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
                SendMessageW(item, CB_SETCURSEL, selected, 0);
            };
            add(L"STATIC", L"倍率", 0, 0, 16, 216, 300);
            add(L"STATIC", L"降噪级别（-1 关闭）", 0, 0, 336, 216, 300);
            const int scales[]{1, 2, 4, 8, 16, 32};
            int selected = 1;
            for (int i = 0; i < 6; ++i) if (scales[i] == dialog->options.scale) selected = i;
            combo(Scale, 16, 244, {L"1", L"2", L"4", L"8", L"16", L"32"}, selected);
            combo(Noise, 336, 244, {L"-1", L"0", L"1", L"2", L"3"}, std::clamp(dialog->options.noise + 1, 0, 4));
            add(L"STATIC", L"分块大小（0 自动）", 0, 0, 16, 292, 300);
            add(L"STATIC", L"GPU（-2 自动，-1 CPU）", 0, 0, 336, 292, 300);
            add(L"EDIT", std::to_wstring(dialog->options.tile), WS_TABSTOP | ES_AUTOHSCROLL, Tile, 16, 320, 300);
            add(L"EDIT", std::to_wstring(dialog->options.gpu), WS_TABSTOP | ES_AUTOHSCROLL, Gpu, 336, 320, 300);
            add(L"STATIC", L"单帧超时（秒）", 0, 0, 16, 368, 300);
            add(L"EDIT", std::to_wstring(dialog->options.timeout), WS_TABSTOP | ES_AUTOHSCROLL, Timeout, 16, 396, 300);
            add(L"BUTTON", L"TTA 增强", WS_TABSTOP | BS_AUTOCHECKBOX, Tta, 336, 396, 300);
            SendDlgItemMessageW(window, Tta, BM_SETCHECK, dialog->options.tta ? BST_CHECKED : BST_UNCHECKED, 0);
            add(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 416, 468, 108, 32);
            add(L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL, 536, 468, 108, 32);
            if (dialog->capture) PostMessageW(window, WM_APP + 9, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
        if (message == WM_APP + 9 && dialog->capture) {
            RECT client{}, button{}; GetClientRect(window, &client);
            GetWindowRect(GetDlgItem(window, IDOK), &button);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&button), 2);
            if (button.right > client.right || button.bottom > client.bottom)
                throw Error("Upscale action is outside the dialog");
            SendDlgItemMessageW(window, Scale, CB_SETCURSEL, 2, 0);
            SendMessageW(window, WM_COMMAND, IDOK, 0); return TRUE;
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
        case ChooseExe: case ChooseModels: {
            ComPtr<IFileOpenDialog> picker;
            check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)), "Choose Waifu2x");
            FILEOPENDIALOGOPTIONS flags{}; picker->GetOptions(&flags);
            bool models = LOWORD(wparam) == ChooseModels;
            picker->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                (models ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
            COMDLG_FILTERSPEC filter{L"Windows 程序", L"*.exe"};
            if (!models) picker->SetFileTypes(1, &filter);
            auto result = picker->Show(window);
            if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return TRUE;
            check(result, "Waifu2x selection");
            ComPtr<IShellItem> item; check(picker->GetResult(&item), "Waifu2x path");
            PWSTR path{}; check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "Waifu2x file");
            SetDlgItemTextW(window, models ? ModelsPath : ExePath, path); CoTaskMemFree(path); return TRUE;
        }
        case IDOK: {
            auto value = dialog->options;
            value.executable = field(window, ExePath); value.models = field(window, ModelsPath);
            value.input = SendDlgItemMessageW(window, InputEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
            value.output = SendDlgItemMessageW(window, OutputEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
            value.tta = SendDlgItemMessageW(window, Tta, BM_GETCHECK, 0, 0) == BST_CHECKED;
            value.scale = integer(window, Scale); value.noise = integer(window, Noise);
            value.tile = integer(window, Tile); value.gpu = integer(window, Gpu);
            value.timeout = integer(window, Timeout); validate(value);
            dialog->options = value; dialog->accepted = true; EndDialog(window, IDOK); return TRUE;
        }
        }
    } catch (const std::exception& error) {
        if (dialog && dialog->capture) {
            dialog->failure = error.what(); EndDialog(window, IDABORT); return TRUE;
        }
        MessageBoxW(window, wide(error.what()).c_str(), L"Waifu2x", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
}
bool editUpscale(HINSTANCE instance, HWND owner, UpscaleOptions& options, bool capture) {
    UpscaleDialog editor; editor.instance = instance; editor.options = options; editor.capture = capture;
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    resource.dialog.cx = 420; resource.dialog.cy = 330;
    auto result = DialogBoxIndirectParamW(instance, &resource.dialog, owner, upscaleProcedure,
        reinterpret_cast<LPARAM>(&editor));
    if (result == -1) throw Error("无法打开 Waifu2x 设置");
    if (!editor.failure.empty()) throw Error(editor.failure);
    if (!editor.accepted) return false;
    options = editor.options; return true;
}
}
