#include "studio.hpp"
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>

namespace studio {
namespace {
enum Id { Prompt = 4100, Styles, AddStyle, RemoveStyle, Preview, Output, Browse };
struct DrawingDialog {
    HINSTANCE instance{};
    HWND window{};
    HFONT font{};
    Job value;
    Image preview;
    bool capture = false, accepted = false;
    std::string failure;
    ~DrawingDialog() { if (font) DeleteObject(font); }
};
static std::wstring text(HWND window, int id) {
    auto control = GetDlgItem(window, id);
    std::wstring value(GetWindowTextLengthW(control) + 1, L'\0');
    GetWindowTextW(control, value.data(), int(value.size())); value.pop_back(); return value;
}
static void showStyle(DrawingDialog& dialog) {
    auto index = SendDlgItemMessageW(dialog.window, Styles, LB_GETCURSEL, 0, 0);
    dialog.preview = index == LB_ERR ? Image{} :
        limited(decode(dialog.value.styles.at(size_t(index))).frames.front().image, 512);
    InvalidateRect(GetDlgItem(dialog.window, Preview), nullptr, TRUE);
}
static void addStyles(DrawingDialog& dialog, const std::vector<fs::path>& paths) {
    auto added = dialog.value.styles;
    for (const auto& path : paths) {
        bool duplicate = std::any_of(added.begin(), added.end(), [&](const auto& item) {
            return fs::equivalent(path, item);
        });
        if (!duplicate) added.push_back(path);
    }
    if (added.size() > 8) throw Error("最多支持 8 张画风参考图");
    for (const auto& path : added) decode(path);
    dialog.value.styles = std::move(added);
    SendDlgItemMessageW(dialog.window, Styles, LB_RESETCONTENT, 0, 0);
    for (const auto& path : dialog.value.styles)
        SendDlgItemMessageW(dialog.window, Styles, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.filename().c_str()));
    SendDlgItemMessageW(dialog.window, Styles, LB_SETCURSEL, dialog.value.styles.empty() ? -1 : 0, 0);
    showStyle(dialog);
}
static void capture(DrawingDialog& dialog) {
    RECT rect{}; GetWindowRect(dialog.window, &rect);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetDC(dialog.window), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels{}; auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) { DeleteDC(memory); ReleaseDC(dialog.window, dc); throw Error("Cannot capture drawing dialog"); }
    auto old = SelectObject(memory, bitmap);
    SendMessageW(dialog.window, WM_PRINT, reinterpret_cast<WPARAM>(memory),
        PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    Image image{UINT(width), UINT(height), Bytes(size_t(width) * height * 4)};
    memcpy(image.pixels.data(), pixels, image.pixels.size());
    for (size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(dialog.window, dc);
    savePng(dialog.value.output.parent_path() / L"ui-drawing.png", image);
}
static INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<DrawingDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<DrawingDialog*>(lparam); dialog->window = window;
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            SetWindowTextW(window, L"无图绘制");
            RECT rect{0, 0, 720, 590};
            AdjustWindowRectEx(&rect, DWORD(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)));
            int width = rect.right - rect.left, height = rect.bottom - rect.top;
            RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            SetWindowPos(window, nullptr, area.left + std::max(0L, (area.right - area.left - width) / 2),
                area.top + std::max(0L, (area.bottom - area.top - height) / 2), width, height, SWP_NOZORDER);
            dialog->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id,
                int x, int y, int w, int h) {
                auto item = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
                    type, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                    reinterpret_cast<HMENU>(INT_PTR(id)), dialog->instance, nullptr);
                SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font), TRUE); return item;
            };
            add(L"STATIC", L"绘制提示词", 0, 0, 16, 16, 688, 24);
            auto prompt = add(L"EDIT", dialog->value.instruction.c_str(), WS_TABSTOP | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, Prompt, 16, 48, 688, 196);
            SendMessageW(prompt, EM_SETLIMITTEXT, 24000, 0);
            add(L"STATIC", L"画风参考（可选）", 0, 0, 16, 266, 360, 24);
            add(L"BUTTON", L"添加…", WS_TABSTOP, AddStyle, 516, 260, 88, 30);
            add(L"BUTTON", L"移除", WS_TABSTOP, RemoveStyle, 616, 260, 88, 30);
            add(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY |
                LBS_NOINTEGRALHEIGHT, Styles, 16, 302, 400, 160);
            add(L"STATIC", L"", SS_OWNERDRAW, Preview, 432, 302, 272, 160);
            add(L"STATIC", L"保存为", 0, 0, 16, 490, 76, 24);
            add(L"EDIT", dialog->value.output.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, Output, 100, 486, 492, 28);
            add(L"BUTTON", L"浏览…", WS_TABSTOP, Browse, 604, 486, 100, 28);
            add(L"BUTTON", L"开始绘制", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 464, 542, 128, 32);
            add(L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL, 608, 542, 96, 32);
            auto styles = dialog->value.styles; dialog->value.styles.clear(); addStyles(*dialog, styles);
            DragAcceptFiles(window, TRUE);
            if (dialog->capture) PostMessageW(window, WM_APP + 9, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
        if (message == WM_DRAWITEM && wparam == Preview) {
            const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            FillRect(draw.hDC, &draw.rcItem, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            const auto& image = dialog->preview;
            if (!image.empty()) {
                int w = draw.rcItem.right - draw.rcItem.left, h = draw.rcItem.bottom - draw.rcItem.top;
                double scale = std::min(double(w) / image.width, double(h) / image.height);
                int width = std::max(1, int(image.width * scale)), height = std::max(1, int(image.height * scale));
                auto pixels = image.pixels;
                for (size_t i = 0; i < pixels.size(); i += 4)
                    for (size_t c = 0; c < 3; ++c)
                        pixels[i + c] = BYTE((UINT(pixels[i + c]) * pixels[i + 3] +
                            255u * (255u - pixels[i + 3]) + 127u) / 255u);
                BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = image.width; info.bmiHeader.biHeight = -LONG(image.height);
                info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
                SetStretchBltMode(draw.hDC, HALFTONE);
                StretchDIBits(draw.hDC, draw.rcItem.left + (w - width) / 2, draw.rcItem.top + (h - height) / 2,
                    width, height, 0, 0, image.width, image.height, pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            }
            return TRUE;
        }
        if (message == WM_DROPFILES) {
            auto drop = reinterpret_cast<HDROP>(wparam);
            struct Release { HDROP value; ~Release() { DragFinish(value); } } release{drop};
            auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            if (count > 8) throw Error("最多支持 8 张画风参考图");
            std::vector<fs::path> paths;
            for (UINT i = 0; i < count; ++i) {
                UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length + 1, L'\0');
                DragQueryFileW(drop, i, path.data(), length + 1); path.resize(length); paths.emplace_back(path);
            }
            addStyles(*dialog, paths); return TRUE;
        }
        if (message == WM_APP + 9 && dialog->capture) {
            SetDlgItemTextW(window, Prompt, L"绘制一张开心挥手的 Q 版人物表情，白色背景，简洁构图。");
            addStyles(*dialog, {dialog->value.output.parent_path() / L"style.png"});
            if (dialog->value.styles.size() != 1 || dialog->preview.empty()) throw Error("Drawing style preview failed");
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(RemoveStyle, BN_CLICKED), 0);
            if (!dialog->value.styles.empty() || !dialog->preview.empty()) throw Error("Drawing style removal failed");
            addStyles(*dialog, {dialog->value.output.parent_path() / L"style.png"});
            capture(*dialog); SendMessageW(window, WM_COMMAND, IDOK, 0); return TRUE;
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
        case Styles:
            if (HIWORD(wparam) == LBN_SELCHANGE) showStyle(*dialog);
            return TRUE;
        case RemoveStyle: {
            auto index = SendDlgItemMessageW(window, Styles, LB_GETCURSEL, 0, 0);
            if (index != LB_ERR) {
                auto paths = dialog->value.styles; paths.erase(paths.begin() + index);
                dialog->value.styles.clear(); addStyles(*dialog, paths);
            }
            return TRUE;
        }
        case AddStyle: {
            ComPtr<IFileOpenDialog> picker;
            check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&picker)), "Drawing references");
            picker->SetOptions(FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
            COMDLG_FILTERSPEC filter{L"图片 / GIF", L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.tif;*.tiff"};
            picker->SetFileTypes(1, &filter);
            auto result = picker->Show(window);
            if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return TRUE;
            check(result, "Select drawing references");
            ComPtr<IShellItemArray> items; check(picker->GetResults(&items), "Drawing reference selection");
            DWORD count{}; items->GetCount(&count); std::vector<fs::path> paths;
            for (DWORD i = 0; i < count; ++i) {
                ComPtr<IShellItem> item; check(items->GetItemAt(i, &item), "Drawing reference");
                PWSTR path{}; check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "Drawing reference path");
                paths.emplace_back(path); CoTaskMemFree(path);
            }
            addStyles(*dialog, paths); return TRUE;
        }
        case Browse: {
            ComPtr<IFileSaveDialog> picker;
            check(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&picker)), "Drawing output");
            COMDLG_FILTERSPEC filter{L"PNG 图片", L"*.png"}; picker->SetFileTypes(1, &filter);
            picker->SetDefaultExtension(L"png");
            picker->SetFileName(fs::path(text(window, Output)).filename().c_str());
            auto result = picker->Show(window);
            if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return TRUE;
            check(result, "Select drawing output");
            ComPtr<IShellItem> item; check(picker->GetResult(&item), "Drawing output item");
            PWSTR path{}; check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "Drawing output path");
            SetDlgItemTextW(window, Output, path); CoTaskMemFree(path); return TRUE;
        }
        case IDOK:
            dialog->value.instruction = text(window, Prompt);
            if (dialog->value.instruction.find_first_not_of(L" \t\r\n") == std::wstring::npos)
                throw Error("请填写绘制提示词");
            dialog->value.output = text(window, Output);
            if (dialog->value.output.empty() || _wcsicmp(dialog->value.output.extension().c_str(), L".png") != 0)
                throw Error("请选择 .png 保存路径");
            dialog->accepted = true; EndDialog(window, IDOK); return TRUE;
        }
    } catch (const std::exception& error) {
        if (dialog && dialog->capture) { dialog->failure = error.what(); EndDialog(window, IDABORT); return TRUE; }
        MessageBoxW(window, wide(error.what()).c_str(), L"无图绘制", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
}
bool editDrawing(HINSTANCE instance, HWND owner, Job& job, bool capture) {
    DrawingDialog dialog; dialog.instance = instance; dialog.value = job; dialog.capture = capture;
    dialog.value.generate = true; dialog.value.refine = false;
    dialog.value.source.clear(); dialog.value.character.clear();
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    resource.dialog.cx = 480; resource.dialog.cy = 380;
    auto result = DialogBoxIndirectParamW(instance, &resource.dialog, owner, procedure, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1) throw Error("无法打开无图绘制窗口");
    if (!dialog.failure.empty()) throw Error(dialog.failure);
    if (!dialog.accepted) return false;
    job = std::move(dialog.value); return true;
}
}
