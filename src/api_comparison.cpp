#include "api_routing.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <thread>

namespace studio {
namespace {
enum { Mode = 4500, Prompt, Rows, Left, Right, ViewLeft, ViewRight, Run, Stop, Summary, Folder };
constexpr UINT ResultMessage = WM_APP + 51, FinishMessage = WM_APP + 52;
struct Result { int index = 0; Image image; Json row; };
struct Dialog {
    HINSTANCE instance{};
    HWND window{}, list{};
    HFONT font{};
    RoutingOptions options;
    Job input;
    std::vector<size_t> channels;
    std::vector<Image> images;
    fs::path output;
    Json report = Json::array();
    Cancel cancel;
    std::thread worker;
    bool busy = false, closing = false, ran = false;
    bool capture = false;
    std::function<void(HWND)> snapshot;
    std::string failure;
    ~Dialog() { cancel.stop(); if (worker.joinable()) worker.join(); if (font) DeleteObject(font); }
};
std::wstring text(HWND window, int id) {
    auto item = GetDlgItem(window, id);
    std::wstring value(GetWindowTextLengthW(item) + 1, L'\0');
    GetWindowTextW(item, value.data(), int(value.size())); value.pop_back(); return value;
}
void cell(HWND list, int row, int column, std::wstring value) {
    ListView_SetItemText(list, row, column, value.data());
}
void drawImage(const DRAWITEMSTRUCT& draw, const Image& image) {
    FillRect(draw.hDC, &draw.rcItem, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    if (image.empty()) return;
    int w = draw.rcItem.right - draw.rcItem.left, h = draw.rcItem.bottom - draw.rcItem.top;
    double scale = std::min(double(w) / image.width, double(h) / image.height);
    int width = std::max(1, int(image.width * scale)), height = std::max(1, int(image.height * scale));
    auto pixels = image.pixels;
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (size_t c = 0; c < 3; ++c) pixels[i + c] = BYTE((UINT(pixels[i + c]) * pixels[i + 3] +
            255u * (255u - pixels[i + 3]) + 127u) / 255u);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width; info.bmiHeader.biHeight = -LONG(image.height);
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    SetStretchBltMode(draw.hDC, HALFTONE);
    StretchDIBits(draw.hDC, (w - width) / 2, (h - height) / 2, width, height,
        0, 0, image.width, image.height, pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}
void start(Dialog& dialog) {
    if (dialog.busy || dialog.ran) return;
    bool generation = SendDlgItemMessageW(dialog.window, Mode, CB_GETCURSEL, 0, 0) == 1;
    Job input = dialog.input;
    input.instruction = text(dialog.window, Prompt);
    std::vector<Upload> uploads;
    std::string promptText;
    if (generation) {
        if (input.instruction.find_first_not_of(L" \r\n\t") == std::wstring::npos) throw Error("请输入对比绘制提示词");
        promptText = "Create one image from the user prompt. Uploaded images are STYLE references only, "
            "not character or composition references.\nUser prompt:\n" + utf8(input.instruction);
    } else {
        if (input.source.empty() || input.character.empty()) throw Error("图片编辑对比需要主界面的原始表情及人物参考");
        uploads.push_back({"source", png(limited(decode(input.source).frames.front().image))});
        uploads.push_back({"identity", png(limited(cropped(decode(input.character).frames.front().image, input.crop)))});
        promptText = prompt(input, 0, 1, input.styles.size(), false);
    }
    if (input.styles.size() > 8) throw Error("最多支持 8 张画风参考");
    for (const auto& path : input.styles) uploads.push_back({"style", png(limited(decode(path).frames.front().image))});
    for (auto index : dialog.channels) {
        auto provider = channelProvider(input.provider, dialog.options.channels[index]);
        if (generation && uploads.empty()) generationBody(provider, promptText);
        else multipart(provider, promptText, uploads);
    }
    PWSTR pictures{}; fs::path directory;
    check(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures), "Comparison output directory");
    directory = pictures; CoTaskMemFree(pictures);
    GUID guid{}; check(CoCreateGuid(&guid), "Comparison ID");
    wchar_t id[48]{}; StringFromGUID2(guid, id, 48);
    dialog.output = directory / (L"MemeStudio-compare-" + std::wstring(id + 1, 36));
    fs::create_directories(dialog.output);
    dialog.cancel.reset(); dialog.busy = true; dialog.ran = true;
    for (int control : {Run, Mode, Prompt}) EnableWindow(GetDlgItem(dialog.window, control), FALSE);
    EnableWindow(GetDlgItem(dialog.window, Stop), TRUE);
    EnableWindow(GetDlgItem(dialog.window, Folder), TRUE);
    SetDlgItemTextW(dialog.window, Summary, L"正在对比；各渠道一次请求，不自动灾备或重试。取消不能撤回已发送请求。");
    dialog.worker = std::thread([&dialog, input, generation, uploads = std::move(uploads), promptText] {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string failure;
        try {
            check(initialized, "Comparison COM");
            for (int i = 0; i < int(dialog.channels.size()); ++i) {
                dialog.cancel.verify();
                auto provider = channelProvider(input.provider, dialog.options.channels[dialog.channels[i]]);
                const auto& channel = dialog.options.channels[dialog.channels[i]];
                auto result = std::make_unique<Result>(); result->index = i;
                result->row = {{"index", i}, {"name", routingRedact(channel.name, dialog.options)},
                    {"model", provider.model}, {"estimated_unit_cost", channel.unitCost}};
                auto start = std::chrono::steady_clock::now();
                bool stop = false;
                try {
                    auto image = generation ? generate(provider, promptText, uploads, dialog.cancel) :
                        edit(provider, promptText, uploads, dialog.cancel);
                    dialog.cancel.verify();
                    auto name = std::to_wstring(i + 1) + L".png";
                    savePng(dialog.output / name, image);
                    result->row["output"] = utf8(name); result->row["status"] = "success";
                    result->image = limited(image, 1024);
                } catch (const std::exception& error) {
                    result->row["status"] = dialog.cancel.stopped ? "cancelled" : "failed";
                    result->row["error"] = routingRedact(error.what(), dialog.options);
                    // Unknown outcomes stop comparison as well as regular routing.
                    stop = dialog.cancel.stopped || (dynamic_cast<const ApiHttpError*>(&error) == nullptr &&
                        dynamic_cast<const ApiConnectionError*>(&error) == nullptr);
                }
                result->row["milliseconds"] =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (PostMessageW(dialog.window, ResultMessage, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
                if (stop) break;
            }
        } catch (const std::exception& error) { failure = routingRedact(error.what(), dialog.options); }
        if (SUCCEEDED(initialized)) CoUninitialize();
        auto done = std::make_unique<std::string>(std::move(failure));
        if (PostMessageW(dialog.window, FinishMessage, 0, reinterpret_cast<LPARAM>(done.get()))) done.release();
    });
}
INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<Dialog*>(lparam); dialog->window = window;
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            SetWindowTextW(window, L"API 效果对比");
            RECT rect{0, 0, 960, 706};
            AdjustWindowRectEx(&rect, DWORD(GetWindowLongW(window, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(window, GWL_EXSTYLE)));
            RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            SetWindowPos(window, nullptr, std::max(0L, (area.right - rect.right + rect.left) / 2),
                std::max(0L, (area.bottom - rect.bottom + rect.top) / 2),
                rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
            dialog->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
                auto control = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, label,
                    WS_CHILD | WS_VISIBLE | style, x, y, w, h, window, reinterpret_cast<HMENU>(INT_PTR(id)), dialog->instance, nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font), TRUE); return control;
            };
            auto mode = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, Mode, 16, 12, 330, 180);
            for (auto label : {L"相同首帧 + 人物 + 画风参考", L"相同提示词 + 可选画风参考"})
                SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(mode, CB_SETCURSEL, dialog->input.source.empty() || dialog->input.character.empty() ? 1 : 0, 0);
            add(L"STATIC", L"附加要求 / 绘制提示词", 0, 0, 366, 16, 578, 24);
            add(L"EDIT", dialog->input.instruction.c_str(), WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                Prompt, 16, 50, 928, 70);
            dialog->list = add(WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                Rows, 16, 132, 928, 144);
            ListView_SetExtendedListViewStyle(dialog->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* titles[]{L"渠道", L"模型", L"状态 / 错误", L"耗时 ms", L"估算单价"};
            const int widths[]{190, 160, 336, 110, 110};
            for (int i = 0; i < 5; ++i) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(titles[i]); column.cx = widths[i];
                ListView_InsertColumn(dialog->list, i, &column);
            }
            for (int i = 0; i < int(dialog->channels.size()); ++i) {
                const auto& channel = dialog->options.channels[dialog->channels[i]];
                auto name = wide(channel.name);
                LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = name.data();
                ListView_InsertItem(dialog->list, &item);
                cell(dialog->list, i, 1, wide(channel.model.empty() ? dialog->input.provider.model : channel.model));
                cell(dialog->list, i, 2, L"未发送"); cell(dialog->list, i, 4, std::to_wstring(channel.unitCost));
            }
            for (int side = 0; side < 2; ++side) {
                auto select = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, side ? Right : Left,
                    side ? 488 : 16, 288, 456, 220);
                for (auto index : dialog->channels) {
                    auto name = wide(dialog->options.channels[index].name);
                    SendMessageW(select, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                }
                SendMessageW(select, CB_SETCURSEL, std::min(size_t(side), dialog->channels.size() - 1), 0);
                add(L"STATIC", L"", SS_OWNERDRAW, side ? ViewRight : ViewLeft, side ? 488 : 16, 330, 456, 270);
            }
            auto summary = L"将发送 " + std::to_wstring(dialog->channels.size()) +
                L" 次独立请求，可能逐项计费。相同 API / 模型只比较一次；GIF 仅取首帧。";
            add(L"STATIC", summary.c_str(), 0, Summary, 16, 612, 928, 40);
            add(L"BUTTON", (L"开始对比（" + std::to_wstring(dialog->channels.size()) + L" 次）").c_str(),
                WS_TABSTOP, Run, 16, 662, 228, 30);
            add(L"BUTTON", L"停止", WS_TABSTOP, Stop, 260, 662, 108, 30);
            add(L"BUTTON", L"结果目录", WS_TABSTOP, Folder, 652, 662, 140, 30);
            add(L"BUTTON", L"关闭", WS_TABSTOP, IDCANCEL, 808, 662, 136, 30);
            EnableWindow(GetDlgItem(window, Stop), FALSE); EnableWindow(GetDlgItem(window, Folder), FALSE);
            if (dialog->capture) PostMessageW(window, WM_APP + 53, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
        if (message == WM_APP + 53 && dialog->capture) {
            if (SendDlgItemMessageW(window, Mode, CB_GETCURSEL, 0, 0) == CB_ERR ||
                SendDlgItemMessageW(window, Left, CB_GETCURSEL, 0, 0) != 0 ||
                SendDlgItemMessageW(window, Right, CB_GETCURSEL, 0, 0) !=
                    LRESULT(std::min(size_t(1), dialog->channels.size() - 1)))
                throw Error("Comparison selectors were not initialized");
            dialog->images[0] = limited(decode(dialog->input.source).frames.front().image);
            if (dialog->images.size() > 1)
                dialog->images[1] = limited(decode(dialog->input.character).frames.front().image);
            if (dialog->snapshot) dialog->snapshot(window);
            EndDialog(window, IDOK); return TRUE;
        }
        if (message == WM_DRAWITEM && (wparam == ViewLeft || wparam == ViewRight)) {
            auto index = SendDlgItemMessageW(window, wparam == ViewLeft ? Left : Right, CB_GETCURSEL, 0, 0);
            drawImage(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam),
                index >= 0 && size_t(index) < dialog->images.size() ? dialog->images[index] : Image{});
            return TRUE;
        }
        if (message == ResultMessage) {
            std::unique_ptr<Result> result(reinterpret_cast<Result*>(lparam));
            dialog->images.at(result->index) = std::move(result->image);
            cell(dialog->list, result->index, 2, result->row["status"] == "success" ? L"成功" :
                wide(result->row.value("error", "失败")));
            cell(dialog->list, result->index, 3, std::to_wstring(int(result->row["milliseconds"].get<double>())));
            dialog->report.push_back(result->row);
            writeJson(dialog->output / L"report.json", {{"results", dialog->report}, {"requested", dialog->channels.size()},
                {"cost_is_estimated", true}});
            InvalidateRect(GetDlgItem(window, ViewLeft), nullptr, TRUE);
            InvalidateRect(GetDlgItem(window, ViewRight), nullptr, TRUE); return TRUE;
        }
        if (message == FinishMessage) {
            std::unique_ptr<std::string> failure(reinterpret_cast<std::string*>(lparam));
            if (dialog->worker.joinable()) dialog->worker.join();
            dialog->busy = false; EnableWindow(GetDlgItem(window, Stop), FALSE);
            for (int i = int(dialog->report.size()); i < int(dialog->channels.size()); ++i)
                cell(dialog->list, i, 2, L"未发送");
            SetDlgItemTextW(window, Summary, failure->empty() ?
                L"对比结束。图像与 report.json 已保存在结果目录；各结果来自独立生成，随机性可能影响比较。" :
                wide(*failure).c_str());
            if (dialog->closing) EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            if (dialog->busy) { dialog->closing = true; dialog->cancel.stop(); }
            else EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case Run: if (!dialog->capture) start(*dialog); return TRUE;
        case Stop: dialog->cancel.stop(); return TRUE;
        case Folder: ShellExecuteW(window, L"open", dialog->output.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return TRUE;
        case Left: case Right:
            InvalidateRect(GetDlgItem(window, LOWORD(wparam) == Left ? ViewLeft : ViewRight), nullptr, TRUE); return TRUE;
        }
    } catch (const std::exception& error) {
        if (dialog && dialog->capture) {
            dialog->failure = error.what(); EndDialog(window, IDABORT); return TRUE;
        }
        MessageBoxW(window, wide(dialog ? routingRedact(error.what(), dialog->options) : error.what()).c_str(),
            L"API 效果对比", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
}
void showApiComparison(HINSTANCE instance, HWND owner, const RoutingOptions& options, const Job& input,
    bool capture, const std::function<void(HWND)>& snapshot) {
    validateRouting(options, input.provider);
    Dialog dialog; dialog.instance = instance; dialog.options = options; dialog.input = input;
    dialog.capture = capture; dialog.snapshot = snapshot;
    dialog.channels = comparisonChannels(options, input.provider);
    if (dialog.channels.empty()) throw Error("请至少将一个启用渠道加入效果对比");
    dialog.images.resize(dialog.channels.size());
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    resource.dialog.cx = 600; resource.dialog.cy = 450;
    if (DialogBoxIndirectParamW(instance, &resource.dialog, owner, procedure, reinterpret_cast<LPARAM>(&dialog)) == -1)
        throw Error("无法打开 API 效果对比");
    if (!dialog.failure.empty()) throw Error(dialog.failure);
}
}
