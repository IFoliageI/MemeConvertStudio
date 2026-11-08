#include "studio.hpp"
#include <commctrl.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace studio {
namespace {
enum Control { Canvas = 3100, SourceFile, SourceName, Prompt, Styles, AddStyle, RemoveStyle,
    WholeImage, OutputPath, SavePath, Frames, FrameInfo };
struct Editor {
    HINSTANCE instance{};
    HWND window{}, canvas{};
    HFONT font{};
    Job value;
    Animation animation;
    RECT imageRect{};
    POINT start{}, end{};
    bool dragging = false, capture = false, accepted = false;
    int frame = 0;
    std::string failure;
    ~Editor() { if (font) DeleteObject(font); }
};
static std::wstring text(HWND window, int id) {
    HWND item = GetDlgItem(window, id);
    std::wstring result(GetWindowTextLengthW(item) + 1, L'\0');
    GetWindowTextW(item, result.data(), int(result.size()));
    result.resize(result.size() - 1); return result;
}
static void set(HWND window, int id, const std::wstring& value) {
    SetDlgItemTextW(window, id, value.c_str());
}
static std::vector<fs::path> pick(HWND window, bool multiple) {
    ComPtr<IFileOpenDialog> dialog;
    check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)), "Open redraw image");
    FILEOPENDIALOGOPTIONS flags{}; dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
        (multiple ? FOS_ALLOWMULTISELECT : 0));
    COMDLG_FILTERSPEC types[]{{L"图片 / GIF", L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.tif;*.tiff"}};
    dialog->SetFileTypes(1, types);
    HRESULT result = dialog->Show(window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    check(result, "Select redraw image");
    ComPtr<IShellItemArray> items; check(dialog->GetResults(&items), "Redraw selection");
    DWORD count{}; items->GetCount(&count);
    std::vector<fs::path> paths;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item; check(items->GetItemAt(i, &item), "Redraw item");
        PWSTR path{}; check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "Redraw path");
        paths.emplace_back(path); CoTaskMemFree(path);
    }
    return paths;
}
static void load(Editor& editor, const fs::path& path) {
    auto animation = decode(path);
    editor.value.source = path; editor.animation = std::move(animation);
    editor.value.region = {}; editor.frame = 0;
    auto extension = editor.animation.animated ? L".gif" : L".png";
    auto stem = path.stem().wstring() + L"-retouched";
    fs::path output = path.parent_path() / (stem + extension);
    for (int i = 2; fs::exists(output); ++i)
        output = path.parent_path() / (stem + L"-" + std::to_wstring(i) + extension);
    editor.value.output = output;
    set(editor.window, SourceName, path.wstring()); set(editor.window, OutputPath, output.wstring());
    set(editor.window, FrameInfo, std::to_wstring(editor.animation.frames.size()) + L" 帧");
    SendDlgItemMessageW(editor.window, Frames, TBM_SETRANGEMIN, FALSE, 0);
    SendDlgItemMessageW(editor.window, Frames, TBM_SETRANGEMAX, TRUE, editor.animation.frames.size() - 1);
    SendDlgItemMessageW(editor.window, Frames, TBM_SETPOS, TRUE, 0);
    EnableWindow(GetDlgItem(editor.window, Frames), editor.animation.frames.size() > 1);
    InvalidateRect(editor.canvas, nullptr, FALSE);
}
static void addStyles(Editor& editor, const std::vector<fs::path>& paths) {
    if (paths.size() + editor.value.styles.size() > 8) throw Error("最多支持 8 张画风参考图");
    for (const auto& path : paths) decode(path);
    for (const auto& path : paths) {
        editor.value.styles.push_back(path);
        SendDlgItemMessageW(editor.window, Styles, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(path.filename().c_str()));
    }
}
static LRESULT CALLBACK canvasProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR context) {
    auto& editor = *reinterpret_cast<Editor*>(context);
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(window, canvasProcedure, 1); return DefSubclassProc(window, message, wparam, lparam); }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
        RECT rect{}; GetClientRect(window, &rect);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        if (!editor.animation.frames.empty()) {
            const auto& image = editor.animation.frames[editor.frame].image;
            double scale = std::min(double(rect.right - 16) / image.width, double(rect.bottom - 16) / image.height);
            int width = std::max(1, int(image.width * scale)), height = std::max(1, int(image.height * scale));
            int x = (rect.right - width) / 2, y = (rect.bottom - height) / 2;
            editor.imageRect = {x, y, x + width, y + height};
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = image.width; info.bmiHeader.biHeight = -LONG(image.height);
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            auto pixels = image.pixels;
            for (size_t i = 0; i < pixels.size(); i += 4)
                for (size_t c = 0; c < 3; ++c)
                    pixels[i + c] = BYTE((UINT(pixels[i + c]) * pixels[i + 3] +
                        255u * (255u - pixels[i + 3]) + 127u) / 255u);
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, x, y, width, height, 0, 0, image.width, image.height,
                pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            auto region = editor.value.region;
            RECT selection{x + LONG(std::lround(region.x * width)), y + LONG(std::lround(region.y * height)),
                x + LONG(std::lround((region.x + region.width) * width)),
                y + LONG(std::lround((region.y + region.height) * height))};
            if (editor.dragging) selection = {std::min(editor.start.x, editor.end.x),
                std::min(editor.start.y, editor.end.y), std::max(editor.start.x, editor.end.x),
                std::max(editor.start.y, editor.end.y)};
            auto pen = CreatePen(PS_SOLID, 2, RGB(0, 135, 118));
            auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, selection.left, selection.top, selection.right, selection.bottom);
            SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
        }
        FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    auto bounds = editor.imageRect;
    if (message == WM_LBUTTONDOWN && PtInRect(&bounds, point)) {
        editor.start = editor.end = point; editor.dragging = true; SetCapture(window); return 0;
    }
    if ((message == WM_MOUSEMOVE || message == WM_LBUTTONUP) && editor.dragging) {
        editor.end = {std::clamp(point.x, bounds.left, bounds.right), std::clamp(point.y, bounds.top, bounds.bottom)};
        if (message == WM_LBUTTONUP) {
            editor.dragging = false; ReleaseCapture();
            if (abs(editor.start.x - editor.end.x) >= 2 && abs(editor.start.y - editor.end.y) >= 2)
                editor.value.region = {
                    double(std::min(editor.start.x, editor.end.x) - bounds.left) / (bounds.right - bounds.left),
                    double(std::min(editor.start.y, editor.end.y) - bounds.top) / (bounds.bottom - bounds.top),
                    double(abs(editor.start.x - editor.end.x)) / (bounds.right - bounds.left),
                    double(abs(editor.start.y - editor.end.y)) / (bounds.bottom - bounds.top)};
        }
        InvalidateRect(window, nullptr, FALSE); return 0;
    }
    if (message == WM_CAPTURECHANGED) { editor.dragging = false; InvalidateRect(window, nullptr, FALSE); }
    return DefSubclassProc(window, message, wparam, lparam);
}
static void captureEditor(Editor& editor) {
    RECT rect{}; GetWindowRect(editor.window, &rect);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetDC(editor.window), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels{}; auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) { DeleteDC(memory); ReleaseDC(editor.window, dc); throw Error("Cannot capture redraw dialog"); }
    auto old = SelectObject(memory, bitmap);
    SendMessageW(editor.window, WM_PRINT, reinterpret_cast<WPARAM>(memory),
        PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    Image image{UINT(width), UINT(height), Bytes(size_t(width) * height * 4)};
    memcpy(image.pixels.data(), pixels, image.pixels.size());
    for (size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(editor.window, dc);
    savePng(editor.value.source.parent_path() / L"ui-refine.png", image);
}
static INT_PTR CALLBACK editorProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            editor = reinterpret_cast<Editor*>(lparam); editor->window = window;
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
            SetWindowTextW(window, L"局部重绘");
            RECT size{0, 0, 928, 684};
            AdjustWindowRectEx(&size, DWORD(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)));
            int width = size.right - size.left, height = size.bottom - size.top;
            SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
            RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            SetWindowPos(window, nullptr, area.left + std::max(0L, (area.right - area.left - width) / 2),
                area.top + std::max(0L, (area.bottom - area.top - height) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            editor->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id,
                int x, int y, int width, int height) {
                auto item = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
                    type, label, WS_CHILD | WS_VISIBLE | style, x, y, width, height, window,
                    reinterpret_cast<HMENU>(INT_PTR(id)), editor->instance, nullptr);
                SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(editor->font), TRUE); return item;
            };
            add(L"STATIC", L"当前产物", 0, 0, 16, 18, 120, 24);
            add(L"STATIC", L"", SS_PATHELLIPSIS, SourceName, 140, 18, 650, 24);
            add(L"BUTTON", L"选择文件…", WS_TABSTOP, SourceFile, 800, 14, 112, 30);
            add(L"STATIC", L"重绘区域", 0, 0, 16, 62, 120, 24);
            add(L"BUTTON", L"全图", WS_TABSTOP, WholeImage, 420, 56, 76, 30);
            editor->canvas = add(L"STATIC", L"", SS_NOTIFY, Canvas, 16, 96, 480, 436);
            SetWindowSubclass(editor->canvas, canvasProcedure, 1, reinterpret_cast<DWORD_PTR>(editor));
            add(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_NOTICKS, Frames, 16, 542, 374, 28);
            add(L"STATIC", L"", 0, FrameInfo, 402, 546, 94, 24);
            add(L"STATIC", L"修改要求", 0, 0, 516, 62, 180, 24);
            auto prompt = add(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN | WS_VSCROLL, Prompt, 516, 96, 396, 190);
            SendMessageW(prompt, EM_SETLIMITTEXT, 24000, 0);
            add(L"STATIC", L"本轮画风参考", 0, 0, 516, 306, 180, 24);
            add(L"BUTTON", L"添加…", WS_TABSTOP, AddStyle, 724, 300, 88, 30);
            add(L"BUTTON", L"移除", WS_TABSTOP, RemoveStyle, 824, 300, 88, 30);
            add(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                LBS_NOTIFY, Styles, 516, 342, 396, 190);
            add(L"STATIC", L"另存为", 0, 0, 16, 588, 84, 24);
            add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, OutputPath, 104, 584, 680, 28);
            add(L"BUTTON", L"选择路径…", WS_TABSTOP, SavePath, 800, 584, 112, 28);
            add(L"BUTTON", L"开始重绘", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 672, 634, 128, 32);
            add(L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL, 816, 634, 96, 32);
            if (!editor->value.source.empty()) load(*editor, editor->value.source);
            if (editor->capture) PostMessageW(window, WM_APP + 8, 0, 0);
            return TRUE;
        }
        if (!editor) return FALSE;
        if (message == WM_APP + 8 && editor->capture) {
            UpdateWindow(editor->canvas);
            // Render once before using the image bounds in synthetic selection events.
            HDC dc = GetDC(editor->canvas);
            SendMessageW(editor->canvas, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
            ReleaseDC(editor->canvas, dc);
            auto bounds = editor->imageRect;
            int x1 = bounds.left + (bounds.right - bounds.left) / 4;
            int y1 = bounds.top + (bounds.bottom - bounds.top) / 4;
            int x2 = bounds.right - (bounds.right - bounds.left) / 4;
            int y2 = bounds.bottom - (bounds.bottom - bounds.top) / 4;
            SendMessageW(editor->canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
            SendMessageW(editor->canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x2, y2));
            SendMessageW(editor->canvas, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
            if (editor->value.region.width < .49 || editor->value.region.width > .51)
                throw Error("Redraw rectangle interaction failed");
            set(window, Prompt, L"修正眼睛细节，沿用新参考的线条画风。");
            addStyles(*editor, {editor->value.source.parent_path() / L"style.png"});
            RECT client{}, button{}; GetClientRect(window, &client);
            GetWindowRect(GetDlgItem(window, IDOK), &button);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&button), 2);
            if (button.right > client.right || button.bottom > client.bottom)
                throw Error("Redraw action is outside the dialog");
            captureEditor(*editor);
            SendMessageW(window, WM_COMMAND, IDOK, 0); return TRUE;
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message == WM_HSCROLL) {
            editor->frame = int(SendDlgItemMessageW(window, Frames, TBM_GETPOS, 0, 0));
            InvalidateRect(editor->canvas, nullptr, FALSE);
            set(window, FrameInfo, std::to_wstring(editor->frame + 1) + L" / " +
                std::to_wstring(editor->animation.frames.size())); return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
        case SourceFile: {
            auto paths = pick(window, false);
            if (!paths.empty()) load(*editor, paths.front()); return TRUE;
        }
        case AddStyle: addStyles(*editor, pick(window, true)); return TRUE;
        case RemoveStyle: {
            auto index = SendDlgItemMessageW(window, Styles, LB_GETCURSEL, 0, 0);
            if (index != LB_ERR) {
                editor->value.styles.erase(editor->value.styles.begin() + index);
                SendDlgItemMessageW(window, Styles, LB_DELETESTRING, index, 0);
            }
            return TRUE;
        }
        case WholeImage:
            editor->value.region = {}; InvalidateRect(editor->canvas, nullptr, FALSE); return TRUE;
        case SavePath: {
            if (editor->animation.frames.empty()) throw Error("请先选择已有产物");
            ComPtr<IFileSaveDialog> dialog;
            check(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&dialog)), "Save redraw");
            auto path = fs::path(text(window, OutputPath));
            dialog->SetFileName(path.filename().c_str());
            auto extension = editor->animation.animated ? L"gif" : L"png";
            COMDLG_FILTERSPEC type{editor->animation.animated ? L"GIF" : L"PNG",
                editor->animation.animated ? L"*.gif" : L"*.png"};
            dialog->SetFileTypes(1, &type); dialog->SetDefaultExtension(extension);
            auto result = dialog->Show(window);
            if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return TRUE;
            check(result, "Redraw save path");
            ComPtr<IShellItem> item; check(dialog->GetResult(&item), "Redraw output");
            PWSTR selected{}; check(item->GetDisplayName(SIGDN_FILESYSPATH, &selected), "Redraw output path");
            set(window, OutputPath, selected); CoTaskMemFree(selected); return TRUE;
        }
        case IDOK:
            if (editor->value.source.empty()) throw Error("请先选择已有产物");
            editor->value.instruction = text(window, Prompt);
            if (editor->value.instruction.find_first_not_of(L" \t\r\n") == std::wstring::npos)
                throw Error("请填写本次重绘的修改要求");
            editor->value.output = text(window, OutputPath);
            if (editor->value.output.empty()) throw Error("请选择保存路径");
            editor->accepted = true; EndDialog(window, IDOK); return TRUE;
        }
    } catch (const std::exception& error) {
        if (editor && editor->capture) {
            editor->failure = error.what(); EndDialog(window, IDABORT); return TRUE;
        }
        MessageBoxW(window, wide(error.what()).c_str(), L"局部重绘", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
}
bool editRefinement(HINSTANCE instance, HWND owner, Job& job, bool capture) {
    Editor editor; editor.instance = instance; editor.value.source = job.source; editor.capture = capture;
    editor.value.refine = true; editor.value.maxFrames = 0;
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    resource.dialog.cx = 580; resource.dialog.cy = 420;
    auto result = DialogBoxIndirectParamW(instance, &resource.dialog, owner, editorProcedure,
        reinterpret_cast<LPARAM>(&editor));
    if (result == -1) throw Error("无法打开局部重绘窗口");
    if (!editor.failure.empty()) throw Error(editor.failure);
    if (!editor.accepted) return false;
    job = std::move(editor.value); return true;
}
}
