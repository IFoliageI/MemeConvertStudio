#pragma once
#include "studio.hpp"
#include "api_routing.hpp"
#include <thread>

namespace studio {
inline constexpr auto dshPlugin = "memestudio.deepseek-harness";
inline constexpr UINT dshDispatch = WM_APP + 122;
using DshWork = std::function<Json(Cancel&)>;

class DshBridge {
    struct Request;
    HWND owner;
    HANDLE pipe;
    Cancel cancel;
    std::mutex mutex;
    std::shared_ptr<Request> pending;
    std::atomic_bool running{false};
    std::jthread server;
    void serve(std::stop_token stop);
public:
    explicit DshBridge(HWND owner);
    ~DshBridge();
    bool active() const { return running.load(); }
    void stopJob() { cancel.stop(); }
    void dispatch(const std::function<DshWork(const Json&)>& prepare);
};

struct DshPolicy {
    bool generate = false, batch = false, draw = false, refine = false, probe = false;
    bool upscale = false, gateway = false, localApi = false, frameHooks = false;
    bool pluginSettings = false, switchModel = false;
    std::vector<std::string> models;
    int perTask = 8, perDay = 32;
};
Json dshPolicyJson(const DshPolicy& policy);
DshPolicy dshPolicyFromJson(const Json& data);

class DshUsage {
    fs::path path;
    std::mutex mutex;
    int usedToday(const std::string& day) const;
public:
    explicit DshUsage(fs::path ledger) : path(std::move(ledger)) {}
    int today();
    void charge(int dayLimit);
};
void showDshPermissions(HINSTANCE instance, HWND owner, const DshPolicy& policy,
    std::shared_ptr<DshUsage> usage, std::function<void(const DshPolicy&)> save);

struct DshSettings {
    std::vector<std::string> enabled;
    Job job;
    RoutingOptions routing;
    DshPolicy policy;
    std::shared_ptr<DshUsage> usage;
};
DshWork prepareDsh(const Json& request, const std::function<DshSettings(bool)>& settings,
    const Reporter& report = {});
int runDshTest(const fs::path& directory, const std::wstring& endpoint);
}
