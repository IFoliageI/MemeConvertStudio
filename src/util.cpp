#include "studio.hpp"
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace studio {
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << operation << " (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        throw Error(message.str());
    }
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw Error("Invalid UTF-16 text");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!count) return L"响应包含无效 UTF-8 文本";
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}
Bytes readFile(const fs::path& path, size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw Error("Cannot open: " + utf8(path.wstring()));
    auto length = file.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > limit)
        throw Error("File exceeds size limit: " + utf8(path.wstring()));
    Bytes data(static_cast<size_t>(length));
    file.seekg(0);
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), data.size()))
        throw Error("Cannot read file");
    return data;
}
void writeFile(const fs::path& path, const Bytes& data) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    fs::path temporary = path; temporary += L".part";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(data.data()), data.size()))
            throw Error("Cannot write: " + utf8(path.wstring()));
        file.flush();
        if (!file) throw Error("Cannot flush file");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Error("Cannot finalize: " + utf8(path.wstring()));
}
void writeJson(const fs::path& path, const Json& data) {
    auto text = data.dump(2);
    writeFile(path, Bytes(text.begin(), text.end()));
}
Json readJson(const fs::path& path) {
    auto data = readFile(path, 4 * 1024 * 1024);
    return Json::parse(data.begin(), data.end());
}
std::string digest(const Bytes& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Error("Cannot initialize SHA-256");
    unsigned char hash[32]{};
    auto status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw Error("SHA-256 failed");
    std::ostringstream result;
    for (auto byte : hash) result << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    return result.str();
}
std::string base64(const Bytes& data) {
    DWORD size = 0;
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size)) throw Error("Base64 failed");
    std::string result(size, '\0');
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &size);
    result.resize(size);
    return result;
}
Bytes unbase64(const std::string& input) {
    std::string data = input;
    if (data.rfind("data:", 0) == 0) {
        auto comma = data.find(',');
        if (comma == std::string::npos) throw Error("Invalid image data URL");
        data = data.substr(comma + 1);
    }
    DWORD size = 0;
    if (data.size() > 180 * 1024 * 1024 ||
        !CryptStringToBinaryA(data.c_str(), static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &size, nullptr, nullptr))
        throw Error("Invalid base64 image");
    Bytes result(size);
    if (!CryptStringToBinaryA(data.c_str(), static_cast<DWORD>(data.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, result.data(), &size, nullptr, nullptr))
        throw Error("Invalid base64 image");
    result.resize(size);
    return result;
}
std::string protect(const std::wstring& key) {
    auto text = utf8(key);
    DATA_BLOB input{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(text.data())}, output{};
    if (!CryptProtectData(&input, L"MemeStudio API key", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output)) throw Error("Cannot protect API key");
    Bytes bytes(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    SecureZeroMemory(text.data(), text.size());
    return base64(bytes);
}
std::wstring unprotect(const std::string& data) {
    if (data.empty()) return {};
    auto bytes = unbase64(data);
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), bytes.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output)) throw Error("Cannot unlock saved API key");
    auto result = wide(std::string(reinterpret_cast<char*>(output.pbData), output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
fs::path settingsPath() {
    PWSTR directory = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory), "LocalAppData");
    fs::path path = fs::path(directory) / L"MemeStudio" / L"settings.json";
    CoTaskMemFree(directory);
    return path;
}
}
