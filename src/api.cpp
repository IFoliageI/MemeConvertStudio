#include "studio.hpp"
#include "api_routing.hpp"
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <cctype>

namespace studio {
struct Internet {
    HINTERNET value = nullptr;
    ~Internet() { if (value) WinHttpCloseHandle(value); }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    explicit Internet(HINTERNET handle) : value(handle) {
        if (!value) throw Error("Cannot initialize HTTP (Windows error " + std::to_string(GetLastError()) + ")");
    }
};
void Cancel::reset() { release(); stopped = false; }
void Cancel::stop() {
    stopped = true;
    std::lock_guard lock(mutex);
    if (request) { WinHttpCloseHandle(request); request = nullptr; }
}
void Cancel::attach(void* handle) {
    std::lock_guard lock(mutex);
    if (stopped) { WinHttpCloseHandle(handle); throw Error("任务已取消"); }
    request = handle;
}
void Cancel::release() {
    std::lock_guard lock(mutex);
    if (request) { WinHttpCloseHandle(request); request = nullptr; }
}
void Cancel::verify() const { if (stopped) throw Error("任务已取消"); }
Cancel::~Cancel() { release(); }
std::wstring editUrl(std::wstring base) {
    auto begin = base.find_first_not_of(L" \r\n\t");
    auto end = base.find_last_not_of(L" \r\n\t");
    if (begin == std::wstring::npos) throw Error("请填写供应商 Base URL");
    base = base.substr(begin, end - begin + 1);
    if (base.find_first_of(L"\r\n#") != std::wstring::npos) throw Error("Base URL 不能包含换行或片段");
    auto queryIndex = base.find(L'?');
    auto query = queryIndex == std::wstring::npos ? L"" : base.substr(queryIndex);
    if (queryIndex != std::wstring::npos) base.resize(queryIndex);
    while (!base.empty() && base.back() == L'/') base.pop_back();
    auto suffix = [&](const wchar_t* part) {
        size_t count = wcslen(part);
        return base.size() >= count && base.compare(base.size() - count, count, part) == 0;
    };
    if (suffix(L"/images/generations")) {
        base.resize(base.size() - wcslen(L"/generations")); base += L"/edits";
    } else if (!suffix(L"/images/edits")) base += suffix(L"/images") ? L"/edits" : L"/images/edits";
    return base + query;
}
std::wstring generationUrl(const std::wstring& base) {
    auto url = editUrl(base);
    auto query = url.find(L'?');
    auto suffix = query == std::wstring::npos ? L"" : url.substr(query);
    if (query != std::wstring::npos) url.resize(query);
    url.resize(url.size() - wcslen(L"/edits"));
    return url + L"/generations" + suffix;
}
std::wstring modelsUrl(const std::wstring& base) {
    auto url = editUrl(base);
    auto query = url.find(L'?');
    auto suffix = query == std::wstring::npos ? L"" : url.substr(query);
    if (query != std::wstring::npos) url.resize(query);
    url.resize(url.size() - wcslen(L"/images/edits"));
    return url + L"/models" + suffix;
}
static void append(Bytes& output, const std::string& text) { output.insert(output.end(), text.begin(), text.end()); }
static bool safeName(const std::string& value) {
    return !value.empty() && value.size() < 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-' || c == '[' || c == ']' || c == '.';
        });
}
static Json imageFields(const Provider& provider, const std::string& text) {
    auto first = provider.model.find_first_not_of(" \t\r\n");
    auto last = provider.model.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) throw Error("请填写模型名称");
    auto model = provider.model.substr(first, last - first + 1);
    if (std::any_of(model.begin(), model.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error("模型名称不能包含控制字符");
    if (!provider.extra.is_object()) throw Error("扩展参数必须是 JSON 对象");
    if (!provider.complexity.empty() && provider.complexity != "low" &&
        provider.complexity != "medium" && provider.complexity != "high")
        throw Error("Complexity 必须为 low、medium、high 或不发送");
    if (!provider.outputFormat.empty() && provider.outputFormat != "png" &&
        provider.outputFormat != "jpeg" && provider.outputFormat != "webp")
        throw Error("返回格式必须为 png、jpeg、webp 或不发送");
    if (provider.compression < -1 || provider.compression > 100)
        throw Error("Compression 必须为 0 到 100");
    if (provider.compression >= 0 && provider.outputFormat != "jpeg" && provider.outputFormat != "webp")
        throw Error("Compression 仅适用于 jpeg 或 webp 返回格式");
    if (provider.stream && (provider.partialImages < 1 || provider.partialImages > 3))
        throw Error("局部预览数量必须为 1 到 3");
    Json fields{{"model", model}, {"prompt", text}};
    if (!provider.quality.empty()) fields["quality"] = provider.quality;
    if (!provider.size.empty()) fields["size"] = provider.size;
    if (!provider.complexity.empty()) fields["complexity"] = provider.complexity;
    if (!provider.outputFormat.empty()) fields["output_format"] = provider.outputFormat;
    if (provider.compression >= 0) fields["output_compression"] = provider.compression;
    if (provider.stream) {
        fields["stream"] = true;
        fields["partial_images"] = provider.partialImages;
    }
    for (const auto& [key, value] : provider.extra.items()) {
        if (!safeName(key)) throw Error("扩展参数字段名无效");
        if (key == "model" || key == "prompt" || key == "image" || key == "image[]" ||
            key == provider.imageField || key == "n" || key == "stream")
            throw Error("扩展参数不能覆盖 model、prompt、image、n 或 stream");
        if (key == "quality" || key == "size" || key == "complexity" || key == "output_format" ||
            key == "output_compression" || key == "partial_images")
            throw Error("请通过对应选项设置 quality、size、complexity、返回格式、压缩率和局部预览数量");
        if (!value.is_null()) fields[key] = value;
    }
    return fields;
}
Json generationBody(const Provider& provider, const std::string& text) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) throw Error("请填写绘制提示词");
    return imageFields(provider, text);
}
Multipart multipart(const Provider& provider, const std::string& text, const std::vector<Upload>& images) {
    if (images.empty() || images.size() > 16) throw Error("上传图片数量必须在 1 到 16 之间");
    if (!safeName(provider.imageField)) throw Error("无效的图片上传字段名称");
    auto fields = imageFields(provider, text);
    GUID guid{}; check(CoCreateGuid(&guid), "Multipart boundary");
    wchar_t name[48]{}; StringFromGUID2(guid, name, 48);
    // StringFromGUID2 includes braces, which are not valid MIME boundary characters.
    std::string boundary = "MemeStudio" + utf8(std::wstring(name + 1, 36));
    Multipart body{"multipart/form-data; boundary=" + boundary, {}};
    auto field = [&](const std::string& key, const std::string& value) {
        if (!safeName(key)) throw Error("扩展参数字段名无效");
        append(body.data, "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" +
            key + "\"\r\n\r\n" + value + "\r\n");
    };
    for (const auto& [key, value] : fields.items())
        field(key, value.is_string() ? value.get<std::string>() : value.dump());
    size_t index = 0;
    for (const auto& image : images) {
        if (image.data.empty() || image.data.size() > 50 * 1024 * 1024)
            throw Error("单张上传图片为空或超过 50 MB");
        append(body.data, "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" +
            provider.imageField + "\"; filename=\"image_" + std::to_string(index++) +
            ".png\"\r\nContent-Type: image/png\r\n\r\n");
        body.data.insert(body.data.end(), image.data.begin(), image.data.end());
        append(body.data, "\r\n");
    }
    append(body.data, "--" + boundary + "--\r\n");
    if (body.data.size() > 180 * 1024 * 1024) throw Error("上传请求超过 180 MB");
    return body;
}
struct Response { DWORD status; Bytes data; bool streamed = false; };
static Response request(const std::wstring& url, const wchar_t* method, const std::wstring& headers,
    const Bytes& body, int timeout, Cancel& cancel,
    const std::function<void(const char*, size_t)>& onChunk = {}) {
    cancel.verify();
    URL_COMPONENTS parts{sizeof(URL_COMPONENTS)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) throw Error("URL 格式无效");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    if (parts.dwUserNameLength || parts.dwPasswordLength) throw Error("URL 不能包含账号或密码");
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(parts.nScheme == INTERNET_SCHEME_HTTP &&
        (host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1")))
        throw Error("供应商和下载地址必须使用 HTTPS（本机调试除外）");
    Internet session(WinHttpOpen(L"MemeStudio/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (timeout < 5 || timeout > 3600) throw Error("请求超时必须在 5 到 3600 秒之间");
    WinHttpSetTimeouts(session.value, 15000, 15000, timeout * 1000, timeout * 1000);
    Internet connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (path.empty()) path = L"/";
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET handle = WinHttpOpenRequest(connection.value, method, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!handle) throw Error("Cannot create HTTP request");
    cancel.attach(handle);
    struct Lease { Cancel& token; ~Lease() { token.release(); } } lease{cancel};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    DWORD disabled = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    auto fail = [&](bool connecting = false) {
        DWORD error = GetLastError(); cancel.verify();
        if (connecting && (error == ERROR_WINHTTP_NAME_NOT_RESOLVED || error == ERROR_WINHTTP_CANNOT_CONNECT))
            throw ApiConnectionError("未能建立供应商连接，Windows 错误 " + std::to_string(error));
        if (error == ERROR_WINHTTP_TIMEOUT)
            throw Error("请求超时。任务已保留；供应商可能已计费，请确认后再续作。");
        throw Error("网络请求失败，Windows 错误 " + std::to_string(error));
    };
    if (!WinHttpSendRequest(handle, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<BYTE*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) fail(true);
    if (!WinHttpReceiveResponse(handle, nullptr)) fail();
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) fail();
    wchar_t contentType[256]{};
    DWORD typeLength = sizeof(contentType);
    bool streamed = onChunk && status >= 200 && status < 300 &&
        WinHttpQueryHeaders(handle, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
            contentType, &typeLength, WINHTTP_NO_HEADER_INDEX) &&
        _wcsnicmp(contentType, L"text/event-stream", 17) == 0;
    Bytes data; std::array<BYTE, 65536> buffer{};
    size_t received = 0;
    for (;;) {
        cancel.verify();
        DWORD readable = static_cast<DWORD>(buffer.size());
        if (streamed) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(handle, &available)) fail();
            if (!available) break;
            readable = std::min(readable, available);
        }
        DWORD count = 0;
        if (!WinHttpReadData(handle, buffer.data(), readable, &count)) fail();
        if (!count) break;
        received += count;
        if (received > 180 * 1024 * 1024) throw Error("响应超过 180 MB");
        if (streamed) onChunk(reinterpret_cast<const char*>(buffer.data()), count);
        else data.insert(data.end(), buffer.begin(), buffer.begin() + count);
    }
    cancel.verify();
    return {status, std::move(data), streamed};
}
static std::wstring authorization(const Provider& provider) {
    if (provider.key.empty()) throw Error("请填写 API Key");
    if (std::any_of(provider.key.begin(), provider.key.end(), [](wchar_t c) { return c < 33 || c > 126; }))
        throw Error("API Key 包含空格、换行或非 ASCII 字符");
    return L"Authorization: Bearer " + provider.key;
}
static Json jsonResponse(const Response& response, const Provider& provider) {
    auto redact = [&](std::string value) {
        auto secret = utf8(provider.key);
        size_t pos = 0;
        while ((pos = value.find(secret, pos)) != std::string::npos) { value.replace(pos, secret.size(), "[REDACTED]"); pos += 10; }
        return value.substr(0, 1600);
    };
    Json result;
    try { result = Json::parse(response.data.begin(), response.data.end()); }
    catch (...) {
        throw Error("HTTP " + std::to_string(response.status) +
            "：供应商返回的不是 JSON，请检查 Base URL 和接口路径");
    }
    if (response.status < 200 || response.status >= 300 || result.contains("error")) {
        std::string detail = result.contains("error") ? result["error"].dump() : result.dump();
        throw Error("HTTP " + std::to_string(response.status) + "：" + redact(detail));
    }
    return result;
}
std::vector<std::string> listModels(const Provider& provider, Cancel& cancel) {
    auto headers = authorization(provider);
    auto response = request(modelsUrl(provider.baseUrl), L"GET", headers, {},
        std::min(provider.timeoutSeconds, 30), cancel);
    auto result = jsonResponse(response, provider);
    if (!result.contains("data") || !result["data"].is_array())
        throw Error("供应商响应缺少 data 模型数组");
    std::vector<std::string> models;
    for (const auto& item : result["data"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) continue;
        auto id = item["id"].get<std::string>();
        if (!id.empty() && id.size() <= 512 &&
            std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            models.push_back(std::move(id));
    }
    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());
    if (models.empty()) throw Error("供应商没有返回可用模型；仍可手动填写模型名称");
    return models;
}
// SSE records can span any number of network reads, including between CR and LF.
class ImageStream {
    const Provider& provider;
    const std::function<void(const Image&, int)>& preview;
    std::string line, data, event;
    bool skipLf = false, firstLine = true, ended = false;
    void dispatch() {
        if (data.empty()) { event.clear(); return; }
        if (data == "[DONE]") { ended = true; data.clear(); event.clear(); return; }
        Json value;
        try { value = Json::parse(data); }
        catch (...) { throw Error("供应商流式事件不是有效 JSON"); }
        auto type = value.value("type", event);
        if (value.contains("error") || type == "error") {
            Json error{{"error", value.contains("error") ? value["error"] : value}};
            auto serialized = error.dump();
            jsonResponse({200, Bytes(serialized.begin(), serialized.end())}, provider);
        }
        if (!ended && (type == "image_edit.partial_image" || type == "image_generation.partial_image")) {
            if (result.empty() && preview && value.contains("b64_json")) {
                auto image = decode(unbase64(value.at("b64_json").get<std::string>()));
                preview(image, value.value("partial_image_index", 0));
            }
        } else if (!ended && (type == "image_edit.completed" || type == "image_generation.completed")) {
            if (!result.empty()) throw Error("供应商重复发送最终图像");
            result = decode(unbase64(value.at("b64_json").get<std::string>()));
        }
        data.clear(); event.clear();
    }
    void finishLine() {
        if (firstLine) {
            if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
            firstLine = false;
        }
        if (line.empty()) dispatch();
        else {
            auto colon = line.find(':');
            auto name = line.substr(0, colon);
            auto value = colon == std::string::npos ? "" : line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.erase(0, 1);
            if (name == "event") event = std::move(value);
            else if (name == "data") { if (!data.empty()) data += '\n'; data += value; }
        }
        line.clear();
    }
public:
    Image result;
    ImageStream(const Provider& settings, const std::function<void(const Image&, int)>& callback)
        : provider(settings), preview(callback) {}
    void feed(const char* bytes, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            char c = bytes[i];
            if (skipLf && c == '\n') { skipLf = false; continue; }
            skipLf = false;
            if (c == '\r' || c == '\n') { finishLine(); skipLf = c == '\r'; }
            else line += c;
            if (line.size() + data.size() > 72 * 1024 * 1024) throw Error("单个流式事件超过 72 MB");
        }
    }
    Image finish() {
        // A final event must have its blank-line terminator; truncated events are never saved.
        if (result.empty()) throw Error("流式传输结束但未收到最终图像；局部预览不会作为成品保存");
        return std::move(result);
    }
};
ApiProbeResult probeImage(const Provider& provider, const std::string& text,
    const std::vector<Upload>& images, bool generation, Cancel& cancel) {
    Multipart body;
    if (generation) {
        auto json = generationBody(provider, text).dump();
        body = {"application/json", Bytes(json.begin(), json.end())};
    } else body = multipart(provider, text, images);
    ApiProbeResult result;
    std::function<void(const Image&, int)> preview = [&](const Image&, int) { ++result.partialImages; };
    ImageStream stream(provider, preview);
    bool streamFailed = false;
    std::string streamError;
    auto response = request(generation ? generationUrl(provider.baseUrl) : editUrl(provider.baseUrl),
        L"POST", authorization(provider) + L"\r\nContent-Type: " + wide(body.type) +
            (provider.stream ? L"\r\nAccept: text/event-stream" : L""),
        body.data, provider.timeoutSeconds, cancel,
        [&](const char* bytes, size_t size) {
            if (streamFailed) return;
            try { stream.feed(bytes, size); }
            catch (const std::exception& error) { streamFailed = true; streamError = error.what(); }
        });
    result.status = response.status; result.streamed = response.streamed;
    try {
        if (response.streamed) {
            if (streamFailed) throw Error(streamError);
            stream.finish();
            result.outcome = "accepted";
            result.detail = "Valid SSE final image; partial images received: " + std::to_string(result.partialImages);
            if (provider.stream && !result.partialImages)
                result.detail += "; partial image count is unverified";
        } else {
            auto json = Json::parse(response.data.begin(), response.data.end(), nullptr, false);
            if (json.is_object() && json.contains("error") && json["error"].is_object() &&
                json["error"].contains("param") && json["error"]["param"].is_string())
                result.errorParameter = json["error"]["param"].get<std::string>();
            auto value = jsonResponse(response, provider);
            if (!value.is_object() || !value.contains("data") || !value["data"].is_array() ||
                value["data"].empty() || !value["data"][0].is_object()) throw Error("Missing data[0] image");
            const auto& item = value["data"][0];
            bool inlineImage = item.contains("b64_json") && item["b64_json"].is_string();
            bool urlImage = item.contains("url") && item["url"].is_string() && !item["url"].get<std::string>().empty();
            if (!inlineImage && !urlImage) throw Error("Missing b64_json or image URL");
            result.outcome = "accepted";
            result.detail = "Image response received; parameter semantics are unverified";
            if (inlineImage) {
                auto bytes = unbase64(item["b64_json"].get<std::string>());
                decode(bytes);
                std::string format;
                if (bytes.size() >= 8 && bytes[0] == 137 && bytes[1] == 'P' && bytes[2] == 'N') format = "png";
                else if (bytes.size() >= 3 && bytes[0] == 255 && bytes[1] == 216) format = "jpeg";
                else if (bytes.size() >= 12 && std::string(bytes.begin(), bytes.begin() + 4) == "RIFF" &&
                    std::string(bytes.begin() + 8, bytes.begin() + 12) == "WEBP") format = "webp";
                if (!provider.outputFormat.empty() && !format.empty() && format != provider.outputFormat) {
                    result.outcome = "ignored";
                    result.detail = "Requested output_format=" + provider.outputFormat + "; received " + format;
                }
            } else result.detail = "Image URL returned; URL contents and parameter semantics unverified (no download)";
            if (provider.stream) {
                result.outcome = "ignored";
                result.detail = "Requested stream=true; received JSON instead of SSE";
            }
        }
    } catch (const std::exception& error) {
        result.detail = error.what();
        result.outcome = response.status == 400 || response.status == 422 ? "request_rejected" : "inconclusive";
    }
    auto secret = utf8(provider.key);
    auto sanitize = [&](std::string& value) {
        if (!secret.empty()) for (size_t pos = 0; (pos = value.find(secret, pos)) != std::string::npos;) {
            value.replace(pos, secret.size(), "[REDACTED]"); pos += 10;
        }
        // Limit on Unicode characters so truncation cannot leave invalid UTF-8 in reports.
        auto textValue = wide(value);
        if (textValue.size() > 900) {
            textValue.resize(899);
            if (textValue.back() >= 0xD800 && textValue.back() <= 0xDBFF) textValue.pop_back();
            textValue += L"…";
        }
        value = utf8(textValue);
    };
    sanitize(result.detail); sanitize(result.errorParameter);
    return result;
}
static Image imageRequest(const Provider& provider, const std::wstring& url, const Multipart& body,
    Cancel& cancel, const std::function<void(const Image&, int)>& preview) {
    auto headers = authorization(provider);
    ImageStream stream(provider, preview);
    auto response = request(url, L"POST",
        headers + L"\r\nContent-Type: " + wide(body.type) +
            (provider.stream ? L"\r\nAccept: text/event-stream" : L""),
        body.data, provider.timeoutSeconds, cancel, provider.stream ?
            std::function<void(const char*, size_t)>([&](const char* bytes, size_t size) { stream.feed(bytes, size); }) :
            std::function<void(const char*, size_t)>{});
    if (response.status < 200 || response.status >= 300) {
        try { jsonResponse(response, provider); }
        catch (const std::exception& error) { throw ApiHttpError(response.status, error.what()); }
    }
    if (response.streamed) return stream.finish();
    auto result = jsonResponse(response, provider);
    if (!result.contains("data") || !result["data"].is_array() || result["data"].empty())
        throw Error("供应商响应缺少 data[0] 图像");
    const auto& item = result["data"][0];
    if (item.contains("b64_json") && item["b64_json"].is_string())
        return decode(unbase64(item["b64_json"].get<std::string>()));
    if (item.contains("url") && item["url"].is_string()) {
        auto imageUrl = item["url"].get<std::string>();
        if (imageUrl.rfind("data:", 0) == 0) return decode(unbase64(imageUrl));
        Response download;
        try { download = request(wide(imageUrl), L"GET", L"", {}, provider.timeoutSeconds, cancel); }
        catch (const ApiConnectionError& error) { throw Error("生成图片下载失败：" + std::string(error.what())); }
        if (download.status != 200) throw Error("生成图片下载失败，HTTP " + std::to_string(download.status));
        return decode(download.data);
    }
    throw Error("供应商响应缺少 b64_json 或 url 图像");
}
Image edit(const Provider& provider, const std::string& text, const std::vector<Upload>& images, Cancel& cancel,
    const std::function<void(const Image&, int)>& preview) {
    if (provider.gateway) return provider.gateway->forward(provider, cancel, [&](const Provider& target) {
        return edit(target, text, images, cancel, preview);
    });
    if (provider.router) return provider.router->invoke(provider, cancel, [&](const Provider& target) {
        return edit(target, text, images, cancel, preview);
    });
    return imageRequest(provider, editUrl(provider.baseUrl), multipart(provider, text, images), cancel, preview);
}
Image generate(const Provider& provider, const std::string& text, const std::vector<Upload>& styles,
    Cancel& cancel, const std::function<void(const Image&, int)>& preview) {
    if (provider.gateway) return provider.gateway->forward(provider, cancel, [&](const Provider& target) {
        return generate(target, text, styles, cancel, preview);
    });
    if (provider.router) return provider.router->invoke(provider, cancel, [&](const Provider& target) {
        return generate(target, text, styles, cancel, preview);
    });
    if (!styles.empty()) return edit(provider, text, styles, cancel, preview);
    auto serialized = generationBody(provider, text).dump();
    return imageRequest(provider, generationUrl(provider.baseUrl),
        {"application/json", Bytes(serialized.begin(), serialized.end())}, cancel, preview);
}
}
