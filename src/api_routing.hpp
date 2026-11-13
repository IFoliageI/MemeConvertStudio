#pragma once
#include "studio.hpp"
#include <chrono>

namespace studio {
struct ApiChannel {
    std::string name = "API", model;
    std::wstring baseUrl, key;
    int weight = 1, priority = 0;
    double unitCost = 1;
    bool enabled = true, compare = true;
};
struct RoutingOptions {
    bool enabled = false, spending = false, failover = true, serviceFailover = false, remember = false;
    int cooldownSeconds = 60;
    std::vector<ApiChannel> channels;
};
Json routingJson(const RoutingOptions& options, bool encrypted);
RoutingOptions routingOptions(const Json& json, bool encrypted);
void validateRouting(const RoutingOptions& options, const Provider& base);
class ApiRouter {
    struct State {
        double current = 0;
        int attempts = 0, successes = 0, failures = 0;
        double milliseconds = 0, estimatedCost = 0;
        std::string error;
        bool disabled = false;
        std::chrono::steady_clock::time_point cooldown{};
    };
    RoutingOptions options;
    std::vector<State> states;
    std::function<void(const std::wstring&)> logger;
    std::mutex mutex;
public:
    ApiRouter(RoutingOptions value, const Provider& base,
        std::function<void(const std::wstring&)> log = {});
    Image invoke(const Provider& base, Cancel& cancel,
        const std::function<Image(const Provider&)>& request);
    Json fingerprint() const;
    Json statistics();
};
class AggregatedGateway {
    std::shared_ptr<ApiRouter> router;
public:
    explicit AggregatedGateway(std::shared_ptr<ApiRouter> value) : router(std::move(value)) {}
    Image forward(const Provider& request, Cancel& cancel,
        const std::function<Image(const Provider&)>& upstream) const {
        return router->invoke(request, cancel, upstream);
    }
    Json statistics() { return router->statistics(); }
    Json fingerprint() const { return router->fingerprint(); }
};
Provider channelProvider(const Provider& base, const ApiChannel& channel);
Json routingFingerprint(const Provider& provider);
bool gatewayActive(bool pluginLoaded, const RoutingOptions& options);
std::string routingRedact(std::string text, const RoutingOptions& options);
std::vector<size_t> comparisonChannels(const RoutingOptions& options, const Provider& base = {});
void showApiComparison(HINSTANCE instance, HWND owner, const RoutingOptions& options, const Job& input,
    bool capture = false, const std::function<void(HWND)>& snapshot = {});
bool editApiRouting(HINSTANCE instance, HWND owner, RoutingOptions& options, const Job& input,
    bool capture = false, const std::function<void(HWND)>& snapshot = {});
int routingTest(const fs::path& directory, const std::wstring& base);
}
