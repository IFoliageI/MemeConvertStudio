#include "studio.hpp"
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
    if (!suffix(L"/images/edits")) base += suffix(L"/images") ? L"/edits" : L"/images/edits";
    return base + query;
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
Multipart multipart(const Provider& provider, const std::string& text, const std::vector<Upload>& images) {
    if (images.empty() || images.size() > 16) throw Error("上传图片数量必须在 1 到 16 之间");
    if (!safeName(provider.imageField)) throw Error("无效的图片上传字段名称");
    auto first = provider.model.find_first_not_of(" \t\r\n");
    auto last = provider.model.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) throw Error("请填写模型名称");
    auto model = provider.model.substr(first, last - first + 1);
    if (std::any_of(model.begin(), model.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error("模型名称不能包含控制字符");
    if (!provider.extra.is_object()) throw Error("扩展参数必须是 JSON 对象");
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
    field("model", model);
    field("prompt", text);
    if (!provider.quality.empty()) field("quality", provider.quality);
    if (!provider.size.empty()) field("size", provider.size);
    for (const auto& [key, value] : provider.extra.items()) {
        if (key == "model" || key == "prompt" || key == "image" || key == "image[]" ||
            key == provider.imageField || key == "n" || key == "stream")
            throw Error("扩展参数不能覆盖 model、prompt、image、n 或 stream");
        if (key == "quality" || key == "size") throw Error("请通过对应选项设置 quality 和 size");
        if (!value.is_null()) field(key, value.is_string() ? value.get<std::string>() : value.dump());
    }
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
struct Response { DWORD status; Bytes data; };
static Response request(const std::wstring& url, const wchar_t* method, const std::wstring& headers,
    const Bytes& body, int timeout, Cancel& cancel) {
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
    auto fail = [&] {
        DWORD error = GetLastError(); cancel.verify();
        if (error == ERROR_WINHTTP_TIMEOUT)
            throw Error("请求超时。任务已保留；供应商可能已计费，请确认后再续作。");
        throw Error("网络请求失败，Windows 错误 " + std::to_string(error));
    };
    if (!WinHttpSendRequest(handle, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<BYTE*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) fail();
    if (!WinHttpReceiveResponse(handle, nullptr)) fail();
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) fail();
    Bytes data; std::array<BYTE, 65536> buffer{};
    for (;;) {
        cancel.verify();
        DWORD count = 0;
        if (!WinHttpReadData(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count)) fail();
        if (!count) break;
        if (data.size() + count > 180 * 1024 * 1024) throw Error("响应超过 180 MB");
        data.insert(data.end(), buffer.begin(), buffer.begin() + count);
    }
    cancel.verify();
    return {status, std::move(data)};
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
Image edit(const Provider& provider, const std::string& text, const std::vector<Upload>& images, Cancel& cancel) {
    auto body = multipart(provider, text, images);
    auto headers = authorization(provider);
    auto response = request(editUrl(provider.baseUrl), L"POST",
        headers + L"\r\nContent-Type: " + wide(body.type),
        body.data, provider.timeoutSeconds, cancel);
    auto result = jsonResponse(response, provider);
    if (!result.contains("data") || !result["data"].is_array() || result["data"].empty())
        throw Error("供应商响应缺少 data[0] 图像");
    const auto& item = result["data"][0];
    if (item.contains("b64_json") && item["b64_json"].is_string())
        return decode(unbase64(item["b64_json"].get<std::string>()));
    if (item.contains("url") && item["url"].is_string()) {
        auto url = item["url"].get<std::string>();
        if (url.rfind("data:", 0) == 0) return decode(unbase64(url));
        auto download = request(wide(url), L"GET", L"", {}, provider.timeoutSeconds, cancel);
        if (download.status != 200) throw Error("生成图片下载失败，HTTP " + std::to_string(download.status));
        return decode(download.data);
    }
    throw Error("供应商响应缺少 b64_json 或 url 图像");
}
}
