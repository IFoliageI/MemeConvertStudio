#include "studio.hpp"
#include "plugins.hpp"
#include "presets.hpp"
#include "batch.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <thread>
#include <algorithm>
#include <memory>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <optional>

namespace studio {
namespace {
constexpr UINT WM_PROGRESS = WM_APP + 1, WM_FINISHED = WM_APP + 2;
constexpr int settingsHeight = 1210;
constexpr COLORREF ink = RGB(34, 39, 43), muted = RGB(104, 113, 120);
constexpr COLORREF paper = RGB(245, 247, 248), line = RGB(218, 224, 227);
enum Id {
    Source = 100, Character, AddStyle, RemoveStyle, ClearCrop, StyleList,
    BaseUrl, Key, Model, Quality, Size, ImageField, Extra, Timeout, Stride, MaxFrames,
    Consistency, PreserveText, Resume, Remember, Instruction, Output, BrowseOutput,
    Generate, CancelJob, SourceTab, ResultTab, Play, Timeline, OpenResult, OpenFolder,
    FetchModels, Complexity, OutputFormat, Compression, Streaming, PartialImages,
    GenerationCount, CountSpin, PluginManage, ResultChoice, RefineEntry, InstructionPresets,
    SourceFolder, SourcePrevious, SourceNext, SourcePosition, SourceTotal
};
struct State;
struct View {
    State* state = nullptr;
    int kind = 0;
    RECT imageRect{};
    POINT start{}, end{};
    bool dragging = false;
    double zoom = 1, panX = 0, panY = 0;
    double gestureDistance = 0;
    POINT gesturePoint{};
};
struct Completion {
    bool success = false, modelFetch = false;
    std::wstring message; fs::path output;
    std::vector<std::string> models;
    std::vector<fs::path> outputs;
};
struct State {
    HINSTANCE instance{};
    HWND window{}, settings{}, sourceView{}, characterView{}, mainView{}, styles{},
        log{}, progress{}, status{}, estimate{}, timeline{}, instruction{}, output{},
        sourcePath{}, characterPath{}, cropInfo{}, frameInfo{}, resultChoice{};
    HFONT font{}, titleFont{}, smallFont{};
    HBRUSH background = CreateSolidBrush(paper), white = CreateSolidBrush(RGB(255, 255, 255));
    std::vector<HWND> lockControls;
    Animation sourceAnimation, characterAnimation, resultAnimation;
    std::shared_ptr<Image> previewImage;
    fs::path source, character, resultPath;
    std::vector<fs::path> stylePaths;
    std::vector<fs::path> resultPaths;
    std::vector<SourceItem> sources;
    size_t sourceIndex = 0;
    std::vector<PluginInfo> availablePlugins;
    std::vector<std::string> enabledPlugins;
    std::shared_ptr<Plugins> plugins;
    UpscaleOptions upscale;
    Crop crop;
    View views[3];
    int frame = 0, elapsed = 0, scroll = 0;
    double scrollFraction = 0;
    bool showingResult = false, playing = true, busy = false, closing = false, demo = false,
        fetchingModels = false;
    bool changingSourcePosition = false;
    Cancel cancel;
    std::thread worker;
    ~State() {
        cancel.stop();
        if (worker.joinable()) worker.join();
        DeleteObject(font); DeleteObject(titleFont); DeleteObject(smallFont);
        DeleteObject(background); DeleteObject(white);
    }
    HWND get(int id) const { return GetDlgItem(settings, id); }
    std::wstring text(HWND control) const {
        int count = GetWindowTextLengthW(control);
        std::wstring value(count + 1, L'\0');
        GetWindowTextW(control, value.data(), count + 1); value.resize(count); return value;
    }
    void set(HWND control, const std::wstring& value) { SetWindowTextW(control, value.c_str()); }
    bool checked(int id) const { return SendMessageW(get(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void checkBox(int id, bool value) { SendMessageW(get(id), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
    void logLine(const std::wstring& message) {
        int length = GetWindowTextLengthW(log);
        SendMessageW(log, EM_SETSEL, length, length);
        auto entry = message + L"\r\n";
        SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(entry.c_str()));
        SendMessageW(log, EM_SCROLLCARET, 0, 0);
    }
    Animation& active() { return showingResult ? resultAnimation : sourceAnimation; }
    bool batchEnabled() const {
        return std::find(enabledPlugins.begin(), enabledPlugins.end(),
            "memestudio.batch-convert") != enabledPlugins.end();
    }
    void refreshViews() {
        frame = 0; elapsed = 0;
        for (auto& view : views) { view.zoom = 1; view.panX = view.panY = 0; }
        SendMessageW(timeline, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageW(timeline, TBM_SETRANGEMAX, TRUE, std::max(0, int(active().frames.size()) - 1));
        SendMessageW(timeline, TBM_SETPOS, TRUE, 0);
        InvalidateRect(mainView, nullptr, FALSE);
        InvalidateRect(sourceView, nullptr, FALSE);
        InvalidateRect(characterView, nullptr, FALSE);
        updateFrameLabel();
    }
    void updateFrameLabel() {
        auto count = active().frames.size();
        set(frameInfo, count ? std::to_wstring(frame + 1) + L" / " + std::to_wstring(count) : L"0 / 0");
    }
    int number(int id, int minimum, int maximum) {
        auto value = text(get(id));
        if (value.empty() || !std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= '0' && c <= '9'; }))
            throw Error("请输入有效的数值参数");
        long long parsed = std::stoll(value);
        if (parsed < minimum || parsed > maximum) throw Error("数值参数超出范围");
        return static_cast<int>(parsed);
    }
    void updateEstimate() {
        if (sourceAnimation.frames.empty()) { set(estimate, L"尚未导入表情素材"); return; }
        try {
            int count = static_cast<int>(sourceAnimation.frames.size());
            int stride = number(Stride, 1, 600), maximum = number(MaxFrames, 0, 600);
            if (maximum) stride = std::max(stride, (count + maximum - 1) / maximum);
            int calls = ((count + stride - 1) / stride) * number(GenerationCount, 1, 10);
            if (batchEnabled() && sources.size() > 1) {
                Job options; options.stride = number(Stride, 1, 600);
                options.maxFrames = number(MaxFrames, 0, 600);
                options.generationCount = number(GenerationCount, 1, 10);
                calls = 0;
                for (const auto& item : sources) calls += sourceCalls(item, options);
            }
            uint64_t duration = 0;
            for (auto& item : sourceAnimation.frames) duration += item.delayMs;
            std::wostringstream durationText;
            durationText << std::fixed << std::setprecision(2) << duration / 1000.0;
            set(estimate, std::to_wstring(sourceAnimation.frames[0].image.width) + L" × " +
                std::to_wstring(sourceAnimation.frames[0].image.height) + L"  ·  " +
                std::to_wstring(count) + L" 帧  ·  " + durationText.str() +
                L" 秒\r\n" + (batchEnabled() && sources.size() > 1 ?
                    std::to_wstring(sources.size()) + L" 份素材 · " : L"") +
                L"预计最多 " + std::to_wstring(calls) + L" 次生成请求");
        } catch (...) { set(estimate, L"请检查抽帧参数"); }
    }
};
static LRESULT CALLBACK settingsWheel(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR parent) {
    if (message == WM_MOUSEWHEEL) {
        wchar_t type[32]{}; GetClassNameW(window, type, 32);
        bool dropped = wcscmp(type, L"ComboBox") == 0 && SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0);
        if (!dropped) {
            SendMessageW(reinterpret_cast<HWND>(parent), message, wparam, lparam);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, settingsWheel, 1);
    return DefSubclassProc(window, message, wparam, lparam);
}
static HWND control(State& state, HWND parent, const wchar_t* type, const std::wstring& label,
    DWORD style, int id, int x, int y, int width, int height, bool lock = false) {
    HWND result = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
        type, label.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, width, height,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), state.instance, nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    if (parent == state.settings && !(wcscmp(type, L"EDIT") == 0 && (style & ES_MULTILINE)))
        SetWindowSubclass(result, settingsWheel, 1, reinterpret_cast<DWORD_PTR>(parent));
    if (lock) state.lockControls.push_back(result);
    return result;
}
enum PresetId { PresetCategory = 2200, PresetList, PresetPreview, PresetReplace };
struct PresetDialog {
    State* state;
    HWND category{}, list{}, preview{};
    int captureCommand = 0;
    std::function<void(HWND)> capture;
    std::wstring value;
    std::string failure;
};
static void previewPreset(PresetDialog& dialog) {
    auto selected = SendMessageW(dialog.list, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR) return;
    auto index = SendMessageW(dialog.list, LB_GETITEMDATA, selected, 0);
    dialog.state->set(dialog.preview, instructionPresets[index].instruction);
}
static void fillPresets(PresetDialog& dialog) {
    auto category = dialog.state->text(dialog.category);
    SendMessageW(dialog.list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < std::size(instructionPresets); ++i) {
        const auto& preset = instructionPresets[i];
        if (category != L"全部" && category != preset.category) continue;
        auto label = category == L"全部" ? std::wstring(preset.category) + L" · " + preset.title : preset.title;
        auto row = SendMessageW(dialog.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(dialog.list, LB_SETITEMDATA, row, i);
    }
    SendMessageW(dialog.list, LB_SETCURSEL, 0, 0);
    previewPreset(dialog);
}
static INT_PTR CALLBACK presetProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<PresetDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<PresetDialog*>(lparam);
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            auto& state = *dialog->state;
            SetWindowTextW(window, L"附加要求预设");
            RECT rect{0, 0, 760, 428}, owner{};
            AdjustWindowRectEx(&rect, GetWindowLongW(window, GWL_STYLE), FALSE,
                GetWindowLongW(window, GWL_EXSTYLE));
            GetWindowRect(state.window, &owner);
            int width = rect.right - rect.left, height = rect.bottom - rect.top;
            SetWindowPos(window, nullptr, owner.left + (owner.right - owner.left - width) / 2,
                owner.top + (owner.bottom - owner.top - height) / 2, width, height, SWP_NOZORDER);
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id,
                int x, int y, int w, int h) {
                return control(state, window, type, label, style, id, x, y, w, h);
            };
            add(L"STATIC", L"分类", SS_LEFT, 0, 16, 18, 40, 22);
            dialog->category = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                PresetCategory, 64, 14, 208, 240);
            SendMessageW(dialog->category, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部"));
            for (const auto& preset : instructionPresets)
                if (SendMessageW(dialog->category, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(preset.category)) == CB_ERR)
                    SendMessageW(dialog->category, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.category));
            SendMessageW(dialog->category, CB_SETCURSEL, 0, 0);
            dialog->list = add(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, PresetList, 16, 54, 256, 300);
            add(L"STATIC", L"要求内容", SS_LEFT, 0, 292, 18, 200, 22);
            dialog->preview = add(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                ES_AUTOVSCROLL | WS_VSCROLL, PresetPreview, 292, 54, 452, 300);
            add(L"BUTTON", L"追加", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 432, 380, 96, 32);
            add(L"BUTTON", L"替换全部", WS_TABSTOP | BS_PUSHBUTTON, PresetReplace, 540, 380, 96, 32);
            add(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 648, 380, 96, 32);
            fillPresets(*dialog);
            if (dialog->captureCommand) PostMessageW(window, WM_APP + 9, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
        if (message == WM_APP + 9 && dialog->captureCommand) {
            // Exercise filtering and selection through the same notifications as user input.
            SendMessageW(dialog->category, CB_SETCURSEL, 5, 0);
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(PresetCategory, CBN_SELCHANGE), 0);
            if (SendMessageW(dialog->list, LB_GETCOUNT, 0, 0) != 3 ||
                dialog->state->text(dialog->preview).find(L"GIF") == std::wstring::npos)
                throw Error("Preset category or preview failed");
            SendMessageW(dialog->list, LB_SETCURSEL, 1, 0);
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(PresetList, LBN_SELCHANGE), 0);
            if (dialog->capture) dialog->capture(window);
            SendMessageW(window, WM_COMMAND, dialog->captureCommand, 0);
            return TRUE;
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        int id = LOWORD(wparam);
        if (id == PresetCategory && HIWORD(wparam) == CBN_SELCHANGE) fillPresets(*dialog);
        else if (id == PresetList && HIWORD(wparam) == LBN_SELCHANGE) previewPreset(*dialog);
        else if (id == IDCANCEL) EndDialog(window, IDCANCEL);
        else if (id == IDOK || id == PresetReplace) {
            dialog->value = dialog->state->text(dialog->preview);
            if (!dialog->value.empty()) EndDialog(window, id);
        }
    } catch (const std::exception& error) {
        if (dialog && dialog->captureCommand) {
            dialog->failure = error.what(); EndDialog(window, IDABORT);
        } else MessageBoxW(window, wide(error.what()).c_str(), L"附加要求预设", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
static void choosePreset(State& state, int captureCommand = 0, std::function<void(HWND)> capture = {}) {
    if (state.busy) return;
    PresetDialog dialog{&state};
    dialog.captureCommand = captureCommand; dialog.capture = std::move(capture);
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_CLIPCHILDREN;
    resource.dialog.cx = 470; resource.dialog.cy = 260;
    auto result = DialogBoxIndirectParamW(state.instance, &resource.dialog, state.window, presetProcedure,
        reinterpret_cast<LPARAM>(&dialog));
    if (result == -1) throw Error("无法打开预设窗口");
    if (!dialog.failure.empty()) throw Error(dialog.failure);
    if (result != IDOK && result != PresetReplace) return;
    auto current = state.text(state.instruction);
    auto value = dialog.value;
    if (result == IDOK) {
        if (!current.empty() && current.back() != L'\n') value = L"\r\n" + value;
        SendMessageW(state.instruction, EM_SETSEL, current.size(), current.size());
    } else SendMessageW(state.instruction, EM_SETSEL, 0, -1);
    SendMessageW(state.instruction, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(value.c_str()));
    SetFocus(state.instruction);
    SendMessageW(state.instruction, EM_SCROLLCARET, 0, 0);
}
static std::vector<fs::path> chooseImages(HWND owner, bool multi) {
    ComPtr<IFileOpenDialog> dialog;
    check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)), "Open file dialog");
    FILEOPENDIALOGOPTIONS flags; dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM |
        (multi ? FOS_ALLOWMULTISELECT : 0));
    COMDLG_FILTERSPEC types[]{{L"图片 / GIF", L"*.png;*.jpg;*.jpeg;*.webp;*.gif;*.bmp;*.tif;*.tiff"},
        {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(2, types);
    if (dialog->Show(owner) == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    ComPtr<IShellItemArray> items; check(dialog->GetResults(&items), "Selected files");
    DWORD count; items->GetCount(&count);
    std::vector<fs::path> result;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item; check(items->GetItemAt(i, &item), "Selected file");
        PWSTR path; check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "Selected path");
        result.emplace_back(path); CoTaskMemFree(path);
    }
    return result;
}
static fs::path chooseOutput(State& state) {
    ComPtr<IFileSaveDialog> dialog;
    check(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)), "Save file dialog");
    bool animated = state.sourceAnimation.animated;
    COMDLG_FILTERSPEC type{animated ? L"GIF 动画" : L"PNG 图片", animated ? L"*.gif" : L"*.png"};
    dialog->SetFileTypes(1, &type); dialog->SetDefaultExtension(animated ? L"gif" : L"png");
    auto current = state.text(state.output);
    dialog->SetFileName(current.empty() ? L"meme-edited" : fs::path(current).filename().c_str());
    if (dialog->Show(state.window) == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    ComPtr<IShellItem> item; check(dialog->GetResult(&item), "Output file");
    PWSTR path; check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "Output path");
    fs::path result(path); CoTaskMemFree(path); return result;
}
static fs::path chooseFolder(State& state, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dialog;
    check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)), "Folder dialog");
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    auto status = dialog->Show(state.window);
    if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    check(status, "Select folder");
    ComPtr<IShellItem> item; check(dialog->GetResult(&item), "Selected folder");
    PWSTR path; check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "Folder path");
    fs::path result(path); CoTaskMemFree(path); return result;
}
static RECT fit(const Image& image, RECT area) {
    int width = area.right - area.left, height = area.bottom - area.top;
    double scale = std::min(double(width) / image.width, double(height) / image.height);
    int w = std::max(1, int(image.width * scale)), h = std::max(1, int(image.height * scale));
    int x = area.left + (width - w) / 2, y = area.top + (height - h) / 2;
    return {x, y, x + w, y + h};
}
static RECT transformed(View& view, const Image& image, RECT area) {
    RECT base = fit(image, area);
    double width = (base.right - base.left) * view.zoom;
    double height = (base.bottom - base.top) * view.zoom;
    double limitX = std::max(0.0, (width - (area.right - area.left)) / 2);
    double limitY = std::max(0.0, (height - (area.bottom - area.top)) / 2);
    view.panX = std::clamp(view.panX, -limitX, limitX);
    view.panY = std::clamp(view.panY, -limitY, limitY);
    LONG x = LONG(std::lround((area.left + area.right - width) / 2 + view.panX));
    LONG y = LONG(std::lround((area.top + area.bottom - height) / 2 + view.panY));
    return {x, y, x + LONG(std::lround(width)), y + LONG(std::lround(height))};
}
static void zoomAt(View& view, double factor, POINT point, RECT client) {
    double next = std::clamp(view.zoom * factor, 1.0, 8.0);
    double ratio = next / view.zoom;
    double x = point.x - (client.right + client.left) / 2.0;
    double y = point.y - (client.bottom + client.top) / 2.0;
    view.panX = x - (x - view.panX) * ratio;
    view.panY = y - (y - view.panY) * ratio;
    view.zoom = next;
}
static void drawImage(HDC target, const Image& image, RECT rect) {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width; info.bmiHeader.biHeight = -LONG(image.height);
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* memory = nullptr;
    HBITMAP dib = CreateDIBSection(target, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
    if (!dib) return;
    memcpy(memory, image.pixels.data(), image.pixels.size());
    BYTE* pixels = static_cast<BYTE*>(memory);
    for (size_t i = 0; i < image.pixels.size(); i += 4)
        for (size_t channel = 0; channel < 3; ++channel)
            pixels[i + channel] = BYTE((UINT(pixels[i + channel]) * pixels[i + 3] + 127) / 255);
    HDC source = CreateCompatibleDC(target);
    auto old = SelectObject(source, dib);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(target, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        source, 0, 0, image.width, image.height, blend);
    SelectObject(source, old); DeleteDC(source); DeleteObject(dib);
}
static void dropFiles(State& state, HDROP drop, int kind);
static LRESULT CALLBACK viewProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* view = reinterpret_cast<View*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    }
    if (!view) return DefWindowProcW(window, message, wparam, lparam);
    auto& state = *view->state;
    if (message == WM_DROPFILES) {
        dropFiles(state, reinterpret_cast<HDROP>(wparam), view->kind); return 0;
    }
    if (message == WM_CREATE) {
        GESTURECONFIG gestures[]{{GID_ZOOM, GC_ZOOM, 0},
            {GID_PAN, GC_PAN | GC_PAN_WITH_INERTIA, 0}};
        SetGestureConfig(window, 0, 2, gestures, sizeof(GESTURECONFIG));
        return 0;
    }
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        RECT client; GetClientRect(window, &client);
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        double delta = double(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        if (message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL))
            zoomAt(*view, std::pow(1.15, delta), point, client);
        else if (message == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT))
            view->panX += (message == WM_MOUSEHWHEEL ? -1 : 1) * delta * 64;
        else view->panY += delta * 64;
        InvalidateRect(window, nullptr, FALSE); return 0;
    }
    if (message == WM_GESTURE) {
        GESTUREINFO gesture{sizeof(gesture)};
        if (!GetGestureInfo(reinterpret_cast<HGESTUREINFO>(lparam), &gesture))
            return DefWindowProcW(window, message, wparam, lparam);
        if (gesture.dwID != GID_ZOOM && gesture.dwID != GID_PAN)
            return DefWindowProcW(window, message, wparam, lparam);
        POINT point{gesture.ptsLocation.x, gesture.ptsLocation.y};
        ScreenToClient(window, &point);
        if (gesture.dwID == GID_ZOOM) {
            double distance = double(gesture.ullArguments);
            if (!(gesture.dwFlags & GF_BEGIN) && view->gestureDistance > 0) {
                RECT client; GetClientRect(window, &client);
                zoomAt(*view, distance / view->gestureDistance, point, client);
            }
            view->gestureDistance = distance;
        } else {
            if (!(gesture.dwFlags & GF_BEGIN)) {
                view->panX += point.x - view->gesturePoint.x;
                view->panY += point.y - view->gesturePoint.y;
            }
            view->gesturePoint = point;
        }
        CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(lparam));
        InvalidateRect(window, nullptr, FALSE); return 0;
    }
    if (message == WM_LBUTTONDBLCLK) {
        view->zoom = 1; view->panX = view->panY = 0;
        InvalidateRect(window, nullptr, FALSE); return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wparam) : BeginPaint(window, &paint);
        RECT client; GetClientRect(window, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP back = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
        auto old = SelectObject(buffer, back);
        FillRect(buffer, &client, state.white);
        HBRUSH checker = CreateSolidBrush(RGB(237, 240, 242));
        for (int y = 0; y < client.bottom; y += 16)
            for (int x = 0; x < client.right; x += 16)
                if ((x / 16 + y / 16) % 2 == 0) { RECT tile{x, y, x + 16, y + 16}; FillRect(buffer, &tile, checker); }
        DeleteObject(checker);
        Animation* animation = view->kind == 0 ? &state.sourceAnimation :
            view->kind == 1 ? &state.characterAnimation : &state.active();
        const Image* imageToDraw = nullptr;
        if (view->kind == 2 && state.showingResult && state.previewImage)
            imageToDraw = state.previewImage.get();
        else if (!animation->frames.empty()) {
            int index = view->kind == 1 ? 0 : state.frame % static_cast<int>(animation->frames.size());
            imageToDraw = &animation->frames[index].image;
        }
        if (imageToDraw) {
            const auto& image = *imageToDraw;
            RECT inset{10, 10, client.right - 10, client.bottom - 10};
            view->imageRect = transformed(*view, image, inset);
            drawImage(buffer, image, view->imageRect);
            if (view->kind == 1 && (state.crop.width < 0.999 || state.crop.height < 0.999 || view->dragging)) {
                auto bounds = view->imageRect;
                RECT selection{
                    bounds.left + LONG(state.crop.x * (bounds.right - bounds.left)),
                    bounds.top + LONG(state.crop.y * (bounds.bottom - bounds.top)),
                    bounds.left + LONG((state.crop.x + state.crop.width) * (bounds.right - bounds.left)),
                    bounds.top + LONG((state.crop.y + state.crop.height) * (bounds.bottom - bounds.top))};
                if (view->dragging) selection = {std::min(view->start.x, view->end.x),
                    std::min(view->start.y, view->end.y), std::max(view->start.x, view->end.x),
                    std::max(view->start.y, view->end.y)};
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(9, 133, 121));
                auto oldPen = SelectObject(buffer, pen), oldBrush = SelectObject(buffer, GetStockObject(HOLLOW_BRUSH));
                Rectangle(buffer, selection.left, selection.top, selection.right, selection.bottom);
                SelectObject(buffer, oldPen); SelectObject(buffer, oldBrush); DeleteObject(pen);
            }
        } else {
            SetBkMode(buffer, TRANSPARENT); SetTextColor(buffer, muted);
            SelectObject(buffer, state.font);
            auto label = view->kind == 1 ? L"参考人物" : L"暂无图像";
            DrawTextW(buffer, label, -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        FrameRect(buffer, &client, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old); DeleteObject(back); DeleteDC(buffer);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    auto clampPoint = [&](LPARAM position) {
        return POINT{std::clamp(GET_X_LPARAM(position), int(view->imageRect.left), int(view->imageRect.right)),
            std::clamp(GET_Y_LPARAM(position), int(view->imageRect.top), int(view->imageRect.bottom))};
    };
    if (view->kind == 1 && !state.busy && !state.characterAnimation.frames.empty()) {
        if (message == WM_LBUTTONDOWN) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (PtInRect(&view->imageRect, point)) {
                view->start = view->end = point; view->dragging = true; SetCapture(window);
            }
            return 0;
        }
        if (message == WM_MOUSEMOVE && view->dragging) {
            view->end = clampPoint(lparam); InvalidateRect(window, nullptr, FALSE); return 0;
        }
        if (message == WM_LBUTTONUP && view->dragging) {
            view->end = clampPoint(lparam); view->dragging = false; ReleaseCapture();
            auto bounds = view->imageRect;
            if (abs(view->end.x - view->start.x) > 5 && abs(view->end.y - view->start.y) > 5) {
                state.crop = {
                    double(std::min(view->start.x, view->end.x) - bounds.left) / (bounds.right - bounds.left),
                    double(std::min(view->start.y, view->end.y) - bounds.top) / (bounds.bottom - bounds.top),
                    double(abs(view->end.x - view->start.x)) / (bounds.right - bounds.left),
                    double(abs(view->end.y - view->start.y)) / (bounds.bottom - bounds.top)};
                state.set(state.cropInfo, L"已设置人物裁剪区域");
            }
            InvalidateRect(window, nullptr, FALSE); return 0;
        }
        if (message == WM_CAPTURECHANGED) { view->dragging = false; InvalidateRect(window, nullptr, FALSE); }
        if (message == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE; }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static LRESULT CALLBACK settingsProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_COMMAND) return SendMessageW(state->window, message, wparam, lparam);
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT) {
        SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 255, 255));
        SetTextColor(reinterpret_cast<HDC>(wparam), ink); return reinterpret_cast<LRESULT>(state->white);
    }
    if (message == WM_SIZE) {
        RECT rect; GetClientRect(window, &rect);
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, settingsHeight,
            static_cast<UINT>(rect.bottom), state->scroll, 0};
        SetScrollInfo(window, SB_VERT, &info, TRUE);
        int next = GetScrollPos(window, SB_VERT);
        ScrollWindowEx(window, 0, state->scroll - next, nullptr, nullptr, nullptr, nullptr,
            SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        state->scroll = next;
    }
    if (message == WM_VSCROLL || message == WM_MOUSEWHEEL) {
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL}; GetScrollInfo(window, SB_VERT, &info);
        int next = info.nPos;
        if (message == WM_MOUSEWHEEL) {
            state->scrollFraction -= double(GET_WHEEL_DELTA_WPARAM(wparam)) * 64 / WHEEL_DELTA;
            int movement = int(state->scrollFraction);
            next += movement; state->scrollFraction -= movement;
        }
        else switch (LOWORD(wparam)) {
            case SB_LINEUP: next -= 28; break;
            case SB_LINEDOWN: next += 28; break;
            case SB_PAGEUP: next -= info.nPage; break;
            case SB_PAGEDOWN: next += info.nPage; break;
            case SB_THUMBTRACK: next = info.nTrackPos; break;
            case SB_TOP: next = 0; break;
            case SB_BOTTOM: next = info.nMax; break;
        }
        next = std::clamp(next, 0, std::max(0, info.nMax - int(info.nPage) + 1));
        ScrollWindowEx(window, 0, state->scroll - next, nullptr, nullptr, nullptr, nullptr,
            SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        state->scroll = next; SetScrollPos(window, SB_VERT, next, TRUE); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static void createSettings(State& state) {
    auto label = [&](const wchar_t* text, int y) {
        control(state, state.settings, L"STATIC", text, 0, 0, 16, y, 292, 20);
    };
    auto editControl = [&](int id, const wchar_t* value, int y, int height = 28, DWORD extra = 0) {
        return control(state, state.settings, L"EDIT", value, WS_TABSTOP | ES_AUTOHSCROLL | extra,
            id, 16, y, 292, height, true);
    };
    auto combo = [&](int id, int y, const std::vector<std::wstring>& values, bool editable = false) {
        auto result = control(state, state.settings, L"COMBOBOX", L"",
            WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST),
            id, 16, y, 292, 200, true);
        for (const auto& value : values) SendMessageW(result, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        SendMessageW(result, CB_SETCURSEL, 0, 0); return result;
    };
    auto checkbox = [&](int id, const wchar_t* text, int y, bool value) {
        control(state, state.settings, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP,
            id, 16, y, 292, 24, true); state.checkBox(id, value);
    };
    label(L"供应商", 12);
    label(L"Base URL / 完整编辑接口地址", 44); editControl(BaseUrl, L"https://api.openai.com/v1", 66);
    label(L"API Key", 104); editControl(Key, L"", 126, 28, ES_PASSWORD);
    checkbox(Remember, L"加密记住密钥（当前用户）", 160, false);
    label(L"模型", 194);
    auto model = combo(Model, 216, {L"gpt-image-1", L"gpt-image-1.5"}, true);
    MoveWindow(model, 16, 216, 194, 200, TRUE);
    SendMessageW(model, CB_SETDROPPEDWIDTH, 292, 0);
    control(state, state.settings, L"BUTTON", L"获取模型", WS_TABSTOP | BS_PUSHBUTTON,
        FetchModels, 218, 216, 90, 28, true);
    label(L"质量 Quality", 256); combo(Quality, 278, {L"auto", L"low", L"medium", L"high", L"xhigh", L"max", L"不发送"});
    label(L"复杂度 Complexity", 318); combo(Complexity, 340, {L"不发送", L"low", L"medium", L"high"});
    label(L"返回格式 Format", 380); combo(OutputFormat, 402, {L"不发送", L"png", L"jpeg", L"webp"});
    label(L"压缩质量 Compression (0–100)", 442);
    combo(Compression, 464, {L"不发送", L"100", L"90", L"80", L"70", L"50", L"25", L"0"}, true);
    checkbox(Streaming, L"流式接收局部图像", 504, true);
    label(L"局部预览数量", 538); combo(PartialImages, 560, {L"2", L"1", L"3"});
    label(L"输出尺寸参数", 600); combo(Size, 622,
        {L"auto", L"1024x1024", L"1536x1024", L"1024x1536", L"不发送"}, true);
    label(L"多图上传字段", 662); combo(ImageField, 684, {L"image[]", L"image"});
    label(L"扩展参数（JSON）", 724);
    editControl(Extra, L"{}", 746, 60, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
    label(L"单次请求超时（秒）", 818); editControl(Timeout, L"600", 840, 28, ES_NUMBER);
    label(L"GIF 间隔 / 帧数上限（0 不限）", 882);
    auto stride = editControl(Stride, L"1", 906, 28, ES_NUMBER);
    auto maximum = editControl(MaxFrames, L"24", 906, 28, ES_NUMBER);
    MoveWindow(stride, 16, 906, 138, 28, TRUE); MoveWindow(maximum, 170, 906, 138, 28, TRUE);
    checkbox(Consistency, L"多帧参考：动作、画风与背景一致", 950, true);
    checkbox(PreserveText, L"保留原表情文字", 984, true);
    checkbox(Resume, L"复用已完成帧，支持断点续作", 1018, true);
    label(L"生成张数 / GIF 份数（1–10）", 1056);
    auto count = editControl(GenerationCount, L"1", 1080, 28, ES_NUMBER);
    auto spin = control(state, state.settings, UPDOWN_CLASSW, L"",
        UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS, CountSpin, 0, 0, 20, 28, true);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(count), 0);
    SendMessageW(spin, UDM_SETRANGE32, 1, 10);
    SendMessageW(spin, UDM_SETPOS32, 0, 1);
    control(state, state.settings, L"BUTTON", L"插件管理…", WS_TABSTOP | BS_PUSHBUTTON,
        PluginManage, 16, 1132, 292, 32, true);
}
static void updateProviderControls(State& state) {
    auto format = state.text(state.get(OutputFormat));
    EnableWindow(state.get(Compression), !state.busy && (format == L"jpeg" || format == L"webp"));
    EnableWindow(state.get(PartialImages), !state.busy && state.checked(Streaming));
}
static void saveSettings(State& state) {
    if (state.demo) return;
    Json data;
    for (int id : {BaseUrl, Model, Quality, Size, ImageField, Extra, Timeout, Stride, MaxFrames,
        Complexity, OutputFormat, Compression, PartialImages, GenerationCount})
        data[std::to_string(id)] = utf8(state.text(state.get(id)));
    for (int id : {Consistency, PreserveText, Resume, Remember, Streaming}) data[std::to_string(id)] = state.checked(id);
    if (state.checked(Remember)) data["encrypted_key"] = protect(state.text(state.get(Key)));
    data["enabled_plugins"] = state.enabledPlugins;
    data["upscale"] = upscaleJson(state.upscale);
    writeJson(settingsPath(), data);
}
static void loadSettings(State& state) {
    if (state.demo) return;
    try {
        if (!fs::exists(settingsPath())) return;
        Json data = readJson(settingsPath());
        for (int id : {BaseUrl, Model, Quality, Size, ImageField, Extra, Timeout, Stride, MaxFrames,
            Complexity, OutputFormat, Compression, PartialImages, GenerationCount}) {
            if (!data.contains(std::to_string(id))) continue;
            auto text = wide(data.at(std::to_string(id)).get<std::string>());
            HWND item = state.get(id);
            if (id == Quality || id == ImageField || id == Complexity || id == OutputFormat || id == PartialImages) {
                auto index = SendMessageW(item, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text.c_str()));
                if (index != CB_ERR) SendMessageW(item, CB_SETCURSEL, index, 0);
            } else state.set(item, text);
        }
        for (int id : {Consistency, PreserveText, Resume, Remember, Streaming})
            if (data.contains(std::to_string(id))) state.checkBox(id, data[std::to_string(id)].get<bool>());
        if (state.checked(Remember) && data.contains("encrypted_key"))
            state.set(state.get(Key), unprotect(data["encrypted_key"].get<std::string>()));
        state.enabledPlugins = data.value("enabled_plugins", std::vector<std::string>{});
        if (data.contains("upscale")) state.upscale = upscaleOptions(data["upscale"]);
    } catch (const std::exception& error) {
        state.logLine(L"读取设置失败：" + wide(error.what()));
    }
}
static void updateSourceControls(State& state) {
    bool enabled = state.batchEnabled();
    for (int id : {SourceFolder, SourcePrevious, SourceNext, SourcePosition, SourceTotal})
        ShowWindow(GetDlgItem(state.window, id), enabled ? SW_SHOW : SW_HIDE);
    MoveWindow(state.sourceView, 16, 134, 242, enabled ? 90 : 126, TRUE);
    state.changingSourcePosition = true;
    state.set(GetDlgItem(state.window, SourcePosition),
        state.sources.empty() ? L"0" : std::to_wstring(state.sourceIndex + 1));
    state.changingSourcePosition = false;
    state.set(GetDlgItem(state.window, SourceTotal), L"/ " + std::to_wstring(state.sources.size()));
    EnableWindow(GetDlgItem(state.window, SourcePrevious), enabled && !state.busy && state.sourceIndex > 0);
    EnableWindow(GetDlgItem(state.window, SourceNext), enabled && !state.busy &&
        state.sourceIndex + 1 < state.sources.size());
    EnableWindow(GetDlgItem(state.window, SourcePosition), enabled && !state.busy && !state.sources.empty());
    EnableWindow(GetDlgItem(state.window, SourceFolder), enabled && !state.busy);
    bool batch = enabled && state.sources.size() > 1;
    state.set(GetDlgItem(state.window, Generate), batch ? L"批量转换" : L"开始生成");
    SendMessageW(state.output, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(
        batch ? L"批量输出文件夹" : L"输出文件路径"));
    state.updateEstimate();
}
static void loadSource(State& state, const fs::path& path) {
    auto animation = decode(path);
    state.source = path; state.sourceAnimation = std::move(animation);
    state.resultAnimation = {}; state.resultPath.clear();
    state.resultPaths.clear(); SendMessageW(state.resultChoice, CB_RESETCONTENT, 0, 0);
    ShowWindow(state.resultChoice, SW_HIDE);
    state.previewImage.reset();
    state.sources = {{path, static_cast<int>(state.sourceAnimation.frames.size()), state.sourceAnimation.animated}};
    state.sourceIndex = 0;
    state.set(state.sourcePath, path.filename().wstring());
    fs::path output = path.parent_path() / (path.stem().wstring() + L"-edited" +
        (state.sourceAnimation.animated ? L".gif" : L".png"));
    state.set(state.output, output.wstring());
    state.showingResult = false; state.refreshViews(); state.updateEstimate();
    state.set(state.status, L"素材已就绪"); updateSourceControls(state);
}
static void importSources(State& state, const std::vector<fs::path>& paths) {
    if (state.busy || paths.empty()) return;
    auto sources = collectSources(paths, [&](const auto& message) { state.logLine(message); });
    // Commit only after validating the new selection, keeping the previous queue on failure.
    auto directory = sources.front().path.parent_path() / L"MemeConvertStudio-output";
    loadSource(state, sources.front().path);
    state.sources = std::move(sources);
    if (state.sources.size() > 1) state.set(state.output, directory.wstring());
    updateSourceControls(state);
    state.set(state.status, L"已导入 " + std::to_wstring(state.sources.size()) + L" 份源素材");
}
static void selectSource(State& state, size_t index) {
    if (state.busy || index >= state.sources.size() || index == state.sourceIndex) return;
    auto animation = decode(state.sources[index].path);
    state.sourceIndex = index; state.source = state.sources[index].path;
    state.sourceAnimation = std::move(animation); state.previewImage.reset(); state.showingResult = false;
    state.set(state.sourcePath, state.source.filename().wstring());
    state.refreshViews(); updateSourceControls(state);
}
static void commitSourcePosition(State& state) {
    auto value = state.text(GetDlgItem(state.window, SourcePosition));
    try {
        if (value.empty() || value.size() > 3 ||
            !std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            throw Error("请输入有效序号");
        auto position = std::stoul(value);
        if (!position || position > state.sources.size()) throw Error("超出素材数量");
        selectSource(state, position - 1);
    } catch (const std::exception& error) { state.set(state.status, L"素材序号无效：" + wide(error.what())); }
    updateSourceControls(state);
}
static LRESULT CALLBACK sourcePositionProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR owner) {
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        SendMessageW(reinterpret_cast<HWND>(owner), WM_COMMAND, MAKEWPARAM(SourcePosition, EN_KILLFOCUS), 0);
        return 0;
    }
    if (message == WM_CHAR && wparam == VK_RETURN) return 0;
    if (message == WM_GETDLGCODE && lparam &&
        reinterpret_cast<MSG*>(lparam)->wParam == VK_RETURN) return DLGC_WANTMESSAGE;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, sourcePositionProcedure, 2);
    return DefSubclassProc(window, message, wparam, lparam);
}
static void loadCharacter(State& state, const fs::path& path) {
    auto animation = decode(path);
    if (animation.frames.size() > 1) animation.frames.resize(1);
    state.character = path; state.characterAnimation = std::move(animation); state.crop = {};
    state.set(state.characterPath, path.filename().wstring());
    state.set(state.cropInfo, L"完整参考图");
    state.views[1].zoom = 1; state.views[1].panX = state.views[1].panY = 0;
    InvalidateRect(state.characterView, nullptr, FALSE);
}
static void dropFiles(State& state, HDROP drop, int kind) {
    struct Finish { HDROP drop; ~Finish() { DragFinish(drop); } } finish{drop};
    try {
        if (state.busy) throw Error("任务进行中，请在结束后拖入图片");
        if (kind == -1) {
            POINT point{}; DragQueryPoint(drop, &point);
            if (point.x < 16 || point.x > 258) return;
            kind = point.y >= 66 && point.y <= 260 ? 0 :
                point.y >= 274 && point.y <= 500 ? 1 : -1;
        }
        if (kind != 0 && kind != 1) return;
        auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if ((!state.batchEnabled() || kind == 1) && count != 1)
            throw Error("每个选图区域请一次拖入一个文件");
        if (count > 500) throw Error("单次最多拖入 500 项");
        std::vector<fs::path> paths;
        for (UINT i = 0; i < count; ++i) {
            UINT size = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(size + 1, L'\0');
            if (!DragQueryFileW(drop, i, path.data(), size + 1)) throw Error("无法读取拖入的文件");
            path.resize(size); paths.emplace_back(path);
        }
        if (kind == 0 && state.batchEnabled()) importSources(state, paths);
        else if (kind == 0) loadSource(state, paths.at(0));
        else loadCharacter(state, paths.at(0));
    } catch (const std::exception& error) {
        state.set(state.status, L"拖入失败"); state.logLine(wide(error.what()));
    }
}
static void start(State& state, const Job* refinement = nullptr);
static void updateRefineEntry(State& state) {
    EnableWindow(GetDlgItem(state.window, RefineEntry), !state.busy &&
        std::find(state.enabledPlugins.begin(), state.enabledPlugins.end(), "memestudio.local-redraw") != state.enabledPlugins.end());
}
static void applyPlugins(State& state, const std::vector<std::string>& enabled) {
    state.plugins.reset();
    state.enabledPlugins.clear();
    updateRefineEntry(state);
    auto logger = [window = state.window](const std::wstring& message) {
        auto event = std::make_unique<Progress>();
        event->message = L"插件：" + message;
        if (IsWindow(window) && PostMessageW(window, WM_PROGRESS, 0, reinterpret_cast<LPARAM>(event.get())))
            event.release();
    };
    state.plugins = std::make_shared<Plugins>(state.availablePlugins, enabled, logger);
    state.enabledPlugins = enabled;
    updateRefineEntry(state);
    if (!state.batchEnabled() && state.sources.size() > 1) {
        auto current = state.source; loadSource(state, current);
    }
    updateSourceControls(state);
}
enum PluginId { PluginList = 2000, PluginCommands, PluginRun, PluginRescan, PluginFolder, PluginApply };
struct PluginDialog {
    State* state;
    HWND list{}, commands{};
    bool capture = false;
    std::string failure;
    std::optional<Job> refinement;
};
static void fillPluginCommands(PluginDialog& dialog) {
    SendMessageW(dialog.commands, CB_RESETCONTENT, 0, 0);
    int selected = ListView_GetNextItem(dialog.list, -1, LVNI_SELECTED);
    if (selected >= 0 && size_t(selected) < dialog.state->availablePlugins.size())
        for (const auto& command : dialog.state->availablePlugins[selected].commands)
        SendMessageW(dialog.commands, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(command.title.c_str()));
    SendMessageW(dialog.commands, CB_SETCURSEL, 0, 0);
}
static std::vector<std::string> selectedPlugins(PluginDialog& dialog) {
    std::vector<std::string> enabled;
    for (int i = 0; i < int(dialog.state->availablePlugins.size()); ++i)
        if (ListView_GetCheckState(dialog.list, i)) {
            const auto& info = dialog.state->availablePlugins[i];
            if (!info.error.empty()) throw Error(info.error);
            enabled.push_back(info.id);
        }
    return enabled;
}
static void fillPluginList(PluginDialog& dialog, const std::vector<std::string>& enabled) {
    ListView_DeleteAllItems(dialog.list);
    for (int i = 0; i < int(dialog.state->availablePlugins.size()); ++i) {
        const auto& info = dialog.state->availablePlugins[i];
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = const_cast<LPWSTR>(info.name.c_str());
        ListView_InsertItem(dialog.list, &item);
        auto version = wide(info.version);
        auto description = info.error.empty() ? info.description : L"不可用：" + wide(info.error);
        ListView_SetItemText(dialog.list, i, 1, version.data());
        ListView_SetItemText(dialog.list, i, 2, description.data());
        ListView_SetCheckState(dialog.list, i,
            std::find(enabled.begin(), enabled.end(), info.id) != enabled.end());
    }
    fillPluginCommands(dialog);
}
static INT_PTR CALLBACK pluginProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<PluginDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<PluginDialog*>(lparam);
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            auto& state = *dialog->state;
            SetWindowTextW(window, L"插件管理");
            SetWindowPos(window, nullptr, 0, 0, 748, 432, SWP_NOMOVE | SWP_NOZORDER);
            RECT owner{}, rect{}; GetWindowRect(state.window, &owner); GetWindowRect(window, &rect);
            SetWindowPos(window, nullptr, owner.left + (owner.right - owner.left - rect.right + rect.left) / 2,
                owner.top + (owner.bottom - owner.top - rect.bottom + rect.top) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id,
                int x, int y, int width, int height) {
                return control(state, window, type, label, style, id, x, y, width, height);
            };
            dialog->list = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                LVS_SHOWSELALWAYS, PluginList, 16, 16, 700, 254);
            ListView_SetExtendedListViewStyle(dialog->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* titles[]{L"插件", L"版本", L"说明 / 状态"};
            int widths[]{200, 85, 385};
            for (int i = 0; i < 3; ++i) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<LPWSTR>(titles[i]); column.cx = widths[i];
                ListView_InsertColumn(dialog->list, i, &column);
            }
            dialog->commands = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                PluginCommands, 16, 284, 526, 180);
            add(L"BUTTON", L"执行命令", WS_TABSTOP | BS_PUSHBUTTON, PluginRun, 554, 284, 162, 30);
            add(L"BUTTON", L"打开目录", WS_TABSTOP | BS_PUSHBUTTON, PluginFolder, 16, 342, 100, 32);
            add(L"BUTTON", L"重新扫描", WS_TABSTOP | BS_PUSHBUTTON, PluginRescan, 128, 342, 100, 32);
            add(L"BUTTON", L"应用", WS_TABSTOP | BS_PUSHBUTTON, PluginApply, 504, 342, 100, 32);
            add(L"BUTTON", L"关闭", WS_TABSTOP | BS_DEFPUSHBUTTON, IDCANCEL, 616, 342, 100, 32);
            fillPluginList(*dialog, state.enabledPlugins);
            if (dialog->capture) PostMessageW(window, WM_APP + 7, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
        auto& state = *dialog->state;
        if (message == WM_APP + 7 && dialog->capture) {
            if (ListView_GetItemCount(dialog->list) != int(state.availablePlugins.size()))
                throw Error("Plugin manager list failed");
            auto found = std::find_if(state.availablePlugins.begin(), state.availablePlugins.end(),
                [](const auto& info) { return info.id == "memestudio.example"; });
            if (found == state.availablePlugins.end()) throw Error("Example plugin missing from manager");
            int index = int(found - state.availablePlugins.begin());
            ListView_SetItemState(dialog->list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetCheckState(dialog->list, index, TRUE);
            SendMessageW(window, WM_COMMAND, PluginRun, 0);
            if (!dialog->failure.empty()) return TRUE;
            if (state.text(state.instruction).find(L"Maintain consistent character identity") == std::wstring::npos)
                throw Error("Plugin manager command action failed");
            EndDialog(window, IDOK); return TRUE;
        }
        if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == PluginList) {
            if (reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) fillPluginCommands(*dialog);
            return FALSE;
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
        case PluginFolder: {
            auto directory = pluginDirectory(); fs::create_directories(directory);
            ShellExecuteW(window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return TRUE;
        }
        case PluginRescan: {
            auto enabled = selectedPlugins(*dialog);
            state.availablePlugins = discoverPlugins(pluginDirectory());
            fillPluginList(*dialog, enabled); return TRUE;
        }
        case PluginApply:
            applyPlugins(state, selectedPlugins(*dialog)); saveSettings(state);
            state.logLine(L"插件设置已应用"); return TRUE;
        case PluginRun: {
            int selected = ListView_GetNextItem(dialog->list, -1, LVNI_SELECTED);
            int commandIndex = int(SendMessageW(dialog->commands, CB_GETCURSEL, 0, 0));
            if (selected < 0 || commandIndex < 0) throw Error("请选择插件和命令");
            auto info = state.availablePlugins[selected];
            applyPlugins(state, selectedPlugins(*dialog)); saveSettings(state);
            Json context{{"source", utf8(state.source.wstring())}, {"character", utf8(state.character.wstring())},
                {"result", utf8(state.resultPath.wstring())},
                {"output", utf8(state.text(state.output))}, {"instruction", utf8(state.text(state.instruction))}};
            auto result = state.plugins->command(info.id, info.commands.at(commandIndex).id, context, window);
            if (result.value("action", "") == "refine") {
                Job job; job.source = state.resultPath;
                if (editRefinement(state.instance, window, job, false)) {
                    dialog->refinement = std::move(job);
                    EndDialog(window, IDOK);
                }
                return TRUE;
            }
            if (result.value("action", "") == "configure_upscale") {
                if (editUpscale(state.instance, window, state.upscale)) {
                    saveSettings(state); state.logLine(L"Waifu2x 输入/输出超分设置已保存");
                }
                return TRUE;
            }
            if (result.value("action", "") == "batch_import" || result.value("action", "") == "batch_folder") {
                if (!state.batchEnabled()) throw Error("请先启用批量转换插件");
                if (result["action"] == "batch_import") importSources(state, chooseImages(state.window, true));
                else {
                    auto folder = chooseFolder(state, L"选择源表情文件夹");
                    if (!folder.empty()) importSources(state, {folder});
                }
                return TRUE;
            }
            if (result.contains("instruction"))
                state.set(state.instruction, wide(result.at("instruction").get<std::string>()));
            if (result.contains("append_instruction")) {
                auto current = state.text(state.instruction);
                state.set(state.instruction, current + (current.empty() ? L"" : L"\r\n") +
                    wide(result.at("append_instruction").get<std::string>()));
            }
            state.logLine(L"已执行插件命令：" + info.commands.at(commandIndex).title); return TRUE;
        }
        }
    } catch (const std::exception& error) {
        if (dialog && dialog->capture) {
            dialog->failure = error.what(); EndDialog(window, IDABORT); return TRUE;
        }
        MessageBoxW(window, wide(error.what()).c_str(), L"插件", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
static void managePlugins(State& state, bool capture = false) {
    if (state.busy) return;
    state.availablePlugins = discoverPlugins(pluginDirectory());
    PluginDialog dialog{&state};
    dialog.capture = capture;
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    resource.dialog.cx = 460; resource.dialog.cy = 260;
    auto result = DialogBoxIndirectParamW(state.instance, &resource.dialog, state.window, pluginProcedure,
        reinterpret_cast<LPARAM>(&dialog));
    if (result == -1) throw Error("无法打开插件管理窗口");
    if (!dialog.failure.empty()) throw Error(dialog.failure);
    if (dialog.refinement) start(state, &*dialog.refinement);
}
static void selectResult(State& state, size_t index) {
    auto path = state.resultPaths.at(index);
    auto animation = decode(path);
    state.previewImage.reset(); state.resultAnimation = std::move(animation);
    state.resultPath = path; state.showingResult = true; state.refreshViews();
}
static void fetchModels(State& state) {
    if (state.busy) return;
    Provider provider;
    provider.baseUrl = state.text(state.get(BaseUrl));
    provider.key = state.text(state.get(Key));
    provider.timeoutSeconds = state.number(Timeout, 5, 3600);
    if (provider.key.empty()) throw Error("请填写供应商 API Key");
    modelsUrl(provider.baseUrl);
    if (state.worker.joinable()) state.worker.join();
    state.cancel.reset(); state.busy = true; state.fetchingModels = true;
    for (auto item : state.lockControls) EnableWindow(item, FALSE);
    EnableWindow(GetDlgItem(state.window, CancelJob), TRUE);
    state.set(state.get(FetchModels), L"获取中…");
    state.set(state.status, L"正在获取供应商模型…");
    state.worker = std::thread([&state, provider] {
        auto completion = std::make_unique<Completion>();
        completion->modelFetch = true;
        try {
            completion->models = listModels(provider, state.cancel);
            completion->success = true;
        } catch (const std::exception& error) { completion->message = wide(error.what()); }
        if (PostMessageW(state.window, WM_FINISHED, 0, reinterpret_cast<LPARAM>(completion.get())))
            completion.release();
    });
}
static void start(State& state, const Job* refinement) {
    if (state.busy) return;
    if (!refinement && (state.source.empty() || state.character.empty()))
        throw Error("请先导入表情素材和人物参考图");
    Job job;
    job.source = state.source; job.character = state.character; job.styles = state.stylePaths;
    job.crop = state.crop; job.instruction = state.text(state.instruction);
    job.output = state.text(state.output);
    job.stride = state.number(Stride, 1, 600); job.maxFrames = state.number(MaxFrames, 0, 600);
    job.consistency = state.checked(Consistency); job.preserveText = state.checked(PreserveText);
    job.resume = state.checked(Resume);
    job.generationCount = state.number(GenerationCount, 1, 10);
    job.plugins = state.plugins;
    if (std::find(state.enabledPlugins.begin(), state.enabledPlugins.end(), "memestudio.waifu2x") != state.enabledPlugins.end())
        job.upscale = state.upscale;
    if (refinement) {
        job.refine = true; job.region = refinement->region;
        job.source = refinement->source; job.character.clear(); job.crop = {};
        job.styles = refinement->styles; job.instruction = refinement->instruction;
        job.output = refinement->output; job.stride = 1; job.maxFrames = 0;
    }
    job.provider.baseUrl = state.text(state.get(BaseUrl)); job.provider.key = state.text(state.get(Key));
    job.provider.model = utf8(state.text(state.get(Model))); job.provider.quality = utf8(state.text(state.get(Quality)));
    auto modelBegin = job.provider.model.find_first_not_of(" \t\r\n");
    auto modelEnd = job.provider.model.find_last_not_of(" \t\r\n");
    if (modelBegin == std::string::npos) throw Error("请先选择或填写模型名称");
    job.provider.model = job.provider.model.substr(modelBegin, modelEnd - modelBegin + 1);
    job.provider.size = utf8(state.text(state.get(Size))); job.provider.imageField = utf8(state.text(state.get(ImageField)));
    job.provider.complexity = utf8(state.text(state.get(Complexity)));
    job.provider.outputFormat = utf8(state.text(state.get(OutputFormat)));
    if (job.provider.complexity == utf8(L"不发送")) job.provider.complexity.clear();
    if (job.provider.outputFormat == utf8(L"不发送")) job.provider.outputFormat.clear();
    if ((job.provider.outputFormat == "jpeg" || job.provider.outputFormat == "webp") &&
        state.text(state.get(Compression)) != L"不发送")
        job.provider.compression = state.number(Compression, 0, 100);
    job.provider.stream = state.checked(Streaming);
    if (job.provider.stream) job.provider.partialImages = state.number(PartialImages, 1, 3);
    if (job.provider.quality == utf8(L"不发送")) job.provider.quality.clear();
    if (job.provider.size == utf8(L"不发送")) job.provider.size.clear();
    job.provider.timeoutSeconds = state.number(Timeout, 5, 3600);
    try { job.provider.extra = Json::parse(utf8(state.text(state.get(Extra)))); }
    catch (...) { throw Error("扩展参数不是有效 JSON"); }
    if (!job.provider.extra.is_object()) throw Error("扩展参数必须是 JSON 对象");
    if (job.provider.key.empty()) throw Error("请填写供应商 API Key");
    // Validate request shape before starting work or asking to replace an output.
    multipart(job.provider, "", {{"validate", Bytes{1}}});
    editUrl(job.provider.baseUrl);
    bool batch = !refinement && state.batchEnabled() && state.sources.size() > 1;
    if (job.output.empty()) {
        auto path = batch ? chooseFolder(state, L"选择批量转换输出文件夹") : chooseOutput(state);
        if (path.empty()) return;
        job.output = path; state.set(state.output, path.wstring());
    }
    std::vector<Job> jobs;
    auto sources = state.sources;
    std::vector<fs::path> outputs;
    if (batch) {
        jobs = sourceJobs(sources, job, job.output);
        for (const auto& item : jobs) {
            auto paths = outputPaths(item);
            outputs.insert(outputs.end(), paths.begin(), paths.end());
        }
    } else outputs = outputPaths(job);
    std::wstring existing;
    int existingCount = 0;
    for (const auto& path : outputs) if (fs::exists(path)) {
        if (++existingCount <= 8) existing += L"\n" + path.wstring();
    }
    if (existingCount > 8) existing += L"\n…共 " + std::to_wstring(existingCount) + L" 个文件";
    if (!existing.empty() && MessageBoxW(state.window,
        (L"以下输出文件已存在，生成完成后将替换：" + existing).c_str(),
        L"替换输出文件", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    state.cancel.reset(); state.busy = true;
    for (auto control : state.lockControls) EnableWindow(control, FALSE);
    EnableWindow(GetDlgItem(state.window, CancelJob), TRUE);
    state.set(state.status, L"正在准备任务…");
    state.previewImage.reset(); state.showingResult = true;
    state.logLine((job.refine ? L"局部重绘开始，模型：" : L"任务开始，模型：") + wide(job.provider.model));
    SendMessageW(state.progress, PBM_SETPOS, 0, 0);
    if (state.worker.joinable()) state.worker.join();
    state.worker = std::thread([&state, job, batch, jobs, sources] {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto completion = std::make_unique<Completion>();
        try {
            check(initialized, "Worker COM");
            auto reporter = [&](const Progress& progress) {
                auto event = std::make_unique<Progress>(progress);
                if (PostMessageW(state.window, WM_PROGRESS, 0, reinterpret_cast<LPARAM>(event.get()))) event.release();
            };
            if (batch) runSourceJobs(sources, jobs, state.cancel, reporter, completion->outputs);
            else { runJob(job, state.cancel, reporter); completion->outputs = outputPaths(job); }
            completion->success = true;
            completion->message = L"生成完成，共 " + std::to_wstring(completion->outputs.size()) + L" 份";
        } catch (const std::exception& error) {
            completion->success = false; completion->message = wide(error.what());
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (PostMessageW(state.window, WM_FINISHED, 0, reinterpret_cast<LPARAM>(completion.get()))) completion.release();
    });
}
static void layout(State& state) {
    RECT rect; GetClientRect(state.window, &rect);
    int width = rect.right, height = rect.bottom, left = 258, right = 332;
    MoveWindow(state.settings, width - right - 14, 60, right, height - 78, TRUE);
    int middleX = left + 30, middleWidth = width - middleX - right - 38;
    MoveWindow(GetDlgItem(state.window, SourceTab), middleX, 68, 82, 30, TRUE);
    MoveWindow(GetDlgItem(state.window, ResultTab), middleX + 90, 68, 82, 30, TRUE);
    MoveWindow(state.resultChoice, middleX + 180, 68, std::max(64, middleWidth - 336), 200, TRUE);
    MoveWindow(GetDlgItem(state.window, OpenResult), middleX + middleWidth - 146, 68, 70, 30, TRUE);
    MoveWindow(GetDlgItem(state.window, OpenFolder), middleX + middleWidth - 70, 68, 70, 30, TRUE);
    int canvasBottom = height - 302;
    MoveWindow(state.mainView, middleX, 110, middleWidth, std::max(120, canvasBottom - 110), TRUE);
    MoveWindow(GetDlgItem(state.window, Play), middleX, canvasBottom + 10, 64, 28, TRUE);
    MoveWindow(state.timeline, middleX + 72, canvasBottom + 10, middleWidth - 156, 28, TRUE);
    MoveWindow(state.frameInfo, middleX + middleWidth - 76, canvasBottom + 12, 76, 26, TRUE);
    MoveWindow(state.estimate, middleX, canvasBottom + 48, middleWidth, 48, TRUE);
    MoveWindow(state.output, middleX, height - 202, middleWidth - 42, 28, TRUE);
    MoveWindow(GetDlgItem(state.window, BrowseOutput), middleX + middleWidth - 36, height - 202, 36, 28, TRUE);
    MoveWindow(state.log, middleX, height - 162, middleWidth, 70, TRUE);
    MoveWindow(state.progress, middleX, height - 80, middleWidth, 8, TRUE);
    MoveWindow(state.status, middleX, height - 62, middleWidth - 200, 42, TRUE);
    MoveWindow(GetDlgItem(state.window, Generate), middleX + middleWidth - 188, height - 60, 112, 34, TRUE);
    MoveWindow(GetDlgItem(state.window, CancelJob), middleX + middleWidth - 68, height - 60, 68, 34, TRUE);
    MoveWindow(state.instruction, 16, 686, 242, std::max(54, height - 706), TRUE);
    InvalidateRect(state.window, nullptr, TRUE);
}
static void initialize(State& state) {
    state.font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    state.titleFont = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    state.smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto label = [&](const wchar_t* value, int x, int y, int width, int height = 22) {
        return control(state, state.window, L"STATIC", value, SS_LEFT, 0, x, y, width, height);
    };
    auto button = [&](int id, const wchar_t* value, int x, int y, int width, bool locked = true) {
        return control(state, state.window, L"BUTTON", value, WS_TABSTOP | BS_PUSHBUTTON,
            id, x, y, width, 30, locked);
    };
    auto title = label(L"表情重绘", 16, 13, 160, 34);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(state.titleFont), TRUE);
    auto brand = label(L"MEME STUDIO", 184, 23, 160);
    SendMessageW(brand, WM_SETFONT, reinterpret_cast<WPARAM>(state.smallFont), TRUE);
    label(L"原始表情", 16, 72, 96); button(Source, L"导入…", 176, 66, 82);
    state.sourcePath = control(state, state.window, L"STATIC", L"未选择文件", SS_PATHELLIPSIS, 0, 16, 103, 242, 22);
    label(L"参考人物", 16, 280, 120); button(Character, L"导入…", 176, 274, 82);
    state.characterPath = control(state, state.window, L"STATIC", L"未选择文件", SS_PATHELLIPSIS, 0, 16, 310, 242, 22);
    for (int i = 0; i < 3; ++i) { state.views[i].state = &state; state.views[i].kind = i; }
    state.sourceView = CreateWindowExW(0, L"MemeStudioView", L"原始表情预览", WS_CHILD | WS_VISIBLE,
        16, 134, 242, 126, state.window, nullptr, state.instance, &state.views[0]);
    button(SourceFolder, L"目录", 116, 66, 54);
    button(SourcePrevious, L"<", 16, 230, 36);
    auto position = control(state, state.window, L"EDIT", L"0", WS_TABSTOP | ES_NUMBER | ES_CENTER,
        SourcePosition, 60, 230, 68, 28, true);
    SendMessageW(position, EM_SETLIMITTEXT, 3, 0);
    SetWindowSubclass(position, sourcePositionProcedure, 2, reinterpret_cast<DWORD_PTR>(state.window));
    control(state, state.window, L"STATIC", L"/ 0", SS_LEFT, SourceTotal, 136, 234, 76, 24);
    button(SourceNext, L">", 222, 230, 36);
    state.characterView = CreateWindowExW(0, L"MemeStudioView", L"人物参考预览与裁剪", WS_CHILD | WS_VISIBLE,
        16, 342, 242, 158, state.window, nullptr, state.instance, &state.views[1]);
    state.mainView = CreateWindowExW(0, L"MemeStudioView", L"工作区预览", WS_CHILD | WS_VISIBLE,
        288, 110, 500, 420, state.window, nullptr, state.instance, &state.views[2]);
    DragAcceptFiles(state.window, TRUE);
    DragAcceptFiles(state.sourceView, TRUE); DragAcceptFiles(state.characterView, TRUE);
    state.cropInfo = label(L"完整参考图", 16, 510, 155);
    button(ClearCrop, L"重置裁剪", 176, 504, 82);
    label(L"画风参考", 16, 550, 100);
    button(AddStyle, L"添加", 130, 544, 60); button(RemoveStyle, L"移除", 198, 544, 60);
    state.styles = control(state, state.window, L"LISTBOX", L"",
        WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER,
        StyleList, 16, 582, 242, 65, true);
    label(L"附加要求", 16, 658, 150); button(InstructionPresets, L"预设…", 176, 652, 82);
    state.instruction = control(state, state.window, L"EDIT", L"",
        WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
        Instruction, 16, 686, 242, 100, true);
    button(SourceTab, L"原图", 0, 0, 82, false); button(ResultTab, L"结果", 0, 0, 82, false);
    button(RefineEntry, L"局部重绘…", 380, 15, 120);
    state.resultChoice = control(state, state.window, L"COMBOBOX", L"",
        WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ResultChoice, 0, 0, 100, 200, true);
    ShowWindow(state.resultChoice, SW_HIDE);
    button(OpenResult, L"打开", 0, 0, 70, false); button(OpenFolder, L"目录", 0, 0, 70, false);
    button(Play, L"暂停", 0, 0, 64, false);
    state.timeline = control(state, state.window, TRACKBAR_CLASSW, L"",
        WS_TABSTOP | TBS_NOTICKS | TBS_HORZ, Timeline, 0, 0, 100, 28);
    state.frameInfo = label(L"0 / 0", 0, 0, 76);
    state.estimate = label(L"尚未导入表情素材", 0, 0, 500, 48);
    state.output = control(state, state.window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
        Output, 0, 0, 400, 28, true);
    SendMessageW(state.output, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"输出文件路径"));
    button(BrowseOutput, L"…", 0, 0, 36);
    state.log = control(state, state.window, L"EDIT", L"",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0, 500, 70);
    state.progress = control(state, state.window, PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, 0, 0, 500, 8);
    SendMessageW(state.progress, PBM_SETRANGE32, 0, 100);
    state.status = label(L"就绪", 0, 0, 400, 42);
    button(Generate, L"开始生成", 0, 0, 112); button(CancelJob, L"取消", 0, 0, 68, false);
    EnableWindow(GetDlgItem(state.window, CancelJob), FALSE);
    state.settings = CreateWindowExW(WS_EX_CONTROLPARENT, L"MemeStudioSettings", L"供应商和生成参数",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL, 850, 60, 332, 760, state.window, nullptr, state.instance, &state);
    createSettings(state); loadSettings(state); updateProviderControls(state);
    try {
        state.availablePlugins = discoverPlugins(pluginDirectory());
        auto enabled = state.enabledPlugins; applyPlugins(state, enabled);
    } catch (const std::exception& error) { state.logLine(L"插件未启用：" + wide(error.what())); }
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, state.window, nullptr, state.instance, nullptr);
    auto tip = [&](HWND target, const wchar_t* text) {
        TOOLINFOW info{sizeof(info)}; info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = state.window; info.uId = reinterpret_cast<UINT_PTR>(target);
        info.lpszText = const_cast<LPWSTR>(text);
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    };
    tip(state.characterView, L"拖动框选人物区域");
    tip(GetDlgItem(state.window, BrowseOutput), L"选择输出位置；多源批量转换时选择文件夹");
    tip(GetDlgItem(state.window, SourcePrevious), L"上一份源素材");
    tip(GetDlgItem(state.window, SourceNext), L"下一份源素材");
    tip(position, L"输入素材序号，按 Enter 或移开焦点跳转");
    tip(GetDlgItem(state.window, SourceFolder), L"导入文件夹当前层级的图片和 GIF");
    tip(state.get(Stride), L"每隔多少源帧生成一帧；合并对应显示时长");
    tip(state.get(MaxFrames), L"自动抽帧，覆盖完整动画；0 表示不设上限");
    tip(state.get(ImageField), L"多张图片重复使用同一个上传字段");
    tip(state.mainView, L"双指滚动平移；捏合或 Ctrl+滚动缩放；双击恢复适应窗口");
    tip(state.get(Complexity), L"独立于 Quality；由供应商和模型决定是否支持");
    tip(state.get(Compression), L"JPEG/WebP 压缩质量；100 保留最多细节");
    if (state.text(state.get(Key)).empty() && !state.demo) {
        wchar_t key[8192]{};
        DWORD count = GetEnvironmentVariableW(L"OPENAI_API_KEY", key, 8192);
        if (count && count < 8192) state.set(state.get(Key), key);
        SecureZeroMemory(key, sizeof(key));
    }
    SetTimer(state.window, 1, 20, nullptr);
}
static void command(State& state, int id, int notification) {
    if (id == SourcePosition) {
        if (!state.changingSourcePosition && notification == EN_KILLFOCUS && !state.busy)
            commitSourcePosition(state);
        return;
    }
    if (id == SourcePrevious || id == SourceNext || id == SourceFolder) {
        if (state.busy || !state.batchEnabled()) return;
        if (id == SourcePrevious && state.sourceIndex) selectSource(state, state.sourceIndex - 1);
        if (id == SourceNext) selectSource(state, state.sourceIndex + 1);
        if (id == SourceFolder) {
            auto folder = chooseFolder(state, L"选择源表情文件夹");
            if (!folder.empty()) importSources(state, {folder});
        }
        return;
    }
    if (id == InstructionPresets) { choosePreset(state); return; }
    if ((id == Stride || id == MaxFrames || id == GenerationCount) && notification == EN_CHANGE) { state.updateEstimate(); return; }
    if (id == PluginManage) { managePlugins(state); return; }
    if (id == RefineEntry && !state.busy) {
        if (!state.plugins) throw Error("请先启用局部重绘插件");
        auto result = state.plugins->command("memestudio.local-redraw", "refine-result",
            {{"result", utf8(state.resultPath.wstring())}}, state.window);
        if (result.value("action", "") == "refine") {
            Job job; job.source = state.resultPath;
            if (editRefinement(state.instance, state.window, job)) start(state, &job);
        }
        return;
    }
    if (id == ResultChoice && notification == CBN_SELCHANGE) {
        auto selected = SendMessageW(state.resultChoice, CB_GETCURSEL, 0, 0);
        if (selected != CB_ERR && !state.busy) selectResult(state, size_t(selected));
        return;
    }
    if ((id == OutputFormat && notification == CBN_SELCHANGE) ||
        (id == Streaming && notification == BN_CLICKED)) { updateProviderControls(state); return; }
    if (id == Source || id == Character) {
        auto paths = chooseImages(state.window, id == Source && state.batchEnabled()); if (paths.empty()) return;
        if (id == Source && state.batchEnabled()) importSources(state, paths);
        else if (id == Source) loadSource(state, paths[0]); else loadCharacter(state, paths[0]);
    } else if (id == ClearCrop) {
        state.crop = {}; state.set(state.cropInfo, L"完整参考图"); InvalidateRect(state.characterView, nullptr, FALSE);
    } else if (id == AddStyle) {
        auto paths = chooseImages(state.window, true);
        if (state.stylePaths.size() + paths.size() > 8) throw Error("最多支持 8 张画风参考图");
        for (const auto& path : paths) {
            decode(path);
            state.stylePaths.push_back(path);
            SendMessageW(state.styles, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.filename().c_str()));
        }
    } else if (id == RemoveStyle) {
        auto index = SendMessageW(state.styles, LB_GETCURSEL, 0, 0);
        if (index != LB_ERR) {
            state.stylePaths.erase(state.stylePaths.begin() + index);
            SendMessageW(state.styles, LB_DELETESTRING, index, 0);
        }
    } else if (id == BrowseOutput) {
        auto path = state.batchEnabled() && state.sources.size() > 1 ?
            chooseFolder(state, L"选择批量转换输出文件夹") : chooseOutput(state);
        if (!path.empty()) state.set(state.output, path.wstring());
    } else if (id == FetchModels) fetchModels(state);
    else if (id == Generate) start(state);
    else if (id == CancelJob) {
        state.cancel.stop(); state.set(state.status, state.fetchingModels ?
            L"正在取消模型获取…" : L"正在取消，已完成帧将保留…");
        EnableWindow(GetDlgItem(state.window, CancelJob), FALSE);
    } else if (id == SourceTab || id == ResultTab) {
        state.showingResult = id == ResultTab; state.refreshViews();
    } else if (id == Play) {
        state.playing = !state.playing; SetWindowTextW(GetDlgItem(state.window, Play), state.playing ? L"暂停" : L"播放");
    } else if (id == OpenResult && !state.resultPath.empty()) {
        ShellExecuteW(state.window, L"open", state.resultPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == OpenFolder) {
        fs::path path = state.resultPath.empty() ? fs::path(state.text(state.output)).parent_path() : state.resultPath.parent_path();
        if (!path.empty() && fs::exists(path)) ShellExecuteW(state.window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_CREATE: initialize(*state); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {1120, 830}; return 0;
        }
        case WM_SIZE: if (state->settings) layout(*state); return 0;
        case WM_COMMAND: command(*state, LOWORD(wparam), HIWORD(wparam)); return 0;
        case WM_DROPFILES: dropFiles(*state, reinterpret_cast<HDROP>(wparam), -1); return 0;
        case WM_HSCROLL:
            state->frame = static_cast<int>(SendMessageW(state->timeline, TBM_GETPOS, 0, 0));
            state->elapsed = 0; state->updateFrameLabel();
            InvalidateRect(state->mainView, nullptr, FALSE); InvalidateRect(state->sourceView, nullptr, FALSE); return 0;
        case WM_TIMER: {
            auto& frames = state->active().frames;
            if (!state->playing || frames.size() < 2) return 0;
            state->elapsed += 20;
            if (state->elapsed >= static_cast<int>(std::max(20u, frames[state->frame].delayMs))) {
                state->elapsed = 0; state->frame = (state->frame + 1) % static_cast<int>(frames.size());
                SendMessageW(state->timeline, TBM_SETPOS, TRUE, state->frame);
                if (!state->showingResult || !state->previewImage) state->updateFrameLabel();
                InvalidateRect(state->mainView, nullptr, FALSE); InvalidateRect(state->sourceView, nullptr, FALSE);
            }
            return 0;
        }
        case WM_PROGRESS: {
            std::unique_ptr<Progress> progress(reinterpret_cast<Progress*>(lparam));
            state->set(state->status, progress->message);
            if (progress->total) SendMessageW(state->progress, PBM_SETPOS, progress->done * 100 / progress->total, 0);
            if (progress->frame >= 0) {
                state->previewImage = progress->preview;
                if (state->showingResult) {
                    InvalidateRect(state->mainView, nullptr, FALSE);
                    state->set(state->frameInfo, progress->partial >= 0 ?
                        L"局部 " + std::to_wstring(progress->partial + 1) :
                        std::to_wstring(progress->frame + 1) + L" / " +
                        std::to_wstring(progress->frameTotal ? progress->frameTotal :
                            progress->total / std::max(1, progress->copies)));
                }
            }
            state->logLine(progress->message); return 0;
        }
        case WM_FINISHED: {
            std::unique_ptr<Completion> completion(reinterpret_cast<Completion*>(lparam));
            if (state->worker.joinable()) state->worker.join();
            state->busy = false;
            for (auto control : state->lockControls) EnableWindow(control, TRUE);
            updateProviderControls(*state);
            updateRefineEntry(*state);
            updateSourceControls(*state);
            EnableWindow(GetDlgItem(window, CancelJob), FALSE);
            if (completion->modelFetch) {
                state->fetchingModels = false;
                state->set(state->get(FetchModels), L"获取模型");
                if (completion->success) {
                    auto selection = state->text(state->get(Model));
                    std::stable_sort(completion->models.begin(), completion->models.end(),
                        [](const auto& a, const auto& b) {
                            return (a.find("gpt-image") != std::string::npos) >
                                (b.find("gpt-image") != std::string::npos);
                        });
                    SendMessageW(state->get(Model), CB_RESETCONTENT, 0, 0);
                    for (const auto& model : completion->models) {
                        auto name = wide(model);
                        SendMessageW(state->get(Model), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                    }
                    state->set(state->get(Model), selection.empty() ? wide(completion->models.front()) : selection);
                    auto summary = L"已获取 " + std::to_wstring(completion->models.size()) + L" 个模型";
                    state->set(state->status, summary); state->logLine(summary);
                } else {
                    state->set(state->status, state->cancel.stopped ? L"已取消模型获取" : L"获取模型失败");
                    state->logLine(completion->message);
                    if (!state->closing && !state->cancel.stopped)
                        MessageBoxW(window, completion->message.c_str(), L"获取模型失败", MB_OK | MB_ICONERROR);
                }
                if (state->closing) DestroyWindow(window);
                return 0;
            }
            state->set(state->status, completion->success ? L"生成完成" :
                state->cancel.stopped ? L"已取消，可继续生成" : L"任务失败，已保留完成帧");
            state->logLine(completion->message);
            if (!completion->outputs.empty()) {
                state->resultPaths = completion->outputs;
                SendMessageW(state->resultChoice, CB_RESETCONTENT, 0, 0);
                for (size_t i = 0; i < state->resultPaths.size(); ++i) {
                    auto label = completion->outputs[i].filename().wstring();
                    SendMessageW(state->resultChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                }
                SendMessageW(state->resultChoice, CB_SETCURSEL, 0, 0);
                ShowWindow(state->resultChoice, state->resultPaths.size() > 1 ? SW_SHOW : SW_HIDE);
                selectResult(*state, 0);
            }
            if (!completion->success && !state->closing && !state->cancel.stopped) {
                MessageBoxW(window, completion->message.c_str(), L"生成未完成", MB_OK | MB_ICONERROR);
            }
            if (state->closing) DestroyWindow(window);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, ink);
            bool editControl = message == WM_CTLCOLOREDIT || reinterpret_cast<HWND>(lparam) == state->log;
            SetBkColor(dc, editControl ? RGB(255, 255, 255) : paper);
            return reinterpret_cast<LRESULT>(editControl ? state->white : state->background);
        }
        case WM_ERASEBKGND: {
            RECT rect; GetClientRect(window, &rect); FillRect(reinterpret_cast<HDC>(wparam), &rect, state->background); return 1;
        }
        case WM_CLOSE:
            try { saveSettings(*state); } catch (const std::exception& error) { state->logLine(wide(error.what())); }
            if (state->busy) { state->closing = true; state->cancel.stop(); return 0; }
            DestroyWindow(window); return 0;
        case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& error) {
        MessageBoxW(window, wide(error.what()).c_str(), L"表情重绘", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int runGui(HINSTANCE instance, const fs::path& demo, bool capture) {
    State state; state.instance = instance; state.demo = !demo.empty();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
        ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{sizeof(type)}; type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hbrBackground = state.background; type.lpszClassName = L"MemeStudio"; type.lpfnWndProc = procedure;
    RegisterClassExW(&type);
    type.style = CS_DBLCLKS;
    type.hbrBackground = state.white; type.lpszClassName = L"MemeStudioView"; type.lpfnWndProc = viewProcedure;
    RegisterClassExW(&type);
    type.lpszClassName = L"MemeStudioSettings"; type.lpfnWndProc = settingsProcedure;
    RegisterClassExW(&type);
    HWND window = CreateWindowExW(0, L"MemeStudio", L"表情重绘 · Meme Studio", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 900, nullptr, nullptr, instance, &state);
    if (!window) throw Error("Cannot create application window");
    if (!demo.empty()) {
        try {
            loadSource(state, demo / L"source.gif"); loadCharacter(state, demo / L"character.png");
            state.stylePaths.push_back(demo / L"style.png");
            SendMessageW(state.styles, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"style.png"));
            state.set(state.instruction, L"保留夸张表情；人物服装沿用参考图。");
        } catch (const std::exception& error) { state.logLine(wide(error.what())); }
    }
    if (capture) {
        if (state.text(state.get(Quality)) != L"auto" || state.text(state.get(ImageField)) != L"image[]")
            throw Error("UI default selection is invalid");
        Json report{{"quality", utf8(state.text(state.get(Quality)))},
            {"image_field", utf8(state.text(state.get(ImageField)))},
            {"character_crop", "ready"}, {"snapshots", Json::array()}};
        auto snapshot = [&](int width, int height, const wchar_t* filename, HWND target = nullptr) {
            if (!target) {
                target = window;
                SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                layout(state);
            }
            Image image{static_cast<UINT>(width), static_cast<UINT>(height),
                Bytes(size_t(width) * height * 4)};
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            HDC dc = GetDC(target), memory = CreateCompatibleDC(dc);
            void* pixels = nullptr;
            HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!dib) throw Error("Cannot capture UI bitmap");
            auto old = SelectObject(memory, dib);
            SendMessageW(target, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND | PRF_NONCLIENT);
            memcpy(image.pixels.data(), pixels, image.pixels.size());
            for (size_t p = 3; p < image.pixels.size(); p += 4) image.pixels[p] = 255;
            SelectObject(memory, old); DeleteObject(dib); DeleteDC(memory); ReleaseDC(target, dc);
            savePng(demo / filename, image);
            report["snapshots"].push_back({{"width", width}, {"height", height}, {"file", utf8(filename)}});
        };
        snapshot(1280, 900, L"ui-wide.png");
        snapshot(1120, 830, L"ui-compact.png");
        auto originalInstruction = state.text(state.instruction);
        choosePreset(state, IDOK, [&](HWND dialog) {
            RECT bounds{}; GetWindowRect(dialog, &bounds);
            snapshot(bounds.right - bounds.left, bounds.bottom - bounds.top, L"ui-presets.png", dialog);
        });
        auto appended = state.text(state.instruction);
        if (appended.find(originalInstruction + L"\r\n") != 0 || appended.find(L"颜色漂移") == std::wstring::npos)
            throw Error("Preset append did not preserve existing requirements");
        choosePreset(state, IDCANCEL);
        if (state.text(state.instruction) != appended) throw Error("Preset cancel changed requirements");
        choosePreset(state, PresetReplace);
        if (state.text(state.instruction).find(originalInstruction) != std::wstring::npos ||
            state.text(state.instruction).find(L"颜色漂移") == std::wstring::npos)
            throw Error("Preset replace failed");
        state.set(state.instruction, L"");
        choosePreset(state, IDOK);
        if (state.text(state.instruction).find(L"若输入为 GIF") != 0)
            throw Error("Preset append to empty requirements failed");
        state.set(state.instruction, originalInstruction);
        report["instruction_presets"] = "passed";
        auto dropMany = [&](HWND target, const std::vector<fs::path>& paths) {
            std::wstring name;
            for (const auto& path : paths) { name += path.wstring(); name.push_back(L'\0'); }
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                sizeof(DROPFILES) + (name.size() + 1) * sizeof(wchar_t));
            if (!memory) throw Error("Cannot allocate drop fixture");
            auto* files = static_cast<DROPFILES*>(GlobalLock(memory));
            if (!files) { GlobalFree(memory); throw Error("Cannot lock drop fixture"); }
            files->pFiles = sizeof(DROPFILES); files->fWide = TRUE;
            memcpy(reinterpret_cast<BYTE*>(files) + sizeof(DROPFILES), name.c_str(),
                name.size() * sizeof(wchar_t));
            GlobalUnlock(memory);
            SendMessageW(target, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
        };
        auto drop = [&](HWND target, const fs::path& path) { dropMany(target, {path}); };
        drop(state.characterView, demo / L"中文路径-人物.png");
        if (state.character.filename() != L"中文路径-人物.png") throw Error("Unicode reference drop failed");
        auto character = state.character;
        drop(state.characterView, demo / L"missing.png");
        if (state.character != character) throw Error("Invalid drop replaced reference");
        state.busy = true;
        drop(state.characterView, demo / L"character.png");
        state.busy = false;
        if (state.character != character) throw Error("Busy drop replaced reference");
        drop(state.characterView, demo / L"character.png");
        drop(state.sourceView, demo / L"source.png");
        if (state.source.filename() != L"source.png") throw Error("Source drop failed");
        drop(state.sourceView, demo / L"source.gif");
        state.set(state.get(GenerationCount), L"3");
        if (state.number(GenerationCount, 1, 10) != 3 ||
            state.text(state.estimate).find(L"次生成请求") == std::wstring::npos)
            throw Error("Generation count control failed");
        state.set(state.get(GenerationCount), L"1");
        report["drop_and_count"] = "passed";
        if (!state.plugins || !state.plugins->fingerprint().empty())
            throw Error("Plugins must be disabled by default");
        auto savedInstruction = state.text(state.instruction);
        applyPlugins(state, {"memestudio.example"});
        if (state.plugins->fingerprint().size() != 1 ||
            state.plugins->transform(Json::object(), "base") == "base")
            throw Error("Plugin load or prompt hook failed");
        auto action = state.plugins->command("memestudio.example", "identity-note", Json::object(), window);
        if (!action.contains("append_instruction")) throw Error("Plugin command failed");
        managePlugins(state, true);
        applyPlugins(state, {});
        state.set(state.instruction, savedInstruction);
        report["plugin_hooks"] = "passed";
        applyPlugins(state, {"memestudio.local-redraw"});
        auto refineAction = state.plugins->command("memestudio.local-redraw", "refine-result",
            Json::object(), window);
        if (refineAction.value("action", "") != "refine") throw Error("Redraw plugin action failed");
        Job refinement; refinement.source = demo / L"source.gif";
        if (!editRefinement(instance, window, refinement, true) || !refinement.refine ||
            refinement.styles.size() != 1 || refinement.instruction.empty() ||
            refinement.region.width >= .6 || refinement.output == refinement.source)
            throw Error("Redraw dialog configuration failed");
        applyPlugins(state, {});
        report["refinement_plugin"] = "passed";
        applyPlugins(state, {"memestudio.waifu2x"});
        auto upscaleAction = state.plugins->command("memestudio.waifu2x", "configure-upscale",
            Json::object(), window);
        if (upscaleAction.value("action", "") != "configure_upscale") throw Error("Upscale plugin action failed");
        UpscaleOptions upscale;
        if (!editUpscale(instance, window, upscale, true) || upscale.input || upscale.output || upscale.scale != 4)
            throw Error("Upscale dialog configuration failed");
        applyPlugins(state, {});
        report["upscale_plugin"] = "passed";
        applyPlugins(state, {"memestudio.batch-convert"});
        auto batchAction = state.plugins->command("memestudio.batch-convert", "import-files", Json::object(), window);
        if (batchAction.value("action", "") != "batch_import") throw Error("Batch plugin command failed");
        dropMany(state.sourceView, {demo / L"source.png", demo / L"source.gif", demo / L"source.png"});
        if (state.sources.size() != 2 || state.sourceIndex != 0 ||
            !(GetWindowLongW(GetDlgItem(window, SourcePosition), GWL_STYLE) & WS_VISIBLE) ||
            IsWindowEnabled(GetDlgItem(window, SourcePrevious)))
            throw Error("Multi-source drop or navigator defaults failed");
        auto batchOutput = state.text(state.output);
        command(state, SourceNext, BN_CLICKED);
        if (state.sourceIndex != 1 || !state.sourceAnimation.animated || state.text(state.output) != batchOutput)
            throw Error("Next source preview failed");
        state.set(GetDlgItem(window, SourcePosition), L"1");
        SendMessageW(GetDlgItem(window, SourcePosition), WM_KEYDOWN, VK_RETURN, 0);
        if (state.sourceIndex != 0 || state.sourceAnimation.animated) throw Error("Source index entry failed");
        state.set(GetDlgItem(window, SourcePosition), L"500");
        command(state, SourcePosition, EN_KILLFOCUS);
        if (state.sourceIndex || state.text(GetDlgItem(window, SourcePosition)) != L"1")
            throw Error("Invalid source index changed preview");
        state.busy = true;
        dropMany(state.sourceView, {demo / L"character.png", demo / L"style.png"});
        command(state, SourceNext, BN_CLICKED); state.busy = false;
        if (state.sourceIndex || state.sources.size() != 2) throw Error("Busy batch selection changed");
        auto folder = demo / L"batch-import";
        fs::create_directories(folder / L"nested");
        fs::copy_file(demo / L"source.png", folder / L"01.png", fs::copy_options::overwrite_existing);
        fs::copy_file(demo / L"source.gif", folder / L"02.gif", fs::copy_options::overwrite_existing);
        fs::copy_file(demo / L"source.png", folder / L"nested/ignored.png", fs::copy_options::overwrite_existing);
        writeFile(folder / L"invalid.png", {0, 1, 2});
        drop(state.sourceView, folder);
        if (state.sources.size() != 2 || state.sources.front().path.filename() != L"01.png")
            throw Error("Folder drop, sorting or invalid-image handling failed");
        snapshot(1120, 830, L"ui-source-batch.png");
        command(state, SourceNext, BN_CLICKED);
        applyPlugins(state, {});
        if (state.sources.size() != 1 || (GetWindowLongW(GetDlgItem(window, SourcePosition), GWL_STYLE) & WS_VISIBLE))
            throw Error("Disabling batch plugin did not restore single-source workflow");
        loadSource(state, demo / L"source.gif");
        report["source_batch_plugin"] = "passed";
        // Repaint after imports so crop coordinates refer to the current reference image.
        snapshot(1120, 830, L"ui-compact.png");
        // Exercise the window's own crop handler using its rendered image bounds.
        const auto bounds = state.views[1].imageRect;
        int x1 = bounds.left + (bounds.right - bounds.left) / 4;
        int y1 = bounds.top + (bounds.bottom - bounds.top) / 4;
        int x2 = bounds.right - (bounds.right - bounds.left) / 4;
        int y2 = bounds.bottom - (bounds.bottom - bounds.top) / 4;
        viewProcedure(state.characterView, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
        viewProcedure(state.characterView, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x2, y2));
        viewProcedure(state.characterView, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
        if (state.crop.width < .4 || state.crop.width > .6 || state.crop.height < .4 || state.crop.height > .6)
            throw Error("Reference crop interaction failed");
        report["character_crop"] = "passed";
        if (state.text(state.get(Complexity)) != L"不发送" || !state.checked(Streaming) ||
            IsWindowEnabled(state.get(Compression)))
            throw Error("New provider control defaults are invalid");
        SendMessageW(state.get(OutputFormat), CB_SETCURSEL, 2, 0);
        command(state, OutputFormat, CBN_SELCHANGE);
        if (!IsWindowEnabled(state.get(Compression))) throw Error("JPEG compression control is disabled");
        state.checkBox(Streaming, false); command(state, Streaming, BN_CLICKED);
        if (IsWindowEnabled(state.get(PartialImages))) throw Error("Partial control must follow streaming");
        state.checkBox(Streaming, true); command(state, Streaming, BN_CLICKED);
        report["provider_controls"] = "passed";
        // Sub-notch wheel input must scroll the panel, rather than changing the model.
        auto model = state.text(state.get(Model));
        SendMessageW(state.get(Model), WM_MOUSEWHEEL, MAKEWPARAM(0, -15), 0);
        if (state.scroll <= 0 || state.text(state.get(Model)) != model)
            throw Error("High-resolution panel scrolling failed");
        viewProcedure(state.mainView, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 120), 0);
        if (state.views[2].zoom <= 1) throw Error("Pinch/Ctrl wheel zoom failed");
        viewProcedure(state.mainView, WM_MOUSEHWHEEL, MAKEWPARAM(0, 15), 0);
        if (state.views[2].panX == 0) throw Error("Horizontal high-resolution pan failed");
        viewProcedure(state.mainView, WM_LBUTTONDBLCLK, 0, 0);
        if (state.views[2].zoom != 1 || state.views[2].panX != 0) throw Error("Preview reset failed");
        report["precision_wheel_messages"] = "passed";
        auto preview = std::make_unique<Progress>();
        preview->frame = 0; preview->total = 3; preview->partial = 0;
        preview->message = L"第 1 帧：收到局部预览 1";
        preview->preview = std::make_shared<Image>(state.characterAnimation.frames.front().image);
        state.showingResult = true;
        SendMessageW(window, WM_PROGRESS, 0, reinterpret_cast<LPARAM>(preview.release()));
        if (!state.previewImage || state.text(state.frameInfo) != L"局部 1")
            throw Error("Partial image preview dispatch failed");
        report["partial_preview_dispatch"] = "passed";
        SendMessageW(state.settings, WM_VSCROLL, SB_BOTTOM, 0);
        snapshot(1280, 900, L"ui-settings.png");
        writeJson(demo / L"ui-results.json", report);
        DestroyWindow(window);
        return 0;
    }
    ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return static_cast<int>(message.wParam);
}
}
