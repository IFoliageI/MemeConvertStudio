#include "api_routing.hpp"
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace studio {
Provider channelProvider(const Provider& base, const ApiChannel& channel) {
    auto result = base;
    result.router.reset();
    result.gateway.reset();
    result.baseUrl = channel.baseUrl; result.key = channel.key;
    if (!channel.model.empty()) result.model = channel.model;
    return result;
}
Json routingJson(const RoutingOptions& options, bool encrypted) {
    Json result{{"enabled", options.enabled}, {"spending", options.spending}, {"failover", options.failover},
        {"service_failover", options.serviceFailover}, {"remember", options.remember},
        {"cooldown_seconds", options.cooldownSeconds}, {"channels", Json::array()}};
    for (const auto& channel : options.channels) {
        Json item{{"name", channel.name}, {"base_url", utf8(channel.baseUrl)}, {"model", channel.model},
            {"weight", channel.weight}, {"priority", channel.priority}, {"unit_cost", channel.unitCost},
            {"enabled", channel.enabled}, {"compare", channel.compare}};
        if (encrypted && options.remember && !channel.key.empty()) item["encrypted_key"] = protect(channel.key);
        result["channels"].push_back(std::move(item));
    }
    return result;
}
RoutingOptions routingOptions(const Json& json, bool encrypted) {
    if (!json.is_object()) throw Error("路由配置必须为 JSON 对象");
    RoutingOptions result;
    result.enabled = json.value("enabled", false); result.spending = json.value("spending", false);
    result.failover = json.value("failover", true); result.serviceFailover = json.value("service_failover", false);
    result.remember = json.value("remember", false); result.cooldownSeconds = json.value("cooldown_seconds", 60);
    auto channels = json.value("channels", Json::array());
    if (!channels.is_array() || channels.size() > 64) throw Error("最多配置 64 个 API / Key 渠道");
    for (const auto& item : channels) {
        ApiChannel channel;
        channel.name = item.value("name", "API"); channel.baseUrl = wide(item.at("base_url").get<std::string>());
        channel.model = item.value("model", ""); channel.weight = item.value("weight", 1);
        channel.priority = item.value("priority", 0); channel.unitCost = item.value("unit_cost", 1.0);
        channel.enabled = item.value("enabled", true); channel.compare = item.value("compare", true);
        if (encrypted && item.contains("encrypted_key")) channel.key = unprotect(item["encrypted_key"].get<std::string>());
        else if (!encrypted) {
            auto name = wide(item.value("api_key_env", ""));
            if (!name.empty()) {
                wchar_t key[8192]{};
                DWORD count = GetEnvironmentVariableW(name.c_str(), key, 8192);
                if (count && count < 8192) channel.key = key;
                SecureZeroMemory(key, sizeof(key));
            }
        }
        result.channels.push_back(std::move(channel));
    }
    return result;
}
void validateRouting(const RoutingOptions& options, const Provider& base) {
    if (options.channels.empty() || options.channels.size() > 64) throw Error("请配置 1 到 64 个渠道");
    if (options.cooldownSeconds < 1 || options.cooldownSeconds > 3600) throw Error("冷却时间须为 1 到 3600 秒");
    int enabled = 0;
    std::set<std::pair<std::wstring, std::wstring>> seen;
    for (const auto& channel : options.channels) {
        if (!channel.enabled) continue;
        ++enabled;
        if (channel.name.empty() || channel.name.size() > 128 ||
            channel.name.find_first_of("\r\n\t") != std::string::npos) throw Error("渠道名称须为 1 到 128 字节的单行文本");
        if (channel.weight < 1 || channel.weight > 10000 || channel.priority < 0 || channel.priority > 99 ||
            !std::isfinite(channel.unitCost) || channel.unitCost < 0.000001 || channel.unitCost > 1000000)
            throw Error("权重须为 1–10000，优先级为 0–99，单价须为 0.000001–1000000");
        if (channel.key.empty() || std::any_of(channel.key.begin(), channel.key.end(),
            [](wchar_t c) { return c < 33 || c > 126; })) throw Error("渠道 API Key 为空或包含无效字符");
        auto provider = channelProvider(base, channel);
        auto url = editUrl(provider.baseUrl);
        URL_COMPONENTS parts{sizeof(parts)};
        parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) throw Error("渠道 URL 格式无效");
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        if (parts.dwUserNameLength || parts.dwPasswordLength || url.find(L'?') != std::wstring::npos)
            throw Error("渠道 URL 不允许内嵌凭据或查询参数，请使用 API Key 字段");
        if (parts.nScheme != INTERNET_SCHEME_HTTPS && !(parts.nScheme == INTERNET_SCHEME_HTTP &&
            (host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1")))
            throw Error("渠道须使用 HTTPS（本机模拟服务除外）");
        if (!seen.emplace(url, channel.key).second) throw Error("同一接口和 Key 请只配置一次");
        generationBody(provider, "validate");
        multipart(provider, "validate", {{"validate", Bytes{1}}});
    }
    if (!enabled) throw Error("至少启用一个渠道");
}
std::string routingRedact(std::string value, const RoutingOptions& options) {
    for (const auto& channel : options.channels) {
        auto secret = utf8(channel.key);
        if (!secret.empty()) for (size_t pos = 0; (pos = value.find(secret, pos)) != std::string::npos;) {
            value.replace(pos, secret.size(), "[REDACTED]"); pos += 10;
        }
    }
    return value;
}
ApiRouter::ApiRouter(RoutingOptions value, const Provider& base,
    std::function<void(const std::wstring&)> log) : options(std::move(value)), logger(std::move(log)) {
    validateRouting(options, base);
    states.resize(options.channels.size());
}
Image ApiRouter::invoke(const Provider& base, Cancel& cancel,
    const std::function<Image(const Provider&)>& request) {
    // A single image has bounded attempts; never send the same channel twice.
    std::vector<bool> tried(states.size());
    std::string last;
    for (;;) {
        cancel.verify();
        size_t selected = states.size();
        {
            std::lock_guard lock(mutex);
            int tier = 100;
            auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < states.size(); ++i)
                if (options.channels[i].enabled && !tried[i] && !states[i].disabled && states[i].cooldown <= now)
                    tier = std::min(tier, options.channels[i].priority);
            double sum = 0;
            for (size_t i = 0; i < states.size(); ++i) {
                auto& state = states[i]; const auto& channel = options.channels[i];
                if (!channel.enabled || tried[i] || state.disabled || state.cooldown > now || channel.priority != tier) continue;
                double weight = channel.weight / (options.spending ? channel.unitCost : 1.0);
                sum += weight; state.current += weight;
                if (selected == states.size() || state.current > states[selected].current) selected = i;
            }
            if (selected != states.size()) {
                states[selected].current -= sum; ++states[selected].attempts;
            }
        }
        if (selected == states.size())
            throw Error(last.empty() ? "无可用渠道：鉴权失败的渠道已停用，限流渠道冷却中；请检查配置后重新开始" :
                "灾备渠道已耗尽：" + last);
        tried[selected] = true;
        const auto& channel = options.channels[selected];
        auto label = wide(routingRedact(channel.name, options));
        if (logger) logger(L"聚合网关 → 供应商池：" + label + L" · 模型 " + wide(channel.model.empty() ? base.model : channel.model));
        auto start = std::chrono::steady_clock::now();
        try {
            auto image = request(channelProvider(base, channel));
            std::lock_guard lock(mutex);
            auto& state = states[selected]; ++state.successes;
            state.estimatedCost += channel.unitCost;
            state.milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            state.error.clear();
            return image;
        } catch (const std::exception& error) {
            cancel.verify();
            auto http = dynamic_cast<const ApiHttpError*>(&error);
            bool connection = dynamic_cast<const ApiConnectionError*>(&error) != nullptr;
            bool auth = http && (http->status == 401 || http->status == 403);
            bool limit = http && http->status == 429;
            bool service = http && options.serviceFailover &&
                (http->status == 502 || http->status == 503 || http->status == 504);
            last = routingRedact(error.what(), options);
            {
                std::lock_guard lock(mutex);
                auto& state = states[selected]; ++state.failures; state.error = last;
                state.milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (auth) state.disabled = true;
                if (limit || service || connection) state.cooldown = std::chrono::steady_clock::now() +
                    std::chrono::seconds(options.cooldownSeconds);
            }
            if (!options.failover || !(auth || limit || service || connection)) throw Error(last);
            if (logger) logger(L"渠道 " + label + (connection ? L" 未建立连接" :
                L" 返回 HTTP " + std::to_wstring(http->status)) + L"，切换下一可用渠道");
        }
    }
}
Json ApiRouter::fingerprint() const {
    auto result = routingJson(options, false);
    result.erase("remember");
    // No API keys, ciphertext or key fingerprints enter caches or plugin context.
    return result;
}
Json routingFingerprint(const Provider& provider) {
    return provider.gateway ? provider.gateway->fingerprint() :
        provider.router ? provider.router->fingerprint() : Json();
}
bool gatewayActive(bool pluginLoaded, const RoutingOptions& options) {
    return pluginLoaded && options.enabled;
}
Json ApiRouter::statistics() {
    std::lock_guard lock(mutex);
    Json result = Json::array();
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < states.size(); ++i) {
        const auto& state = states[i];
        result.push_back({{"index", i}, {"name", routingRedact(options.channels[i].name, options)},
            {"attempts", state.attempts}, {"successes", state.successes}, {"failures", state.failures},
            {"estimated_cost", state.estimatedCost}, {"milliseconds", state.milliseconds},
            {"state", state.disabled ? "disabled" : state.cooldown > now ? "cooldown" : "ready"},
            {"error", state.error}});
    }
    return result;
}
std::vector<size_t> comparisonChannels(const RoutingOptions& options, const Provider& base) {
    std::vector<size_t> result;
    std::set<std::pair<std::wstring, std::string>> seen;
    for (size_t i = 0; i < options.channels.size(); ++i) {
        const auto& channel = options.channels[i];
        if (!channel.enabled || !channel.compare) continue;
        auto model = generationBody(channelProvider(base, channel), "compare").at("model").get<std::string>();
        if (seen.emplace(editUrl(channel.baseUrl), model).second) result.push_back(i);
    }
    return result;
}
}
