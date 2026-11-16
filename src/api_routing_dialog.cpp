#include "api_routing.hpp"
#include "panels.hpp"
#include <commctrl.h>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <thread>
#include <set>

namespace studio {
namespace {
enum { Channels = 4300, Name, Url, Model, Key, Weight, Priority, Cost, Active, Compare,
    Add, Update, Remove, CopyMain, Enabled, Spending, Failover, Service, Remember, Cooldown, Comparison, Stats,
    Models, Fetch, SelectAll, SelectNone, ModelStatus };
constexpr UINT ModelsDone = WM_APP + 61;
struct ModelResult { std::vector<std::string> models; std::string error; };
struct Dialog {
    HINSTANCE instance{};
    HWND window{}, list{};
    HFONT font{};
    RoutingOptions options;
    Job input;
    bool accepted = false, capture = false;
    std::string failure;
    std::function<void(HWND)> snapshot;
    ApiChannel draft;
    Cancel cancel;
    std::thread worker;
    bool fetching = false, filling = false, closing = false;
    ~Dialog() { cancel.stop(); if (worker.joinable()) worker.join(); if (font) DeleteObject(font); }
};
std::wstring text(HWND window, int id) {
    auto control = GetDlgItem(window, id);
    std::wstring value(GetWindowTextLengthW(control) + 1, L'\0');
    GetWindowTextW(control, value.data(), int(value.size())); value.pop_back(); return value;
}
bool checked(HWND window, int id) { return IsDlgButtonChecked(window, id) == BST_CHECKED; }
void checkBox(HWND window, int id, bool value) { CheckDlgButton(window, id, value ? BST_CHECKED : BST_UNCHECKED); }
void fillModels(Dialog& dialog) {
    dialog.filling = true;
    auto list = GetDlgItem(dialog.window, Models);
    ListView_DeleteAllItems(list);
    for (int i = 0; i < int(dialog.draft.availableModels.size()); ++i) {
        auto label = wide(dialog.draft.availableModels[i]);
        LVITEMW row{}; row.mask = LVIF_TEXT; row.iItem = i; row.pszText = label.data();
        ListView_InsertItem(list, &row);
        ListView_SetCheckState(list, i, std::find(dialog.draft.enabledModels.begin(),
            dialog.draft.enabledModels.end(), dialog.draft.availableModels[i]) != dialog.draft.enabledModels.end());
    }
    auto status = L"已启用 " + std::to_wstring(dialog.draft.enabledModels.size()) + L" / " +
        std::to_wstring(dialog.draft.availableModels.size()) + L" 个模型";
    SetDlgItemTextW(dialog.window, ModelStatus, status.c_str());
    dialog.filling = false;
}
void storeModels(Dialog& dialog) {
    auto& draft = dialog.draft;
    draft.enabledModels.clear();
    for (int i = 0; i < int(draft.availableModels.size()); ++i)
        if (ListView_GetCheckState(GetDlgItem(dialog.window, Models), i))
            draft.enabledModels.push_back(draft.availableModels[i]);
    int selected = ListView_GetNextItem(dialog.list, -1, LVNI_SELECTED);
    if (selected >= 0) {
        auto& channel = dialog.options.channels.at(selected);
        if (channel.baseUrl == text(dialog.window, Url) && channel.key == text(dialog.window, Key)) {
            channel.modelsConfigured = draft.modelsConfigured;
            channel.availableModels = draft.availableModels; channel.enabledModels = draft.enabledModels;
            auto label = channel.modelsConfigured ? std::to_wstring(channel.enabledModels.size()) + L" 个模型" : wide(channel.model);
            ListView_SetItemText(dialog.list, selected, 3, label.data());
        }
    }
    auto label = L"已启用 " + std::to_wstring(draft.enabledModels.size()) + L" / " +
        std::to_wstring(draft.availableModels.size()) + L" 个模型";
    SetDlgItemTextW(dialog.window, ModelStatus, label.c_str());
}
void fetchingControls(Dialog& dialog) {
    for (int id : std::initializer_list<int>{Channels, Name, Url, Model, Key, Add, Update, Remove, CopyMain, Fetch, Models,
        SelectAll, SelectNone, IDOK, Comparison})
        EnableWindow(GetDlgItem(dialog.window, id), !dialog.fetching);
}
void fetch(Dialog& dialog) {
    if (dialog.fetching) return;
    Provider provider = dialog.input.provider;
    provider.baseUrl = text(dialog.window, Url); provider.key = text(dialog.window, Key);
    provider.router.reset(); provider.gateway.reset();
    if (provider.key.empty()) throw Error("请填写当前 API 的 Key");
    modelsUrl(provider.baseUrl);
    if (dialog.worker.joinable()) dialog.worker.join();
    dialog.cancel.reset(); dialog.fetching = true; fetchingControls(dialog);
    SetDlgItemTextW(dialog.window, ModelStatus, L"正在获取当前 API 的模型…");
    dialog.worker = std::thread([&dialog, provider] {
        auto result = std::make_unique<ModelResult>();
        try { result->models = listModels(provider, dialog.cancel); }
        catch (const std::exception& error) {
            result->error = error.what();
            auto secret = utf8(provider.key);
            for (size_t pos = 0; !secret.empty() && (pos = result->error.find(secret, pos)) != std::string::npos;)
                result->error.replace(pos, secret.size(), "[REDACTED]");
        }
        if (PostMessageW(dialog.window, ModelsDone, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    });
}
double number(HWND window, int id, bool integer) {
    auto value = text(window, id);
    size_t end = 0;
    double result;
    try { result = std::stod(value, &end); }
    catch (...) { throw Error("请输入有效数值"); }
    if (end != value.size() || !std::isfinite(result) || (integer && result != std::floor(result)) ||
        result < 0 || result > 1000000) throw Error("数值无效或超出范围");
    return result;
}
void fill(Dialog& dialog, int selected = -1) {
    ListView_DeleteAllItems(dialog.list);
    for (int i = 0; i < int(dialog.options.channels.size()); ++i) {
        const auto& channel = dialog.options.channels[i];
        auto label = wide(channel.name);
        LVITEMW row{}; row.mask = LVIF_TEXT; row.iItem = i; row.pszText = label.data();
        ListView_InsertItem(dialog.list, &row);
        std::wstring values[]{channel.enabled ? L"启用" : L"停用", channel.baseUrl,
            channel.modelsConfigured ? std::to_wstring(channel.enabledModels.size()) + L" 个模型" :
                wide(channel.model), std::to_wstring(channel.weight), std::to_wstring(channel.priority),
            std::to_wstring(channel.unitCost), channel.compare ? L"是" : L"否"};
        for (int column = 1; column <= 7; ++column)
            ListView_SetItemText(dialog.list, i, column, values[column - 1].data());
    }
    if (selected >= 0) {
        ListView_SetItemState(dialog.list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(dialog.list, selected, FALSE);
    }
}
void select(Dialog& dialog, int index) {
    if (index < 0 || index >= int(dialog.options.channels.size())) return;
    const auto& channel = dialog.options.channels[index];
    auto window = dialog.window;
    SetDlgItemTextW(window, Name, wide(channel.name).c_str());
    SetDlgItemTextW(window, Url, channel.baseUrl.c_str());
    SetDlgItemTextW(window, Model, wide(channel.model).c_str());
    SetDlgItemTextW(window, Key, channel.key.c_str());
    SetDlgItemTextW(window, Weight, std::to_wstring(channel.weight).c_str());
    SetDlgItemTextW(window, Priority, std::to_wstring(channel.priority).c_str());
    SetDlgItemTextW(window, Cost, std::to_wstring(channel.unitCost).c_str());
    checkBox(window, Active, channel.enabled); checkBox(window, Compare, channel.compare);
    dialog.draft = channel; fillModels(dialog);
}
void controls(Dialog& dialog) {
    auto window = dialog.window;
    dialog.options.enabled = checked(window, Enabled); dialog.options.spending = checked(window, Spending);
    dialog.options.failover = checked(window, Failover); dialog.options.serviceFailover = checked(window, Service);
    dialog.options.remember = checked(window, Remember);
    dialog.options.cooldownSeconds = int(number(window, Cooldown, true));
}
void applyChannel(Dialog& dialog, bool update) {
    ApiChannel channel;
    auto window = dialog.window;
    channel.name = utf8(text(window, Name)); channel.baseUrl = text(window, Url);
    channel.model = utf8(text(window, Model)); channel.key = text(window, Key);
    channel.weight = int(number(window, Weight, true)); channel.priority = int(number(window, Priority, true));
    channel.unitCost = number(window, Cost, false);
    channel.enabled = checked(window, Active); channel.compare = checked(window, Compare);
    if (dialog.draft.baseUrl == channel.baseUrl && dialog.draft.key == channel.key) {
        channel.modelsConfigured = dialog.draft.modelsConfigured;
        channel.availableModels = dialog.draft.availableModels; channel.enabledModels = dialog.draft.enabledModels;
    }
    auto pending = dialog.options;
    int index = ListView_GetNextItem(dialog.list, -1, LVNI_SELECTED);
    if (update) {
        if (index < 0) throw Error("请选择需要更新的渠道");
        pending.channels.at(index) = channel;
    } else {
        if (pending.channels.size() >= 64) throw Error("最多 64 个渠道");
        index = int(pending.channels.size()); pending.channels.push_back(channel);
    }
    // Even a disabled row must be valid when edited, but may duplicate a disabled backup.
    auto single = pending; single.channels = {channel}; single.channels[0].enabled = true;
    validateRouting(single, dialog.input.provider);
    if (std::any_of(pending.channels.begin(), pending.channels.end(), [](const auto& item) { return item.enabled; }))
        validateRouting(pending, dialog.input.provider);
    dialog.options = std::move(pending);
    dialog.draft = channel;
    fill(dialog, index);
}
INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<Dialog*>(lparam); dialog->window = window;
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            SetWindowTextW(window, L"API 负载均衡与灾备 · 聚合网关 / 供应商池");
            RECT rect{0, 0, 960, 730};
            AdjustWindowRectEx(&rect, DWORD(GetWindowLongW(window, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(window, GWL_EXSTYLE)));
            RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            SetWindowPos(window, nullptr, std::max(0L, area.left + (area.right - area.left - rect.right + rect.left) / 2),
                std::max(0L, area.top + (area.bottom - area.top - rect.bottom + rect.top) / 2),
                rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
            dialog->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
                auto item = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, label,
                    WS_CHILD | WS_VISIBLE | style, x, y, w, h, window, reinterpret_cast<HMENU>(INT_PTR(id)), dialog->instance, nullptr);
                SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font), TRUE); return item;
            };
            auto tick = [&](int id, const wchar_t* label, int x, int y, int w, bool value) {
                add(L"BUTTON", label, WS_TABSTOP | BS_AUTOCHECKBOX, id, x, y, w, 26); checkBox(window, id, value);
            };
            tick(Enabled, L"启用聚合网关", 16, 12, 190, dialog->options.enabled);
            tick(Spending, L"按估算花销比例", 220, 12, 188, dialog->options.spending);
            tick(Failover, L"启用灾备切换", 430, 12, 170, dialog->options.failover);
            tick(Remember, L"本机加密记住 Keys", 670, 12, 274, dialog->options.remember);
            dialog->list = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                Channels, 16, 50, 928, 206);
            ListView_SetExtendedListViewStyle(dialog->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* titles[]{L"供应商池 / Key", L"状态", L"API 地址", L"模型", L"权重", L"优先级", L"估算单价", L"对比"};
            const int widths[]{142, 56, 248, 145, 62, 64, 116, 66};
            for (int i = 0; i < 8; ++i) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(titles[i]); column.cx = widths[i];
                ListView_InsertColumn(dialog->list, i, &column);
            }
            add(L"STATIC", L"渠道别名", 0, 0, 16, 270, 150, 22);
            add(L"STATIC", L"Base URL", 0, 0, 204, 270, 460, 22);
            add(L"STATIC", L"固定模型（未获取列表时）", 0, 0, 680, 270, 264, 22);
            add(L"EDIT", L"API-1 / Key-1", WS_TABSTOP | ES_AUTOHSCROLL, Name, 16, 298, 172, 28);
            add(L"EDIT", dialog->input.provider.baseUrl.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, Url, 204, 298, 460, 28);
            add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Model, 680, 298, 264, 28);
            add(L"STATIC", L"API Key", 0, 0, 16, 340, 180, 22);
            add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, Key, 16, 368, 420, 28);
            SendDlgItemMessageW(window, Key, EM_SETLIMITTEXT, 8191, 0);
            add(L"STATIC", L"权重", 0, 0, 452, 340, 96, 22);
            add(L"EDIT", L"1", WS_TABSTOP | ES_NUMBER, Weight, 452, 368, 96, 28);
            add(L"STATIC", L"优先级", 0, 0, 564, 340, 96, 22);
            add(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, Priority, 564, 368, 96, 28);
            add(L"STATIC", L"估算单价 / 请求", 0, 0, 676, 340, 160, 22);
            add(L"EDIT", L"1", WS_TABSTOP | ES_AUTOHSCROLL, Cost, 676, 368, 160, 28);
            tick(Active, L"启用", 848, 368, 96, true);
            tick(Compare, L"加入效果对比", 16, 410, 180, true);
            add(L"BUTTON", L"从主界面带入", WS_TABSTOP, CopyMain, 204, 408, 160, 30);
            add(L"BUTTON", L"添加为新渠道", WS_TABSTOP, Add, 396, 408, 172, 30);
            add(L"BUTTON", L"更新选中渠道", WS_TABSTOP, Update, 584, 408, 172, 30);
            add(L"BUTTON", L"删除选中", WS_TABSTOP, Remove, 772, 408, 172, 30);
            add(L"STATIC", L"限流冷却（秒）", 0, 0, 16, 454, 158, 24);
            add(L"EDIT", std::to_wstring(dialog->options.cooldownSeconds).c_str(), WS_TABSTOP | ES_NUMBER, Cooldown, 180, 450, 80, 28);
            tick(Service, L"遇 502 / 503 / 504 也切换（可能重复计费）", 284, 450, 660, dialog->options.serviceFailover);
            add(L"BUTTON", L"获取当前 API 模型", WS_TABSTOP, Fetch, 16, 490, 172, 30);
            add(L"BUTTON", L"全部启用", WS_TABSTOP, SelectAll, 16, 530, 172, 30);
            add(L"BUTTON", L"全部隐藏", WS_TABSTOP, SelectNone, 16, 570, 172, 30);
            auto models = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS,
                Models, 204, 490, 740, 110);
            ListView_SetExtendedListViewStyle(models, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = 710;
            column.pszText = const_cast<wchar_t*>(L"当前 API 模型 · 勾选后加入供应商池");
            ListView_InsertColumn(models, 0, &column);
            add(L"STATIC", L"尚未获取模型", SS_ENDELLIPSIS | SS_NOPREFIX, ModelStatus, 16, 608, 928, 24);
            std::wstring stats = L"统计在生成任务后显示；估算花销仅累计成功请求，不含失败计费、缓存、Token 及实际账单差异。";
            if (dialog->input.provider.router) stats = wide(dialog->input.provider.router->statistics().dump(2));
            add(L"EDIT", stats.c_str(), WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                Stats, 16, 640, 928, 30);
            add(L"BUTTON", L"效果对比…", WS_TABSTOP, Comparison, 16, 686, 172, 30);
            add(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 684, 686, 124, 30);
            add(L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL, 820, 686, 124, 30);
            fill(*dialog);
            if (dialog->capture) PostMessageW(window, WM_APP + 41, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
        if (message == WM_DESTROY) {
            dialog->cancel.stop();
            if (dialog->worker.joinable()) dialog->worker.join();
            MSG pending{};
            while (PeekMessageW(&pending, window, ModelsDone, ModelsDone, PM_REMOVE))
                delete reinterpret_cast<ModelResult*>(pending.lParam);
        }
        if (message == ModelsDone) {
            std::unique_ptr<ModelResult> result(reinterpret_cast<ModelResult*>(lparam));
            if (dialog->worker.joinable()) dialog->worker.join();
            dialog->fetching = false; fetchingControls(*dialog);
            if (dialog->closing) { finishPanel(window, IDCANCEL); return TRUE; }
            if (!result->error.empty()) {
                SetDlgItemTextW(window, ModelStatus, wide(result->error).c_str()); return TRUE;
            }
            auto previous = dialog->draft;
            bool same = previous.baseUrl == text(window, Url) && previous.key == text(window, Key);
            dialog->draft.baseUrl = text(window, Url); dialog->draft.key = text(window, Key);
            dialog->draft.availableModels = std::move(result->models);
            dialog->draft.enabledModels.clear(); dialog->draft.modelsConfigured = true;
            for (const auto& model : dialog->draft.availableModels)
                if (same && previous.modelsConfigured ?
                    std::find(previous.enabledModels.begin(), previous.enabledModels.end(), model) != previous.enabledModels.end() :
                    (model == utf8(text(window, Model)) || model == dialog->input.provider.model))
                    dialog->draft.enabledModels.push_back(model);
            fillModels(*dialog); storeModels(*dialog); return TRUE;
        }
        if (message == WM_APP + 41 && dialog->capture) {
            SendMessageW(window, WM_COMMAND, CopyMain, 0);
            SendMessageW(window, WM_COMMAND, Add, 0);
            if (dialog->options.channels.size() != 1 || dialog->options.channels[0].key != dialog->input.provider.key)
                throw Error("Routing channel editor failed");
            SetDlgItemTextW(window, Key, L"second-key");
            SetDlgItemTextW(window, Name, L"API-1 / Key-2");
            SetDlgItemTextW(window, Weight, L"3");
            SendMessageW(window, WM_COMMAND, Add, 0);
            if (dialog->options.channels.size() != 2 || dialog->options.channels[1].weight != 3)
                throw Error("Routing multi-key weights missing");
            dialog->draft = dialog->options.channels[1];
            dialog->draft.modelsConfigured = true;
            dialog->draft.availableModels = {dialog->input.provider.model, "another-image-model", "hidden-model"};
            dialog->draft.enabledModels = {dialog->input.provider.model, "another-image-model"};
            fillModels(*dialog); storeModels(*dialog);
            if (!dialog->options.channels[1].modelsConfigured || dialog->options.channels[1].enabledModels.size() != 2)
                throw Error("Supplier model selection was not persisted");
            checkBox(window, Enabled, true);
            if (dialog->snapshot) dialog->snapshot(window);
            SendMessageW(window, WM_COMMAND, IDOK, 0); return TRUE;
        }
        if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == Models &&
            reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED && !dialog->filling) {
            storeModels(*dialog); return FALSE;
        }
        if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == Channels &&
            reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) {
            auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED))
                select(*dialog, change->iItem);
            return FALSE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            if (dialog->fetching) { dialog->closing = true; dialog->cancel.stop(); }
            else finishPanel(window, IDCANCEL);
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        if (dialog->fetching) return TRUE;
        if ((LOWORD(wparam) == Url || LOWORD(wparam) == Key) && HIWORD(wparam) == EN_CHANGE &&
            (dialog->draft.baseUrl != text(window, Url) || dialog->draft.key != text(window, Key))) {
            dialog->draft = ApiChannel{};
            dialog->draft.baseUrl = text(window, Url); dialog->draft.key = text(window, Key);
            fillModels(*dialog); return TRUE;
        }
        switch (LOWORD(wparam)) {
        case Fetch: fetch(*dialog); return TRUE;
        case SelectAll: case SelectNone:
            dialog->filling = true;
            for (int i = 0; i < int(dialog->draft.availableModels.size()); ++i)
                ListView_SetCheckState(GetDlgItem(window, Models), i, LOWORD(wparam) == SelectAll);
            dialog->filling = false; storeModels(*dialog); return TRUE;
        case CopyMain:
            SetDlgItemTextW(window, Url, dialog->input.provider.baseUrl.c_str());
            SetDlgItemTextW(window, Model, wide(dialog->input.provider.model).c_str());
            SetDlgItemTextW(window, Key, dialog->input.provider.key.c_str()); return TRUE;
        case Add: applyChannel(*dialog, false); return TRUE;
        case Update: applyChannel(*dialog, true); return TRUE;
        case Remove: {
            int index = ListView_GetNextItem(dialog->list, -1, LVNI_SELECTED);
            if (index >= 0) dialog->options.channels.erase(dialog->options.channels.begin() + index);
            fill(*dialog); return TRUE;
        }
        case Comparison:
            controls(*dialog); validateRouting(dialog->options, dialog->input.provider);
            if (!dialog->capture) showApiComparison(dialog->instance, window, dialog->options, dialog->input);
            return TRUE;
        case IDOK:
            controls(*dialog);
            if (dialog->options.enabled) validateRouting(dialog->options, dialog->input.provider);
            dialog->accepted = true; finishPanel(window, IDOK); return TRUE;
        case IDCANCEL: finishPanel(window, IDCANCEL); return TRUE;
        }
    } catch (const std::exception& error) {
        if (dialog && dialog->capture) { dialog->failure = error.what(); EndDialog(window, IDABORT); }
        else MessageBoxW(window, wide(dialog ? routingRedact(error.what(), dialog->options) : error.what()).c_str(),
            L"API 路由", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
}
bool editApiRouting(HINSTANCE instance, HWND owner, RoutingOptions& options, const Job& input,
    bool capture, const std::function<void(HWND)>& snapshot,
    const std::function<void(const RoutingOptions&)>& completed) {
    if (!capture && activatePanel("routing")) return false;
    auto storage = std::make_shared<Dialog>(); auto& dialog = *storage;
    dialog.instance = instance; dialog.options = options; dialog.input = input;
    dialog.capture = capture; dialog.snapshot = snapshot;
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    resource.dialog.cx = 600; resource.dialog.cy = 460;
    if (!capture) {
        openPanel("routing", instance, owner, &resource.dialog, procedure,
            reinterpret_cast<LPARAM>(&dialog), storage, [storage, completed](INT_PTR result) {
                if (result == IDOK && completed) completed(storage->options);
            });
        return false;
    }
    if (DialogBoxIndirectParamW(instance, &resource.dialog, owner, procedure, reinterpret_cast<LPARAM>(&dialog)) == -1)
        throw Error("无法打开 API 路由配置");
    if (!dialog.failure.empty()) throw Error(dialog.failure);
    if (!dialog.accepted) return false;
    options = std::move(dialog.options); return true;
}
}
