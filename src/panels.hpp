#pragma once
#include "studio.hpp"
#include <algorithm>

namespace studio {
namespace panels {
constexpr UINT Finish = WM_APP + 120;
struct Panel {
    std::string key;
    HWND window{}, owner{};
    DLGPROC procedure{};
    LPARAM parameter{};
    std::shared_ptr<void> lifetime;
    std::function<void(INT_PTR)> completed;
};
inline std::vector<std::shared_ptr<Panel>> opened;
inline INT_PTR CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto found = std::find_if(opened.begin(), opened.end(), [=](const auto& p) { return p->window == window; });
    std::shared_ptr<Panel> panel;
    if (message == WM_INITDIALOG) {
        auto* incoming = reinterpret_cast<Panel*>(lparam);
        found = std::find_if(opened.begin(), opened.end(), [=](const auto& p) { return p.get() == incoming; });
        if (found != opened.end()) { panel = *found; panel->window = window; lparam = panel->parameter; }
    } else if (found != opened.end()) panel = *found;
    if (!panel) return FALSE;
    if (message == Finish) {
        DestroyWindow(window);
        try { if (panel->completed) panel->completed(INT_PTR(wparam)); }
        catch (const std::exception& error) {
            MessageBoxW(panel->owner, wide(error.what()).c_str(), L"插件面板", MB_OK | MB_ICONERROR);
        }
        return TRUE;
    }
    auto result = panel->procedure(window, message, wparam, lparam);
    if (message == WM_NCDESTROY)
        std::erase_if(opened, [=](const auto& item) { return item == panel; });
    return result;
}
}
inline HWND findPanel(const std::string& key) {
    for (const auto& panel : panels::opened) if (panel->key == key && IsWindow(panel->window)) return panel->window;
    return nullptr;
}
inline bool activatePanel(const std::string& key) {
    if (auto window = findPanel(key)) {
        ShowWindow(window, SW_RESTORE); SetForegroundWindow(window); return true;
    }
    return false;
}
inline HWND openPanel(const std::string& key, HINSTANCE instance, HWND owner,
    const DLGTEMPLATE* resource, DLGPROC procedure, LPARAM parameter,
    std::shared_ptr<void> lifetime, std::function<void(INT_PTR)> completed = {}) {
    if (activatePanel(key)) return findPanel(key);
    owner = GetAncestor(owner, GA_ROOTOWNER);
    auto panel = std::make_shared<panels::Panel>();
    panel->key = key; panel->owner = owner; panel->procedure = procedure; panel->parameter = parameter;
    panel->lifetime = std::move(lifetime); panel->completed = std::move(completed);
    panels::opened.push_back(panel);
    auto window = CreateDialogIndirectParamW(instance, resource, owner, panels::dispatch, reinterpret_cast<LPARAM>(panel.get()));
    if (!window) { std::erase(panels::opened, panel); throw Error("无法打开插件面板"); }
    ShowWindow(window, SW_SHOW); return window;
}
inline void finishPanel(HWND window, INT_PTR result) {
    for (const auto& panel : panels::opened) if (panel->window == window) {
        EnableWindow(window, FALSE);
        PostMessageW(window, panels::Finish, WPARAM(result), 0); return;
    }
    EndDialog(window, result);
}
inline void destroyPanels() {
    auto copy = panels::opened;
    for (const auto& panel : copy) if (IsWindow(panel->window)) DestroyWindow(panel->window);
}
inline void destroyPanel(const std::string& key) {
    if (auto window = findPanel(key)) DestroyWindow(window);
}
inline bool panelMessage(MSG& message) {
    auto root = GetAncestor(message.hwnd, GA_ROOT);
    wchar_t name[32]{};
    if (root) GetClassNameW(root, name, 32);
    return root && wcscmp(name, L"#32770") == 0 && IsDialogMessageW(root, &message);
}
}
