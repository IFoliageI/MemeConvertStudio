#include "api_probe.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <cctype>

namespace studio {
int estimateProbeTokens(const std::string& prompt) {
    // This generic character heuristic is informative, not a provider tokenizer.
    int tokens = 0, asciiRun = 0;
    auto flushAscii = [&] {
        if (asciiRun) tokens += (asciiRun + 3) / 4;
        asciiRun = 0;
    };
    for (size_t i = 0; i < prompt.size();) {
        unsigned char lead = static_cast<unsigned char>(prompt[i]);
        if (lead < 0x80) {
            if (std::isalnum(lead)) ++asciiRun;
            else { flushAscii(); if (!std::isspace(lead)) ++tokens; }
            ++i;
            continue;
        }
        flushAscii();
        int continuation = (lead & 0xF0) == 0xF0 ? 3 : (lead & 0xE0) == 0xE0 ? 2 : 1;
        uint32_t codepoint = lead & (continuation == 3 ? 0x07 : continuation == 2 ? 0x0F : 0x1F);
        if (i + continuation >= prompt.size()) { ++tokens; break; }
        for (int j = 1; j <= continuation; ++j)
            codepoint = (codepoint << 6) | (static_cast<unsigned char>(prompt[i + j]) & 0x3F);
        tokens += codepoint >= 0x2E80 && codepoint <= 0x9FFF ? 1 : 2;
        i += continuation + 1;
    }
    flushAscii();
    return std::max(1, tokens);
}
Json estimateProbeUsage(const Provider& provider) {
    static const std::string text = "Create a simple blue square on a white background. Return one image without text.";
    Json result{{"text_input", estimateProbeTokens(text)}, {"image_input", nullptr}, {"output_min", nullptr},
        {"output_max", nullptr}, {"partial_image_overhead", provider.stream ? 100 * provider.partialImages : 0},
        {"estimated", true}};
    // Keep this legacy profile narrow: aliases and newer models may bill differently.
    if (provider.model != "gpt-image-1" && provider.model != "gpt-image-1.5" &&
        provider.model != "gpt-image-1-mini") return result;
    const int tokens[3][3]{{272, 408, 400}, {1056, 1584, 1568}, {4160, 6240, 6208}};
    int q = provider.quality == "low" ? 0 : provider.quality == "medium" ? 1 : provider.quality == "high" ? 2 : -1;
    if (!provider.quality.empty() && provider.quality != "auto" && q < 0) return result;
    int s = provider.size == "1024x1024" ? 0 : provider.size == "1024x1536" ? 1 :
        provider.size == "1536x1024" ? 2 : -1;
    if (!provider.size.empty() && provider.size != "auto" && s < 0) return result;
    int minimum = 100000, maximum = 0;
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column)
        if ((q < 0 || q == row) && (s < 0 || s == column)) {
            minimum = std::min(minimum, tokens[row][column]); maximum = std::max(maximum, tokens[row][column]);
        }
    result["output_min"] = minimum; result["output_max"] = maximum;
    result["profile"] = "legacy_gpt_image_standard_sizes";
    return result;
}
std::wstring probeTokenLabel(const Provider& provider) {
    auto usage = estimateProbeUsage(provider);
    auto result = L"文本 ~" + std::to_wstring(usage["text_input"].get<int>()) + L"；输出 ";
    if (usage["output_min"].is_null()) return result + L"待定";
    result += std::to_wstring(usage["output_min"].get<int>());
    if (usage["output_min"] != usage["output_max"]) result += L"–" + std::to_wstring(usage["output_max"].get<int>());
    int partial = usage["partial_image_overhead"];
    if (partial) result += L" + 局部图 ≤" + std::to_wstring(partial);
    return result;
}
static Provider baselineProvider(const Provider& current) {
    Provider base;
    base.baseUrl = current.baseUrl; base.key = current.key; base.model = current.model;
    base.timeoutSeconds = current.timeoutSeconds;
    base.quality.clear(); base.size.clear();
    base.imageField = "image[]";
    return base;
}
Json probeCaseParameters(const Provider& provider) {
    auto value = generationBody(provider, "probe");
    value.erase("prompt"); value.erase("model");
    return value;
}
std::vector<ProbeCase> parameterProbeCases(const Provider& provider, bool generation) {
    auto base = baselineProvider(provider);
    std::vector<ProbeCase> cases{{"基线（仅模型、提示词及编辑输入）", base, true}};
    std::set<std::string> seen;
    auto add = [&](const std::string& name, const Provider& value, bool selected) {
        auto signature = probeCaseParameters(value).dump() + (generation ? "" : value.imageField);
        if (seen.insert(signature).second) cases.push_back({name, value, selected});
    };
    seen.insert(probeCaseParameters(base).dump() + (generation ? "" : base.imageField));
    auto current = probeCaseParameters(provider);
    for (const auto& [key, value] : current.items()) {
        auto single = base;
        if (key == "quality") single.quality = provider.quality;
        else if (key == "size") single.size = provider.size;
        else if (key == "complexity") single.complexity = provider.complexity;
        else if (key == "output_format") single.outputFormat = provider.outputFormat;
        else if (key == "output_compression") {
            single.outputFormat = provider.outputFormat; single.compression = provider.compression;
        } else if (key == "stream") {
            single.stream = true; single.partialImages = provider.partialImages;
        } else if (key == "partial_images") continue;
        else single.extra[key] = value;
        add(key == "stream" ? "stream + partial_images" : key + " = " + value.dump(), single, true);
    }
    if (!generation && provider.imageField != base.imageField) {
        auto field = base; field.imageField = provider.imageField;
        add("image_field = " + provider.imageField, field, true);
    }
    add("当前完整参数组合", provider, true);
    auto candidate = [&](const std::string& key, const Json& value) {
        auto item = customProbeCase(base, {{"name", key + " = " + value.dump()}, {"parameters", {{key, value}}}});
        add(item.name, item.provider, false);
    };
    for (const auto* quality : {"auto", "low", "medium", "high", "xhigh", "max"}) candidate("quality", quality);
    for (const auto* complexity : {"low", "medium", "high"}) candidate("complexity", complexity);
    for (const auto* size : {"auto", "1024x1024", "1536x1024", "1024x1536"}) candidate("size", size);
    for (const auto* format : {"png", "jpeg", "webp"}) candidate("output_format", format);
    for (int compression : {0, 80, 100}) {
        auto value = base; value.outputFormat = "jpeg"; value.compression = compression;
        add("output_compression = " + std::to_string(compression) + " (jpeg)", value, false);
    }
    for (int partial : {1, 2, 3}) {
        auto value = base; value.stream = true; value.partialImages = partial;
        add("stream = true, partial_images = " + std::to_string(partial), value, false);
    }
    if (!generation) {
        auto alternate = base; alternate.imageField = base.imageField == "image[]" ? "image" : "image[]";
        add("image field = " + alternate.imageField, alternate, false);
    }
    if (cases.size() > 64) throw Error("参数测试清单不能超过 64 项");
    return cases;
}
ProbeCase customProbeCase(const Provider& provider, const Json& value) {
    if (!value.is_object()) throw Error("每个组合必须是 JSON 对象");
    auto result = baselineProvider(provider);
    const auto& parameters = value.at("parameters");
    if (!parameters.is_object() || parameters.size() > 16) throw Error("每个组合的 parameters 必须是最多 16 个字段的对象");
    for (const auto& [key, item] : parameters.items()) {
        if (item.is_null()) continue;
        if (key == "quality") result.quality = item.get<std::string>();
        else if (key == "size") result.size = item.get<std::string>();
        else if (key == "complexity") result.complexity = item.get<std::string>();
        else if (key == "output_format") result.outputFormat = item.get<std::string>();
        else if (key == "output_compression") result.compression = item.get<int>();
        else if (key == "stream") result.stream = item.get<bool>();
        else if (key == "partial_images") result.partialImages = item.get<int>();
        else result.extra[key] = item;
    }
    result.imageField = value.value("image_field", provider.imageField);
    auto name = value.value("name", "自定义组合");
    if (name.empty() || name.size() > 160) throw Error("测试名称必须在 1 到 160 字节之间");
    generationBody(result, "probe");
    // Validate the edit field and all optional fields before any request is sent.
    multipart(result, "probe", {{"validate", Bytes{1}}});
    return {name, result, true};
}
std::vector<Upload> probeImages(const fs::path& source, const fs::path& character, const Crop& crop) {
    std::vector<Upload> images;
    if (!source.empty()) images.push_back({"source", png(limited(decode(source).frames.front().image))});
    if (!character.empty()) images.push_back({"character", png(limited(cropped(decode(character).frames.front().image, crop)))});
    if (images.empty()) {
        Image image{64, 64, Bytes(64 * 64 * 4, 255)};
        for (UINT y = 16; y < 48; ++y) for (UINT x = 16; x < 48; ++x) {
            auto offset = (y * 64 + x) * 4;
            image.pixels[offset] = 210; image.pixels[offset + 1] = 100; image.pixels[offset + 2] = 40;
        }
        images.push_back({"diagnostic", png(image)});
    }
    return images;
}
static std::string redact(std::string value, const Provider& provider) {
    auto key = utf8(provider.key);
    if (!key.empty()) for (size_t pos = 0; (pos = value.find(key, pos)) != std::string::npos;) {
        value.replace(pos, key.size(), "[REDACTED]"); pos += 10;
    }
    return value;
}
Json runParameterProbe(const ProbeOptions& options, Cancel& cancel, const std::function<void(const Json&)>& notify) {
    if (options.cases.empty() || options.cases.size() > 64) throw Error("测试清单必须有 1 到 64 项");
    if (options.provider.key.empty()) throw Error("请填写 API Key");
    auto baseline = baselineProvider(options.provider);
    // The first request must be the actual minimal baseline, regardless of caller input.
    std::vector<ProbeCase> selected{{"基线", baseline, true}};
    for (size_t i = 1; i < options.cases.size(); ++i) if (options.cases[i].selected) {
        auto item = options.cases[i];
        item.provider.baseUrl = options.provider.baseUrl; item.provider.key = options.provider.key;
        item.provider.model = options.provider.model; item.provider.timeoutSeconds = options.provider.timeoutSeconds;
        selected.push_back(std::move(item));
    }
    const std::string prompt = "Create a simple blue square on a white background. Return one image without text.";
    for (const auto& item : selected) {
        generationBody(item.provider, prompt);
        if (!options.generation) multipart(item.provider, prompt, options.images);
    }
    auto endpoint = options.generation ? generationUrl(options.provider.baseUrl) : editUrl(options.provider.baseUrl);
    endpoint.resize(endpoint.find(L'?') == std::wstring::npos ? endpoint.size() : endpoint.find(L'?'));
    SYSTEMTIME time{}; GetSystemTime(&time);
    wchar_t timestamp[32]{};
    swprintf_s(timestamp, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    Json report{{"version", 1}, {"model", options.provider.model}, {"provider_endpoint", utf8(endpoint)},
        {"created_at", utf8(timestamp)},
        {"endpoint", options.generation ? "/images/generations" : "/images/edits"},
        {"estimated_prompt_text_tokens_per_request", estimateProbeTokens(prompt)},
        {"estimated_prompt_text_tokens_total", estimateProbeTokens(prompt) * int(selected.size())},
        {"token_estimate_note", "Rough text-only estimate using a generic character heuristic; provider/model-specific tokenization, image input and generated output tokens are excluded."},
        {"planned_requests", selected.size()}, {"sent_requests", 0}, {"cancelled", false},
        {"conclusion", "Request acceptance does not prove semantic support. Results apply only to this model, endpoint and tested values."},
        {"results", Json::array()}};
    int outputMin = 0, outputMax = 0, unknown = 0, partial = 0;
    for (const auto& item : selected) {
        auto usage = estimateProbeUsage(item.provider);
        if (usage["output_min"].is_null()) ++unknown;
        else { outputMin += usage["output_min"].get<int>(); outputMax += usage["output_max"].get<int>(); }
        partial += usage["partial_image_overhead"].get<int>();
    }
    report["estimated_tokens"] = {{"text_input", estimateProbeTokens(prompt) * int(selected.size())},
        {"image_input", options.generation ? Json(0) : Json(nullptr)},
        {"output_min", unknown ? Json(nullptr) : Json(outputMin)}, {"output_max", unknown ? Json(nullptr) : Json(outputMax)},
        {"unknown_output_requests", unknown}, {"partial_image_overhead", partial}, {"estimated", true}};
    bool stop = false;
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& item = selected[i];
        Json row{{"index", i}, {"name", item.name}, {"parameters", probeCaseParameters(item.provider)},
            {"estimated_tokens", estimateProbeUsage(item.provider)},
            {"image_field", options.generation ? "" : item.provider.imageField}, {"status", 0},
            {"outcome", "skipped"}, {"detail", "前序请求失败，未发送"}};
        if (!stop && !cancel.stopped) {
            row["outcome"] = "running"; row["detail"] = "请求中";
            if (notify) notify(Json::parse(redact(row.dump(), options.provider)));
            auto begin = std::chrono::steady_clock::now();
            try {
                report["sent_requests"] = report["sent_requests"].get<int>() + 1;
                auto result = probeImage(item.provider, prompt, options.images, options.generation, cancel);
                row["status"] = result.status; row["outcome"] = result.outcome;
                row["detail"] = result.detail; row["streamed"] = result.streamed;
                row["partial_images_received"] = result.partialImages;
                if (result.outcome == "request_rejected") {
                    auto parameters = probeCaseParameters(item.provider);
                    bool parameterError = !result.errorParameter.empty() && parameters.contains(result.errorParameter);
                    row["outcome"] = parameterError ? "rejected" : "request_rejected";
                }
                if ((i == 0 && result.outcome != "accepted") || result.status == 401 ||
                    result.status == 402 || result.status == 403 || result.status == 404 ||
                    result.status == 429 || result.status >= 500) stop = true;
            } catch (const std::exception& error) {
                row["outcome"] = cancel.stopped ? "cancelled" : "inconclusive";
                row["detail"] = error.what(); stop = true;
            }
            row["elapsed_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin).count();
        }
        row = Json::parse(redact(row.dump(), options.provider));
        report["results"].push_back(row);
        if (notify) notify(row);
    }
    report["cancelled"] = cancel.stopped.load();
    return Json::parse(redact(report.dump(), options.provider));
}
}
