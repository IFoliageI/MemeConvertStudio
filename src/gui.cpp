#include "studio.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <thread>
#include <algorithm>
#include <memory>
#include <sstream>
#include <iomanip>

namespace studio {
namespace {
constexpr UINT WM_PROGRESS = WM_APP + 1, WM_FINISHED = WM_APP + 2;
constexpr COLORREF ink = RGB(34, 39, 43), muted = RGB(104, 113, 120);
constexpr COLORREF paper = RGB(245, 247, 248), line = RGB(218, 224, 227);
enum Id {
    Source = 100, Character, AddStyle, RemoveStyle, ClearCrop, StyleList,
    BaseUrl, Key, Model, Quality, Size, ImageField, Extra, Timeout, Stride, MaxFrames,
    Consistency, PreserveText, Resume, Remember, Instruction, Output, BrowseOutput,
    Generate, CancelJob, SourceTab, ResultTab, Play, Timeline, OpenResult, OpenFolder,
    FetchModels
};
struct State;
struct View {
    State* state = nullptr;
    int kind = 0;
    RECT imageRect{};
    POINT start{}, end{};
    bool dragging = false;
};
struct Completion {
    bool success = false, modelFetch = false;
    std::wstring message; fs::path output;
    std::vector<std::string> models;
};
struct State {
    HINSTANCE instance{};
    HWND window{}, settings{}, sourceView{}, characterView{}, mainView{}, styles{},
        log{}, progress{}, status{}, estimate{}, timeline{}, instruction{}, output{},
        sourcePath{}, characterPath{}, cropInfo{}, frameInfo{};
    HFONT font{}, titleFont{}, smallFont{};
    HBRUSH background = CreateSolidBrush(paper), white = CreateSolidBrush(RGB(255, 255, 255));
    std::vector<HWND> lockControls;
    Animation sourceAnimation, characterAnimation, resultAnimation;
    fs::path source, character, resultPath;
    std::vector<fs::path> stylePaths;
    Crop crop;
    View views[3];
    int frame = 0, elapsed = 0, scroll = 0;
    bool showingResult = false, playing = true, busy = false, closing = false, demo = false,
        fetchingModels = false;
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
    void refreshViews() {
        frame = 0; elapsed = 0;
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
            int calls = (count + stride - 1) / stride;
            uint64_t duration = 0;
            for (auto& item : sourceAnimation.frames) duration += item.delayMs;
            std::wostringstream durationText;
            durationText << std::fixed << std::setprecision(2) << duration / 1000.0;
            set(estimate, std::to_wstring(sourceAnimation.frames[0].image.width) + L" × " +
                std::to_wstring(sourceAnimation.frames[0].image.height) + L"  ·  " +
                std::to_wstring(count) + L" 帧  ·  " + durationText.str() +
                L" 秒\r\n预计最多 " + std::to_wstring(calls) + L" 次生成请求");
        } catch (...) { set(estimate, L"请检查抽帧参数"); }
    }
};
static HWND control(State& state, HWND parent, const wchar_t* type, const std::wstring& label,
    DWORD style, int id, int x, int y, int width, int height, bool lock = false) {
    HWND result = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
        type, label.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, width, height,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), state.instance, nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    if (lock) state.lockControls.push_back(result);
    return result;
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
static RECT fit(const Image& image, RECT area) {
    int width = area.right - area.left, height = area.bottom - area.top;
    double scale = std::min(double(width) / image.width, double(height) / image.height);
    int w = std::max(1, int(image.width * scale)), h = std::max(1, int(image.height * scale));
    int x = area.left + (width - w) / 2, y = area.top + (height - h) / 2;
    return {x, y, x + w, y + h};
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
static LRESULT CALLBACK viewProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* view = reinterpret_cast<View*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    }
    if (!view) return DefWindowProcW(window, message, wparam, lparam);
    auto& state = *view->state;
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
        if (!animation->frames.empty()) {
            int index = view->kind == 1 ? 0 : state.frame % static_cast<int>(animation->frames.size());
            auto& image = animation->frames[index].image;
            RECT inset{10, 10, client.right - 10, client.bottom - 10};
            view->imageRect = fit(image, inset);
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
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 836,
            static_cast<UINT>(rect.bottom), state->scroll, 0};
        SetScrollInfo(window, SB_VERT, &info, TRUE);
    }
    if (message == WM_VSCROLL || message == WM_MOUSEWHEEL) {
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL}; GetScrollInfo(window, SB_VERT, &info);
        int next = info.nPos;
        if (message == WM_MOUSEWHEEL) next -= GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 64;
        else switch (LOWORD(wparam)) {
            case SB_LINEUP: next -= 28; break;
            case SB_LINEDOWN: next += 28; break;
            case SB_PAGEUP: next -= info.nPage; break;
            case SB_PAGEDOWN: next += info.nPage; break;
            case SB_THUMBTRACK: next = info.nTrackPos; break;
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
    label(L"质量", 256); combo(Quality, 278, {L"auto", L"low", L"medium", L"high", L"不发送"});
    label(L"输出尺寸参数", 318); combo(Size, 340,
        {L"auto", L"1024x1024", L"1536x1024", L"1024x1536", L"不发送"}, true);
    label(L"多图上传字段", 380); combo(ImageField, 402, {L"image[]", L"image"});
    label(L"扩展参数（JSON）", 442);
    editControl(Extra, L"{}", 464, 60, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
    label(L"单次请求超时（秒）", 536); editControl(Timeout, L"600", 558, 28, ES_NUMBER);
    label(L"GIF 间隔 / 帧数上限（0 不限）", 600);
    auto stride = editControl(Stride, L"1", 624, 28, ES_NUMBER);
    auto maximum = editControl(MaxFrames, L"24", 624, 28, ES_NUMBER);
    MoveWindow(stride, 16, 624, 138, 28, TRUE); MoveWindow(maximum, 170, 624, 138, 28, TRUE);
    checkbox(Consistency, L"多帧参考：动作、画风与背景一致", 668, true);
    checkbox(PreserveText, L"保留原表情文字", 702, true);
    checkbox(Resume, L"复用已完成帧，支持断点续作", 736, true);
}
static void saveSettings(State& state) {
    if (state.demo) return;
    Json data;
    for (int id : {BaseUrl, Model, Quality, Size, ImageField, Extra, Timeout, Stride, MaxFrames})
        data[std::to_string(id)] = utf8(state.text(state.get(id)));
    for (int id : {Consistency, PreserveText, Resume, Remember}) data[std::to_string(id)] = state.checked(id);
    if (state.checked(Remember)) data["encrypted_key"] = protect(state.text(state.get(Key)));
    writeJson(settingsPath(), data);
}
static void loadSettings(State& state) {
    if (state.demo) return;
    try {
        if (!fs::exists(settingsPath())) return;
        Json data = readJson(settingsPath());
        for (int id : {BaseUrl, Model, Quality, Size, ImageField, Extra, Timeout, Stride, MaxFrames}) {
            if (!data.contains(std::to_string(id))) continue;
            auto text = wide(data.at(std::to_string(id)).get<std::string>());
            HWND item = state.get(id);
            if (id == Quality || id == ImageField) {
                auto index = SendMessageW(item, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text.c_str()));
                if (index != CB_ERR) SendMessageW(item, CB_SETCURSEL, index, 0);
            } else state.set(item, text);
        }
        for (int id : {Consistency, PreserveText, Resume, Remember})
            if (data.contains(std::to_string(id))) state.checkBox(id, data[std::to_string(id)].get<bool>());
        if (state.checked(Remember) && data.contains("encrypted_key"))
            state.set(state.get(Key), unprotect(data["encrypted_key"].get<std::string>()));
    } catch (const std::exception& error) {
        state.logLine(L"读取设置失败：" + wide(error.what()));
    }
}
static void loadSource(State& state, const fs::path& path) {
    auto animation = decode(path);
    state.source = path; state.sourceAnimation = std::move(animation);
    state.resultAnimation = {}; state.resultPath.clear();
    state.set(state.sourcePath, path.filename().wstring());
    fs::path output = path.parent_path() / (path.stem().wstring() + L"-edited" +
        (state.sourceAnimation.animated ? L".gif" : L".png"));
    state.set(state.output, output.wstring());
    state.showingResult = false; state.refreshViews(); state.updateEstimate();
    state.set(state.status, L"素材已就绪");
}
static void loadCharacter(State& state, const fs::path& path) {
    auto animation = decode(path);
    if (animation.frames.size() > 1) animation.frames.resize(1);
    state.character = path; state.characterAnimation = std::move(animation); state.crop = {};
    state.set(state.characterPath, path.filename().wstring());
    state.set(state.cropInfo, L"完整参考图");
    InvalidateRect(state.characterView, nullptr, FALSE);
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
static void start(State& state) {
    if (state.busy) return;
    if (state.source.empty() || state.character.empty()) throw Error("请先导入表情素材和人物参考图");
    Job job;
    job.source = state.source; job.character = state.character; job.styles = state.stylePaths;
    job.crop = state.crop; job.instruction = state.text(state.instruction);
    job.output = state.text(state.output);
    job.stride = state.number(Stride, 1, 600); job.maxFrames = state.number(MaxFrames, 0, 600);
    job.consistency = state.checked(Consistency); job.preserveText = state.checked(PreserveText);
    job.resume = state.checked(Resume);
    job.provider.baseUrl = state.text(state.get(BaseUrl)); job.provider.key = state.text(state.get(Key));
    job.provider.model = utf8(state.text(state.get(Model))); job.provider.quality = utf8(state.text(state.get(Quality)));
    auto modelBegin = job.provider.model.find_first_not_of(" \t\r\n");
    auto modelEnd = job.provider.model.find_last_not_of(" \t\r\n");
    if (modelBegin == std::string::npos) throw Error("请先选择或填写模型名称");
    job.provider.model = job.provider.model.substr(modelBegin, modelEnd - modelBegin + 1);
    job.provider.size = utf8(state.text(state.get(Size))); job.provider.imageField = utf8(state.text(state.get(ImageField)));
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
    if (job.output.empty()) {
        auto path = chooseOutput(state); if (path.empty()) return;
        job.output = path; state.set(state.output, path.wstring());
    }
    if (fs::exists(job.output) && MessageBoxW(state.window,
        (L"输出文件已存在，生成完成后将替换：\n" + job.output.wstring()).c_str(),
        L"替换输出文件", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    state.cancel.reset(); state.busy = true;
    for (auto control : state.lockControls) EnableWindow(control, FALSE);
    EnableWindow(GetDlgItem(state.window, CancelJob), TRUE);
    state.set(state.status, L"正在准备任务…");
    state.logLine(L"任务开始，模型：" + wide(job.provider.model));
    SendMessageW(state.progress, PBM_SETPOS, 0, 0);
    if (state.worker.joinable()) state.worker.join();
    state.worker = std::thread([&state, job] {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto completion = std::make_unique<Completion>();
        try {
            check(initialized, "Worker COM");
            runJob(job, state.cancel, [&](const Progress& progress) {
                auto event = std::make_unique<Progress>(progress);
                if (PostMessageW(state.window, WM_PROGRESS, 0, reinterpret_cast<LPARAM>(event.get()))) event.release();
            });
            completion->success = true; completion->output = job.output;
            completion->message = L"生成完成";
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
    label(L"原始表情", 16, 72, 120); button(Source, L"导入…", 176, 66, 82);
    state.sourcePath = control(state, state.window, L"STATIC", L"未选择文件", SS_PATHELLIPSIS, 0, 16, 103, 242, 22);
    label(L"参考人物", 16, 280, 120); button(Character, L"导入…", 176, 274, 82);
    state.characterPath = control(state, state.window, L"STATIC", L"未选择文件", SS_PATHELLIPSIS, 0, 16, 310, 242, 22);
    for (int i = 0; i < 3; ++i) { state.views[i].state = &state; state.views[i].kind = i; }
    state.sourceView = CreateWindowExW(0, L"MemeStudioView", L"原始表情预览", WS_CHILD | WS_VISIBLE,
        16, 134, 242, 126, state.window, nullptr, state.instance, &state.views[0]);
    state.characterView = CreateWindowExW(0, L"MemeStudioView", L"人物参考预览与裁剪", WS_CHILD | WS_VISIBLE,
        16, 342, 242, 158, state.window, nullptr, state.instance, &state.views[1]);
    state.mainView = CreateWindowExW(0, L"MemeStudioView", L"工作区预览", WS_CHILD | WS_VISIBLE,
        288, 110, 500, 420, state.window, nullptr, state.instance, &state.views[2]);
    state.cropInfo = label(L"完整参考图", 16, 510, 155);
    button(ClearCrop, L"重置裁剪", 176, 504, 82);
    label(L"画风参考", 16, 550, 100);
    button(AddStyle, L"添加", 130, 544, 60); button(RemoveStyle, L"移除", 198, 544, 60);
    state.styles = control(state, state.window, L"LISTBOX", L"",
        WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER,
        StyleList, 16, 582, 242, 65, true);
    label(L"附加要求", 16, 658, 200);
    state.instruction = control(state, state.window, L"EDIT", L"",
        WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
        Instruction, 16, 686, 242, 100, true);
    button(SourceTab, L"原图", 0, 0, 82, false); button(ResultTab, L"结果", 0, 0, 82, false);
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
    createSettings(state); loadSettings(state);
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
    tip(GetDlgItem(state.window, BrowseOutput), L"选择输出文件");
    tip(state.get(Stride), L"每隔多少源帧生成一帧；合并对应显示时长");
    tip(state.get(MaxFrames), L"自动抽帧，覆盖完整动画；0 表示不设上限");
    tip(state.get(ImageField), L"多张图片重复使用同一个上传字段");
    if (state.text(state.get(Key)).empty() && !state.demo) {
        wchar_t key[8192]{};
        DWORD count = GetEnvironmentVariableW(L"OPENAI_API_KEY", key, 8192);
        if (count && count < 8192) state.set(state.get(Key), key);
        SecureZeroMemory(key, sizeof(key));
    }
    SetTimer(state.window, 1, 20, nullptr);
}
static void command(State& state, int id, int notification) {
    if ((id == Stride || id == MaxFrames) && notification == EN_CHANGE) { state.updateEstimate(); return; }
    if (id == Source || id == Character) {
        auto paths = chooseImages(state.window, false); if (paths.empty()) return;
        if (id == Source) loadSource(state, paths[0]); else loadCharacter(state, paths[0]);
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
        auto path = chooseOutput(state); if (!path.empty()) state.set(state.output, path.wstring());
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
                SendMessageW(state->timeline, TBM_SETPOS, TRUE, state->frame); state->updateFrameLabel();
                InvalidateRect(state->mainView, nullptr, FALSE); InvalidateRect(state->sourceView, nullptr, FALSE);
            }
            return 0;
        }
        case WM_PROGRESS: {
            std::unique_ptr<Progress> progress(reinterpret_cast<Progress*>(lparam));
            state->set(state->status, progress->message);
            if (progress->total) SendMessageW(state->progress, PBM_SETPOS, progress->done * 100 / progress->total, 0);
            state->logLine(progress->message); return 0;
        }
        case WM_FINISHED: {
            std::unique_ptr<Completion> completion(reinterpret_cast<Completion*>(lparam));
            if (state->worker.joinable()) state->worker.join();
            state->busy = false;
            for (auto control : state->lockControls) EnableWindow(control, TRUE);
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
            if (completion->success) {
                state->resultAnimation = decode(completion->output); state->resultPath = completion->output;
                state->showingResult = true; state->refreshViews();
            } else if (!state->closing && !state->cancel.stopped) {
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
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{sizeof(type)}; type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hbrBackground = state.background; type.lpszClassName = L"MemeStudio"; type.lpfnWndProc = procedure;
    RegisterClassExW(&type);
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
        auto snapshot = [&](int width, int height, const wchar_t* filename) {
            SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(state);
            Image image{static_cast<UINT>(width), static_cast<UINT>(height),
                Bytes(size_t(width) * height * 4)};
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            HDC dc = GetDC(window), memory = CreateCompatibleDC(dc);
            void* pixels = nullptr;
            HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!dib) throw Error("Cannot capture UI bitmap");
            auto old = SelectObject(memory, dib);
            SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND | PRF_NONCLIENT);
            memcpy(image.pixels.data(), pixels, image.pixels.size());
            for (size_t p = 3; p < image.pixels.size(); p += 4) image.pixels[p] = 255;
            SelectObject(memory, old); DeleteObject(dib); DeleteDC(memory); ReleaseDC(window, dc);
            savePng(demo / filename, image);
            report["snapshots"].push_back({{"width", width}, {"height", height}, {"file", utf8(filename)}});
        };
        snapshot(1280, 900, L"ui-wide.png");
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
