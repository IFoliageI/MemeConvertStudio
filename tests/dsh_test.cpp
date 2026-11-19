#include "../src/dsh.hpp"
#include "../src/plugins.hpp"

namespace studio {
namespace {
struct Fixture {
    DshSettings settings;
    std::unique_ptr<DshBridge> bridge;
};
LRESULT CALLBACK testWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        fixture = static_cast<Fixture*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fixture));
    }
    if (message == dshDispatch && fixture && fixture->bridge) {
        fixture->bridge->dispatch([&](const Json& request) {
            return prepareDsh(request, [&](bool) { return fixture->settings; });
        });
        return 0;
    }
    if (message == WM_TIMER) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int runDshTest(const fs::path& directory, const std::wstring& endpoint) {
    if (!endpoint.starts_with(L"http://127.0.0.1:")) throw Error("Harness test requires a loopback fixture");
    Fixture fixture;
    auto config = readJson(directory / L"fixture.json");
    fixture.settings.policy = dshPolicyFromJson(config.value("policy", Json::object()));
    fixture.settings.usage = std::make_shared<DshUsage>(directory / L"usage.json");
    fixture.settings.enabled = {dshPlugin, "memestudio.prompt-draw", "memestudio.local-redraw",
        "memestudio.batch-convert", "memestudio.api-parameter-test", "memestudio.waifu2x",
        "memestudio.api-load-balancing", "memestudio.example"};
    auto& job = fixture.settings.job;
    job.plugins = std::make_shared<Plugins>(discoverPlugins(pluginDirectory()), fixture.settings.enabled);
    job.provider.key = L"fixture-dsh-key"; job.provider.model = "fixture-image";
    job.provider.baseUrl = endpoint; job.provider.timeoutSeconds = 15;
    fixture.settings.routing.enabled = config.value("routing", false);
    if (fixture.settings.routing.enabled) {
        ApiChannel first, second;
        first.baseUrl = second.baseUrl = endpoint;
        first.key = L"fixture-route-first"; second.key = L"fixture-route-second";
        first.name = "First"; second.name = "Second"; second.priority = 1;
        fixture.settings.routing.channels = {first, second};
    }
    WNDCLASSW type{}; type.lpfnWndProc = testWindow; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"MemeStudioDshTest";
    if (!RegisterClassW(&type)) throw Error("Cannot register Harness test window");
    auto window = CreateWindowW(type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, &fixture);
    if (!window) throw Error("Cannot create Harness test window");
    fixture.bridge = std::make_unique<DshBridge>(window);
    writeJson(directory / L"ready.json", {{"ready", true}});
    SetTimer(window, 1, 60000, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    fixture.bridge.reset();
    return 0;
}
}
