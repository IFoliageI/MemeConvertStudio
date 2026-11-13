#include "../src/api_routing.hpp"
#include <array>

namespace studio {
int routingTest(const fs::path& directory, const std::wstring& base) {
    fs::create_directories(directory);
    auto require = [](bool valid, const char* message) { if (!valid) throw Error(message); };
    Provider provider; provider.baseUrl = base; provider.key = L"unused-main-key";
    RoutingOptions options; options.enabled = true;
    ApiChannel a; a.name = "A"; a.baseUrl = base; a.key = L"route-secret-a"; a.weight = 3;
    ApiChannel b = a; b.name = "B"; b.key = L"route-secret-b"; b.weight = 1;
    options.channels = {a, b};
    Cancel cancel;
    auto image = [] { return Image{1, 1, {0, 0, 0, 255}}; };
    std::array<int, 2> counts{};
    auto request = [&](const Provider& target) {
        require(!target.router && !target.gateway, "Recursive gateway/router not cleared");
        ++counts[target.key == a.key ? 0 : 1]; return image();
    };
    ApiRouter weighted(options, provider);
    for (int i = 0; i < 40; ++i) weighted.invoke(provider, cancel, request);
    require(counts[0] == 30 && counts[1] == 10, "Request weights are incorrect");
    {
        auto router = std::make_shared<ApiRouter>(options, provider);
        auto gateway = std::make_shared<AggregatedGateway>(router);
        auto local = provider; local.gateway = gateway;
        local.baseUrl = L"memestudio://local-gateway/v1"; local.key = L"local-gateway";
        counts = {};
        for (int i = 0; i < 4; ++i) gateway->forward(local, cancel, request);
        require(counts[0] == 3 && counts[1] == 1, "Gateway did not distribute through provider pool");
        require(gateway->statistics()[0]["successes"] == 3, "Gateway statistics lost");
        require(gatewayActive(true, options) && !gatewayActive(false, options), "Gateway plugin gate failed");
    }
    options.spending = true; options.channels[0].weight = 1; options.channels[1].unitCost = 3;
    ApiRouter cost(options, provider); counts = {};
    for (int i = 0; i < 40; ++i) cost.invoke(provider, cancel, request);
    require(counts[0] == 30 && counts[1] == 10, "Estimated spending weights are incorrect");
    auto stats = cost.statistics();
    require(stats[0]["estimated_cost"] == stats[1]["estimated_cost"], "Estimated cost shares differ");
    options.spending = false; options.channels[1].priority = 1;
    ApiRouter standby(options, provider); counts = {};
    for (int i = 0; i < 4; ++i) standby.invoke(provider, cancel, request);
    require(counts[1] == 0, "Standby selected before primary failed");
    int attempts = 0;
    standby.invoke(provider, cancel, [&](const Provider& target) {
        ++attempts;
        if (target.key == a.key) throw ApiHttpError(401, "echo route-secret-a route-secret-b");
        return image();
    });
    require(attempts == 2 && standby.statistics()[0]["state"] == "disabled", "Authentication failover failed");
    auto serialized = standby.statistics().dump();
    require(serialized.find("route-secret") == std::string::npos, "Cross-channel secret leaked");
    auto expectStop = [&](int status) {
        ApiRouter router(options, provider); int sent = 0; bool failed = false;
        try {
            router.invoke(provider, cancel, [&](const Provider&) -> Image {
                ++sent;
                if (status) throw ApiHttpError(DWORD(status), "rejected");
                throw Error("unknown outcome");
            });
        } catch (const Error&) { failed = true; }
        require(failed && sent == 1, "Unsafe automatic retry or missing error");
    };
    for (int code : {0, 400, 422, 500, 502, 503, 504}) expectStop(code);
    ApiRouter limited(options, provider); attempts = 0;
    limited.invoke(provider, cancel, [&](const Provider& target) {
        ++attempts; if (target.key == a.key) throw ApiHttpError(429, "limited"); return image();
    });
    require(attempts == 2 && limited.statistics()[0]["state"] == "cooldown", "Rate limit cooldown failed");
    ApiRouter disconnected(options, provider); attempts = 0;
    disconnected.invoke(provider, cancel, [&](const Provider& target) {
        ++attempts; if (target.key == a.key) throw ApiConnectionError("cannot connect"); return image();
    });
    require(attempts == 2 && disconnected.statistics()[0]["state"] == "cooldown", "Connection failover failed");
    options.serviceFailover = true;
    ApiRouter service(options, provider); attempts = 0;
    service.invoke(provider, cancel, [&](const Provider& target) {
        ++attempts; if (target.key == a.key) throw ApiHttpError(503, "unavailable"); return image();
    });
    require(attempts == 2, "Opt-in service failover failed");
    ApiRouter exhausted(options, provider); attempts = 0;
    try {
        exhausted.invoke(provider, cancel, [&](const Provider&) -> Image { ++attempts; throw ApiHttpError(429, "limited"); });
    } catch (const Error&) {}
    require(attempts == 2, "Channel retried within same request");
    ApiRouter stopped(options, provider); attempts = 0; cancel.stop();
    try { stopped.invoke(provider, cancel, [&](const Provider&) { ++attempts; return image(); }); } catch (const Error&) {}
    require(attempts == 0, "Cancelled request sent"); cancel.reset();
    options.remember = false;
    require(routingJson(options, true).dump().find("encrypted_key") == std::string::npos, "Keys saved without opt-in");
    options.remember = true;
    auto saved = routingJson(options, true);
    require(saved.dump().find("route-secret") == std::string::npos, "Plaintext key persisted");
    require(routingOptions(saved, true).channels[1].key == b.key, "DPAPI key roundtrip failed");
    require(weighted.fingerprint().dump().find("route-secret") == std::string::npos, "Key in cache manifest");
    require(comparisonChannels(options).size() == 1, "Same provider/model was compared twice");
    options.channels[0].model = provider.model;
    require(comparisonChannels(options, provider).size() == 1, "Inherited model was not deduplicated");
    options.channels[1].model = "different-model";
    require(comparisonChannels(options).size() == 2, "Different model missing from comparison");
    options.channels[1].key = a.key;
    bool duplicate = false;
    try { validateRouting(options, provider); } catch (const Error&) { duplicate = true; }
    require(duplicate, "Duplicate API/key accepted");
    writeJson(directory / L"routing-unit.json", {{"weighted_requests", "30:10"}, {"estimated_spending", "30:30"},
        {"failover", "passed"}, {"ambiguous_outcome_stop", "passed"}, {"bounded_attempts", "passed"},
        {"cancellation", "passed"}, {"credential_protection", "passed"}, {"comparison_deduplication", "passed"}});
    return 0;
}
}
