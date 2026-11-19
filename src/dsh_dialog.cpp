#include "dsh.hpp"
#include "panels.hpp"
#include <algorithm>
#include <sstream>
#include <tuple>

namespace studio {
namespace {
enum Control { FirstPermission = 4900, Models = 4920, PerTask, PerDay, Used, Saved };
struct Permission {
    const wchar_t* label;
    bool DshPolicy::* member;
};
constexpr Permission permissions[]{
    {L"基础生成与实际参数探测", &DshPolicy::generate}, {L"批量转换", &DshPolicy::batch},
    {L"无图绘制", &DshPolicy::draw}, {L"局部重绘", &DshPolicy::refine},
    {L"API 参数支持测试", &DshPolicy::probe}, {L"Waifu2x 输入 / 输出超分", &DshPolicy::upscale},
    {L"聚合网关与供应商池", &DshPolicy::gateway}, {L"本机端点（含 Qwen-Image）", &DshPolicy::localApi},
    {L"运行已启用插件的帧处理", &DshPolicy::frameHooks}, {L"覆盖插件设置（仅本次）", &DshPolicy::pluginSettings},
    {L"切换模型（仅本次）", &DshPolicy::switchModel}
};
struct Dialog {
    DshPolicy policy;
    std::shared_ptr<DshUsage> usage;
    std::function<void(const DshPolicy&)> save;
};
HWND control(HWND window, const wchar_t* type, const wchar_t* label, DWORD style,
    int id, int x, int y, int width, int height) {
    RECT rect{x, y, x + width, y + height};
    MapDialogRect(window, &rect);
    auto item = CreateWindowW(type, label, WS_CHILD | WS_VISIBLE | style, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top, window, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(item, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    return item;
}
std::wstring text(HWND window, int id) {
    auto item = GetDlgItem(window, id);
    int size = GetWindowTextLengthW(item);
    std::wstring value(size + 1, L'\0');
    GetWindowTextW(item, value.data(), size + 1);
    value.resize(size); return value;
}
void refreshUsage(HWND window, const Dialog& dialog) {
    std::wstring label = L"今日用量（UTC）：账本不可用";
    try {
        if (!dialog.usage) throw Error("Usage unavailable");
        int used = dialog.usage->today();
        label = L"今日用量（UTC）：" + std::to_wstring(used) + L" / " +
            std::to_wstring(dialog.policy.perDay) + L"；剩余 " + std::to_wstring(std::max(0, dialog.policy.perDay - used));
    } catch (const std::exception&) {}
    if (text(window, Used) != label) SetDlgItemTextW(window, Used, label.c_str());
}
int limit(HWND window, int id) {
    auto value = text(window, id);
    if (value.empty() || value.size() > 5 || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw Error("请输入 0 到 10000 的限额");
    return std::stoi(value);
}
INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<Dialog*>(lparam);
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
            SetWindowTextW(window, L"DeepSeek Harness 权限");
            control(window, L"BUTTON", L"调用权限", BS_GROUPBOX, -1, 9, 8, 334, 109);
            for (int i = 0; i < int(std::size(permissions)); ++i) {
                const auto& p = permissions[i];
                control(window, L"BUTTON", p.label, WS_TABSTOP | BS_AUTOCHECKBOX,
                    FirstPermission + i, 18 + (i % 2) * 160, 22 + (i / 2) * 15, 150, 13);
                CheckDlgButton(window, FirstPermission + i, dialog->policy.*(p.member) ? BST_CHECKED : BST_UNCHECKED);
            }
            control(window, L"STATIC", L"模型白名单（每行一个模型 ID）", 0, -1, 10, 126, 290, 12);
            control(window, L"EDIT", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                Models, 10, 141, 333, 48);
            SendDlgItemMessageW(window, Models, EM_SETLIMITTEXT, 64 * 258, 0);
            std::wstring ids;
            for (const auto& id : dialog->policy.models) { if (!ids.empty()) ids += L"\r\n"; ids += wide(id); }
            SetDlgItemTextW(window, Models, ids.c_str());
            EnableWindow(GetDlgItem(window, Models), dialog->policy.switchModel);
            control(window, L"BUTTON", L"生成请求限额（0 = 禁止发送）", BS_GROUPBOX, -1, 9, 199, 334, 58);
            control(window, L"STATIC", L"每任务", 0, -1, 18, 215, 37, 12);
            control(window, L"STATIC", L"每日", 0, -1, 179, 215, 30, 12);
            for (const auto& [id, x, value] : {std::tuple{PerTask, 59, dialog->policy.perTask},
                std::tuple{PerDay, 213, dialog->policy.perDay}}) {
                control(window, L"EDIT", std::to_wstring(value).c_str(), WS_TABSTOP | WS_BORDER | ES_NUMBER,
                    id, x, 212, 57, 15);
                SendDlgItemMessageW(window, id, EM_SETLIMITTEXT, 5, 0);
            }
            control(window, L"STATIC", L"", 0, Used, 18, 235, 310, 13);
            control(window, L"STATIC", L"", 0, Saved, 10, 269, 193, 14);
            control(window, L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 211, 265, 61, 18);
            control(window, L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 281, 265, 62, 18);
            SetDlgItemTextW(window, Saved, L"");
            refreshUsage(window, *dialog);
            SetTimer(window, 1, 1500, nullptr);
            return TRUE;
        }
        if (!dialog) return FALSE;
        if (message == WM_TIMER) { refreshUsage(window, *dialog); return TRUE; }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            finishPanel(window, IDCANCEL); return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        const int id = LOWORD(wparam);
        if (id == IDOK) {
            DshPolicy value;
            for (int i = 0; i < int(std::size(permissions)); ++i)
                value.*(permissions[i].member) = IsDlgButtonChecked(window, FirstPermission + i) == BST_CHECKED;
            value.perTask = limit(window, PerTask); value.perDay = limit(window, PerDay);
            std::wistringstream lines(text(window, Models));
            for (std::wstring line; std::getline(lines, line);) {
                auto first = line.find_first_not_of(L" \t\r"), last = line.find_last_not_of(L" \t\r");
                if (first != std::wstring::npos) value.models.push_back(utf8(line.substr(first, last - first + 1)));
            }
            value = dshPolicyFromJson(dshPolicyJson(value));
            dialog->save(value);
            dialog->policy = std::move(value);
            refreshUsage(window, *dialog);
            SetDlgItemTextW(window, Saved, L"已保存");
            return TRUE;
        }
        if (id == FirstPermission + int(std::size(permissions)) - 1)
            EnableWindow(GetDlgItem(window, Models), IsDlgButtonChecked(window, id) == BST_CHECKED);
        if ((id >= FirstPermission && id < FirstPermission + int(std::size(permissions))) ||
            ((id == Models || id == PerTask || id == PerDay) && HIWORD(wparam) == EN_CHANGE))
            SetDlgItemTextW(window, Saved, L"有未保存的更改");
    } catch (const std::exception& error) {
        MessageBoxW(window, wide(error.what()).c_str(), L"DeepSeek Harness 权限", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}
}
void showDshPermissions(HINSTANCE instance, HWND owner, const DshPolicy& policy,
    std::shared_ptr<DshUsage> usage, std::function<void(const DshPolicy&)> save) {
    if (activatePanel("dsh-permissions")) return;
    auto dialog = std::make_shared<Dialog>(Dialog{policy, std::move(usage), std::move(save)});
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, type = 0, title = 0; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    resource.dialog.cx = 352; resource.dialog.cy = 293;
    openPanel("dsh-permissions", instance, owner, &resource.dialog, procedure, reinterpret_cast<LPARAM>(dialog.get()), dialog);
}
}
