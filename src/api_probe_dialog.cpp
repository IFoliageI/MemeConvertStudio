#include "api_probe.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <thread>

namespace studio {
namespace {
enum { Endpoint = 4100, Cases, Detail, Custom, AddCases, Run, Stop, Export, Summary, Supplier, ModelLabel };
constexpr UINT RowMessage = WM_APP + 31, DoneMessage = WM_APP + 32;
struct Dialog {
    HINSTANCE instance{};
    HWND window{}, list{};
    HFONT font{};
    Provider provider;
    std::vector<ProbeTarget> targets;
    fs::path source, character;
    Crop crop;
    std::vector<ProbeCase> cases;
    std::vector<int> runRows;
    Json report;
    Cancel cancel;
    std::thread worker;
    bool busy = false, closing = false, filling = false, capture = false;
    std::string failure;
    std::function<void(HWND)> snapshot;
    ~Dialog() { cancel.stop(); if (worker.joinable()) worker.join(); if (font) DeleteObject(font); }
};
std::wstring text(HWND window, int id) {
    auto item = GetDlgItem(window, id);
    int size = GetWindowTextLengthW(item);
    std::wstring result(size + 1, L'\0');
    GetWindowTextW(item, result.data(), size + 1); result.resize(size);
    return result;
}
void cell(HWND list, int row, int column, const std::wstring& value) {
    ListView_SetItemText(list, row, column, const_cast<wchar_t*>(value.c_str()));
}
bool generation(const Dialog& dialog) {
    return SendDlgItemMessageW(dialog.window, Endpoint, CB_GETCURSEL, 0, 0) == 1;
}
const wchar_t* outcomeName(const std::string& outcome) {
    if (outcome == "accepted") return L"已接受 / 语义待验证";
    if (outcome == "rejected") return L"参数被拒绝";
    if (outcome == "request_rejected") return L"请求被拒绝 / 原因待查";
    if (outcome == "ignored") return L"参数未遵循";
    if (outcome == "cancelled") return L"已取消";
    if (outcome == "skipped") return L"未发送";
    if (outcome == "running") return L"请求中…";
    return L"无法判断";
}
void updateSummary(Dialog& dialog) {
    int count = 1;
    int outputMin = 0, outputMax = 0, unknown = 0, partial = 0;
    for (int i = 0; i < static_cast<int>(dialog.cases.size()); ++i) {
        if (i > 0 && !ListView_GetCheckState(dialog.list, i)) continue;
        auto usage = estimateProbeUsage(dialog.cases[i].provider);
        if (usage["output_min"].is_null()) ++unknown;
        else { outputMin += usage["output_min"].get<int>(); outputMax += usage["output_max"].get<int>(); }
        partial += usage["partial_image_overhead"].get<int>();
    }
    for (int i = 1; i < static_cast<int>(dialog.cases.size()); ++i)
        if (ListView_GetCheckState(dialog.list, i)) ++count;
    static const std::string prompt = "Create a simple blue square on a white background. Return one image without text.";
    int perRequest = estimateProbeTokens(prompt);
    auto summary = L"预计 " + std::to_wstring(count) + L" 次请求 · 提示文本约 " +
        std::to_wstring(perRequest) + L" tokens/次，合计约 " +
        std::to_wstring(perRequest * count) +
        L"；输出 " + (unknown ? L"待定（" + std::to_wstring(unknown) + L" 项）" :
            std::to_wstring(outputMin) + L"–" + std::to_wstring(outputMax)) +
        (partial ? L"；局部图 ≤" + std::to_wstring(partial) : L"") + L"；图像输入待定";
    SetDlgItemTextW(dialog.window, Summary, summary.c_str());
    SetDlgItemTextW(dialog.window, Run, (L"开始测试（" + std::to_wstring(count) + L" 次）").c_str());
}
void fillCases(Dialog& dialog) {
    dialog.filling = true;
    ListView_DeleteAllItems(dialog.list);
    for (int i = 0; i < static_cast<int>(dialog.cases.size()); ++i) {
        const auto& item = dialog.cases[i];
        auto name = wide(item.name);
        LVITEMW row{}; row.mask = LVIF_TEXT; row.iItem = i; row.pszText = name.data();
        ListView_InsertItem(dialog.list, &row);
        auto parameters = probeCaseParameters(item.provider).dump();
        if (!generation(dialog)) parameters += "; " + item.provider.imageField;
        cell(dialog.list, i, 1, wide(parameters));
        cell(dialog.list, i, 2, L"待测试");
        cell(dialog.list, i, 4, probeTokenLabel(item.provider));
        ListView_SetCheckState(dialog.list, i, item.selected);
    }
    dialog.filling = false;
    updateSummary(dialog);
}
void enabled(Dialog& dialog, bool idle) {
    for (int id : {Endpoint, Cases, Custom, AddCases, Run, Supplier}) EnableWindow(GetDlgItem(dialog.window, id), idle);
    EnableWindow(GetDlgItem(dialog.window, Stop), !idle);
    EnableWindow(GetDlgItem(dialog.window, Export), idle && !dialog.report.is_null());
}
void start(Dialog& dialog) {
    if (dialog.busy || dialog.capture) return;
    ProbeOptions options; options.provider = dialog.provider; options.generation = generation(dialog);
    if (!options.generation) options.images = probeImages(dialog.source, dialog.character, dialog.crop);
    options.cases = dialog.cases;
    dialog.runRows.clear();
    for (int i = 0; i < static_cast<int>(options.cases.size()); ++i) {
        auto& item = options.cases[i];
        item.selected = i == 0 || ListView_GetCheckState(dialog.list, i);
        if (item.selected) {
            // Validate every selected shape before the first paid request.
            generationBody(item.provider, "diagnostic");
            if (!options.generation) multipart(item.provider, "diagnostic", options.images);
            dialog.runRows.push_back(i); cell(dialog.list, i, 2, L"等待中"); cell(dialog.list, i, 3, L"");
        }
    }
    auto confirm = L"将发送 " + std::to_wstring(dialog.runRows.size()) +
        L" 次实际图像接口请求，每项最多一次。图像 API 可能按请求计费；"
        L"按取消可关闭此次测试，开始后停止操作不能撤回已发送请求。\n\n" +
        text(dialog.window, Summary) + L"\n\nToken 估算不是费用上限。继续？";
    if (MessageBoxW(dialog.window, confirm.c_str(), L"确认测试费用", MB_YESNO | MB_ICONWARNING |
        MB_DEFBUTTON2) != IDYES) return;
    dialog.report = nullptr;
    dialog.cancel.reset(); dialog.busy = true; dialog.closing = false;
    enabled(dialog, false);
    SetDlgItemTextW(dialog.window, Detail, L"正在发送请求；取消会中止网络等待，但供应商可能已开始处理并计费。");
    dialog.worker = std::thread([&dialog, options = std::move(options)] {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto completion = std::make_unique<Json>();
        try {
            check(initialized, "Parameter probe COM");
            *completion = runParameterProbe(options, dialog.cancel, [&](const Json& row) {
                auto event = std::make_unique<Json>(row);
                if (PostMessageW(dialog.window, RowMessage, 0, reinterpret_cast<LPARAM>(event.get()))) event.release();
            });
        } catch (const std::exception& error) {
            *completion = {{"error", error.what()}, {"results", Json::array()}};
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (PostMessageW(dialog.window, DoneMessage, 0, reinterpret_cast<LPARAM>(completion.get()))) completion.release();
    });
}
void exportReport(Dialog& dialog) {
    if (dialog.report.is_null() || dialog.busy) return;
    wchar_t filename[32768] = L"API-parameter-report.json";
    OPENFILENAMEW picker{sizeof(picker)};
    picker.hwndOwner = dialog.window; picker.lpstrFile = filename; picker.nMaxFile = 32768;
    picker.lpstrFilter = L"JSON 报告\0*.json\0\0"; picker.lpstrDefExt = L"json";
    picker.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&picker)) writeJson(filename, dialog.report);
}
INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<Dialog*>(lparam); dialog->window = window;
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            SetWindowTextW(window, L"API 参数支持测试");
            RECT bounds{0, 0, 960, 694};
            AdjustWindowRectEx(&bounds, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)));
            RECT owner{}; GetWindowRect(GetWindow(window, GW_OWNER), &owner);
            int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
            SetWindowPos(window, nullptr, std::max(0L, owner.left + (owner.right - owner.left - width) / 2),
                std::max(0L, owner.top + (owner.bottom - owner.top - height) / 2), width, height, SWP_NOZORDER);
            dialog->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            auto add = [&](const wchar_t* type, const std::wstring& label, DWORD style, int id,
                int x, int y, int w, int h) {
                auto control = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
                    type, label.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                    reinterpret_cast<HMENU>(INT_PTR(id)), dialog->instance, nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font), TRUE);
                return control;
            };
            add(L"STATIC", L"模型：" + wide(dialog->provider.model), SS_ENDELLIPSIS | SS_NOPREFIX, ModelLabel, 16, 16, 582, 24);
            auto endpoint = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, Endpoint, 614, 12, 330, 180);
            for (auto label : {L"图片编辑 /images/edits", L"纯文本生成 /images/generations"})
                SendMessageW(endpoint, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(endpoint, CB_SETCURSEL, 0, 0);
            auto input = dialog->source.empty() && dialog->character.empty() ?
                L"编辑输入：内置 64×64 诊断图" :
                L"编辑输入：当前已选择的源图和人物图（GIF 首帧，沿用人物裁剪）";
            add(L"STATIC", input, SS_ENDELLIPSIS | SS_NOPREFIX, 0, 16, 48, 582, 24);
            auto supplier = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                Supplier, 614, 46, 330, 220);
            if (dialog->targets.empty()) SendMessageW(supplier, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"主界面供应商（固定测试目标）"));
            else for (const auto& target : dialog->targets) {
                auto label = L"供应商池：" + wide(target.name);
                SendMessageW(supplier, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(supplier, CB_SETCURSEL, 0, 0);
            dialog->list = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                Cases, 16, 84, 928, 256);
            ListView_SetExtendedListViewStyle(dialog->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* titles[]{L"测试项", L"发送参数", L"判定", L"HTTP", L"预估 Tokens（不含图片输入）"};
            const int widths[]{220, 260, 150, 60, 280};
            for (int i = 0; i < 5; ++i) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(titles[i]); column.cx = widths[i];
                ListView_InsertColumn(dialog->list, i, &column);
            }
            add(L"EDIT", L"基线固定执行。当前配置已勾选，其他候选值可按需选择。", WS_TABSTOP | ES_READONLY |
                ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, Detail, 16, 352, 928, 82);
            add(L"STATIC", L"附加组合（JSON 数组）", 0, 0, 16, 446, 740, 24);
            add(L"EDIT", L"[{\"name\":\"组合示例\",\"parameters\":{\"quality\":\"low\",\"output_format\":\"jpeg\",\"output_compression\":80}}]",
                WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, Custom, 16, 474, 756, 76);
            add(L"BUTTON", L"添加组合", WS_TABSTOP, AddCases, 788, 474, 156, 32);
            SendDlgItemMessageW(window, Custom, EM_SETLIMITTEXT, 16000, 0);
            add(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, Summary, 16, 564, 928, 24);
            add(L"STATIC", L"文本采用字符启发式；输出只对已知 GPT Image 旧版规格估算，其他模型待定；图片输入不计入。实际用量以供应商记录为准。",
                0, 0, 16, 594, 928, 42);
            add(L"BUTTON", L"开始测试", WS_TABSTOP | BS_DEFPUSHBUTTON, Run, 16, 646, 260, 32);
            add(L"BUTTON", L"取消测试", WS_TABSTOP, Stop, 288, 646, 140, 32);
            add(L"BUTTON", L"导出报告…", WS_TABSTOP, Export, 632, 646, 152, 32);
            add(L"BUTTON", L"关闭", WS_TABSTOP, IDCANCEL, 796, 646, 148, 32);
            dialog->cases = parameterProbeCases(dialog->provider, false); fillCases(*dialog); enabled(*dialog, true);
            if (dialog->capture) PostMessageW(window, WM_APP + 33, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
        if (message == WM_APP + 33 && dialog->capture) {
            if (dialog->cases.size() < 20 || !ListView_GetCheckState(dialog->list, 0)) throw Error("Probe plan missing candidates");
            if (text(window, Summary).find(L"tokens/次") == std::wstring::npos ||
                Header_GetItemCount(ListView_GetHeader(dialog->list)) != 5)
                throw Error("Probe token estimate controls missing");
            SendMessageW(window, WM_COMMAND, AddCases, 0);
            if (dialog->cases.back().name != "组合示例") throw Error("Custom probe combination missing");
            if (dialog->snapshot) dialog->snapshot(window);
            SendDlgItemMessageW(window, Endpoint, CB_SETCURSEL, 1, 0);
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(Endpoint, CBN_SELCHANGE), 0);
            if (!generation(*dialog) || dialog->cases.back().name.find("image field") != std::string::npos)
                throw Error("Generation probe mode has edit-only controls");
            if (dialog->targets.size() > 1) {
                SendDlgItemMessageW(window, Supplier, CB_SETCURSEL, 1, 0);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(Supplier, CBN_SELCHANGE), 0);
                if (dialog->provider.model != dialog->targets[1].provider.model ||
                    !estimateProbeUsage(dialog->provider)["output_min"].is_null())
                    throw Error("Fixed supplier target selection failed");
            }
            EndDialog(window, IDOK); return TRUE;
        }
        if (message == RowMessage) {
            std::unique_ptr<Json> row(reinterpret_cast<Json*>(lparam));
            int index = row->at("index").get<int>();
            if (index >= 0 && index < static_cast<int>(dialog->runRows.size())) {
                int position = dialog->runRows[index];
                cell(dialog->list, position, 2, outcomeName(row->value("outcome", "")));
                auto status = row->value("status", 0);
                cell(dialog->list, position, 3, status ? std::to_wstring(status) : L"-");
                ListView_EnsureVisible(dialog->list, position, FALSE);
            }
            SetDlgItemTextW(window, Detail, wide(row->dump(2)).c_str()); return TRUE;
        }
        if (message == DoneMessage) {
            std::unique_ptr<Json> report(reinterpret_cast<Json*>(lparam));
            if (dialog->worker.joinable()) dialog->worker.join();
            dialog->busy = false; dialog->report = std::move(*report); enabled(*dialog, true);
            SetDlgItemTextW(window, Summary, dialog->cancel.stopped ? L"已取消；已发送请求可能仍被计费。" : L"测试结束；结果可选择查看并导出。");
            if (dialog->report.contains("error")) SetDlgItemTextW(window, Detail, wide(dialog->report["error"].get<std::string>()).c_str());
            if (dialog->closing) EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == Cases && !dialog->filling) {
            if (reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED && !dialog->busy) {
                if (!ListView_GetCheckState(dialog->list, 0)) {
                    dialog->filling = true; ListView_SetCheckState(dialog->list, 0, TRUE); dialog->filling = false;
                }
                updateSummary(*dialog);
            }
            int selected = ListView_GetNextItem(dialog->list, -1, LVNI_SELECTED);
            if (!dialog->busy && selected >= 0 && selected < static_cast<int>(dialog->cases.size())) {
                std::wstring value = wide(dialog->cases[selected].name) + L"\r\n" + wide(probeCaseParameters(dialog->cases[selected].provider).dump(2));
                if (dialog->report.is_object()) for (const auto& row : dialog->report.value("results", Json::array()))
                    if (row.value("name", "") == dialog->cases[selected].name || (selected == 0 && row.value("index", -1) == 0))
                        value += L"\r\n" + wide(row.dump(2));
                SetDlgItemTextW(window, Detail, value.c_str());
            }
            return FALSE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            if (dialog->busy) { dialog->closing = true; dialog->cancel.stop(); }
            else EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case Supplier:
            if (HIWORD(wparam) == CBN_SELCHANGE && !dialog->busy && !dialog->targets.empty()) {
                int selected = int(SendDlgItemMessageW(window, Supplier, CB_GETCURSEL, 0, 0));
                if (selected >= 0 && selected < int(dialog->targets.size())) {
                    dialog->provider = dialog->targets[selected].provider;
                    SetDlgItemTextW(window, ModelLabel, (L"模型：" + wide(dialog->provider.model)).c_str());
                    dialog->report = nullptr;
                    dialog->cases = parameterProbeCases(dialog->provider, generation(*dialog));
                    fillCases(*dialog); enabled(*dialog, true);
                }
            }
            return TRUE;
        case Endpoint:
            if (HIWORD(wparam) == CBN_SELCHANGE && !dialog->busy) {
                dialog->report = nullptr;
                dialog->cases = parameterProbeCases(dialog->provider, generation(*dialog));
                fillCases(*dialog); enabled(*dialog, true);
            }
            return TRUE;
        case AddCases: {
            if (dialog->busy) return TRUE;
            auto values = Json::parse(utf8(text(window, Custom)));
            if (!values.is_array() || values.empty() || values.size() + dialog->cases.size() > 64)
                throw Error("输入须为非空 JSON 数组，合计最多 64 项");
            std::vector<ProbeCase> pending;
            for (const auto& value : values) pending.push_back(customProbeCase(dialog->provider, value));
            for (int i = 0; i < static_cast<int>(dialog->cases.size()); ++i)
                dialog->cases[i].selected = ListView_GetCheckState(dialog->list, i) != 0;
            dialog->cases.insert(dialog->cases.end(), pending.begin(), pending.end());
            fillCases(*dialog); return TRUE;
        }
        case Run: start(*dialog); return TRUE;
        case Stop: dialog->cancel.stop(); SetDlgItemTextW(window, Summary, L"正在停止；不会自动重试。"); return TRUE;
        case Export: exportReport(*dialog); return TRUE;
        }
    } catch (const std::exception& error) {
        if (dialog && dialog->capture) { dialog->failure = error.what(); EndDialog(window, IDABORT); }
        else MessageBoxW(window, wide(error.what()).c_str(), L"API 参数测试", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
}
void showParameterProbe(HINSTANCE instance, HWND owner, const Provider& provider,
    const fs::path& source, const fs::path& character, const Crop& crop, bool capture,
    const std::function<void(HWND)>& snapshot, const std::vector<ProbeTarget>& targets) {
    Dialog dialog; dialog.instance = instance; dialog.provider = provider;
    dialog.targets = targets;
    if (!targets.empty()) dialog.provider = targets.front().provider;
    dialog.source = source; dialog.character = character; dialog.crop = crop;
    dialog.capture = capture; dialog.snapshot = snapshot;
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    resource.dialog.cx = 600; resource.dialog.cy = 440;
    if (DialogBoxIndirectParamW(instance, &resource.dialog, owner, procedure, reinterpret_cast<LPARAM>(&dialog)) == -1)
        throw Error("无法打开 API 参数测试窗口");
    if (!dialog.failure.empty()) throw Error(dialog.failure);
}
}
