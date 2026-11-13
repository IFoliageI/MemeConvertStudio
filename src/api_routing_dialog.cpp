#include "api_routing.hpp"
#include <commctrl.h>
#include <algorithm>
#include <sstream>
#include <cmath>

namespace studio {
namespace {
enum { Channels = 4300, Name, Url, Model, Key, Weight, Priority, Cost, Active, Compare,
    Add, Update, Remove, CopyMain, Enabled, Spending, Failover, Service, Remember, Cooldown, Comparison, Stats };
struct Dialog {
    HINSTANCE instance{};
    HWND window{}, list{};
    HFONT font{};
    RoutingOptions options;
    Job input;
    bool accepted = false, capture = false;
    std::string failure;
    std::function<void(HWND)> snapshot;
    ~Dialog() { if (font) DeleteObject(font); }
};
std::wstring text(HWND window, int id) {
    auto control = GetDlgItem(window, id);
    std::wstring value(GetWindowTextLengthW(control) + 1, L'\0');
    GetWindowTextW(control, value.data(), int(value.size())); value.pop_back(); return value;
}
bool checked(HWND window, int id) { return IsDlgButtonChecked(window, id) == BST_CHECKED; }
void checkBox(HWND window, int id, bool value) { CheckDlgButton(window, id, value ? BST_CHECKED : BST_UNCHECKED); }
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
    fill(dialog, index);
}
INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<Dialog*>(lparam); dialog->window = window;
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            SetWindowTextW(window, L"API 负载均衡与灾备");
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
            tick(Enabled, L"生成任务启用路由", 16, 12, 190, dialog->options.enabled);
            tick(Spending, L"按估算花销比例", 220, 12, 188, dialog->options.spending);
            tick(Failover, L"启用灾备切换", 430, 12, 170, dialog->options.failover);
            tick(Remember, L"本机加密记住 Keys", 670, 12, 274, dialog->options.remember);
            dialog->list = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                Channels, 16, 50, 928, 206);
            ListView_SetExtendedListViewStyle(dialog->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* titles[]{L"渠道 / Key 别名", L"状态", L"API 地址", L"模型", L"权重", L"优先级", L"估算单价", L"对比"};
            const int widths[]{142, 56, 248, 145, 62, 64, 116, 66};
            for (int i = 0; i < 8; ++i) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(titles[i]); column.cx = widths[i];
                ListView_InsertColumn(dialog->list, i, &column);
            }
            add(L"STATIC", L"渠道别名", 0, 0, 16, 270, 150, 22);
            add(L"STATIC", L"Base URL", 0, 0, 204, 270, 460, 22);
            add(L"STATIC", L"模型（空为主界面模型）", 0, 0, 680, 270, 264, 22);
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
            add(L"STATIC", L"优先级越小越优先；同级按权重分流，默认按请求数。花销模式按 权重 ÷ 估算单价 分配请求。",
                0, 0, 16, 490, 928, 24);
            add(L"STATIC", L"连接未建立或 429 将冷却切换，401/403 停用；超时、流中断、图像下载失败等结果不明错误停止。",
                0, 0, 16, 520, 928, 24);
            std::wstring stats = L"统计在生成任务后显示；估算花销仅累计成功请求，不含失败计费、缓存、Token 及实际账单差异。";
            if (dialog->input.provider.router) stats = wide(dialog->input.provider.router->statistics().dump(2));
            add(L"EDIT", stats.c_str(), WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                Stats, 16, 556, 928, 110);
            add(L"BUTTON", L"效果对比…", WS_TABSTOP, Comparison, 16, 686, 172, 30);
            add(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 684, 686, 124, 30);
            add(L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL, 820, 686, 124, 30);
            fill(*dialog);
            if (dialog->capture) PostMessageW(window, WM_APP + 41, 0, 0);
            return TRUE;
        }
        if (!dialog) return FALSE;
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
            checkBox(window, Enabled, true);
            if (dialog->snapshot) dialog->snapshot(window);
            SendMessageW(window, WM_COMMAND, IDOK, 0); return TRUE;
        }
        if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == Channels &&
            reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) {
            auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED))
                select(*dialog, change->iItem);
            return FALSE;
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
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
            dialog->accepted = true; EndDialog(window, IDOK); return TRUE;
        case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
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
    bool capture, const std::function<void(HWND)>& snapshot) {
    Dialog dialog; dialog.instance = instance; dialog.options = options; dialog.input = input;
    dialog.capture = capture; dialog.snapshot = snapshot;
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    resource.dialog.cx = 600; resource.dialog.cy = 460;
    if (DialogBoxIndirectParamW(instance, &resource.dialog, owner, procedure, reinterpret_cast<LPARAM>(&dialog)) == -1)
        throw Error("无法打开 API 路由配置");
    if (!dialog.failure.empty()) throw Error(dialog.failure);
    if (!dialog.accepted) return false;
    options = std::move(dialog.options); return true;
}
}
