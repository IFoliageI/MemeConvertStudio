#include "../src/studio.hpp"
#include <map>

int wmain(int argc, wchar_t** argv) {
    using namespace studio;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int code = 0;
    try {
        check(initialized, "Mock COM");
        std::map<std::wstring, std::wstring> args;
        for (int i = 1; i < argc; ++i) {
            auto key = std::wstring(argv[i]);
            if (key == L"-x") args[key] = L"true";
            else if (i + 1 < argc) args[key] = argv[++i];
            else throw Error("Missing argument");
        }
        auto source = decode(fs::path(args.at(L"-i"))).frames.front().image;
        int scale = std::stoi(args.at(L"-s"));
        if (args.at(L"-f") != L"png" || !fs::is_directory(args.at(L"-m"))) throw Error("Invalid mock contract");
        wchar_t log[32768]{}, mode[40]{};
        GetEnvironmentVariableW(L"MEMESTUDIO_WAIFU_TEST_LOG", log, 32768);
        GetEnvironmentVariableW(L"MEMESTUDIO_WAIFU_TEST_MODE", mode, 40);
        if (*log) {
            HANDLE file = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw Error("Mock log");
            Json fields{{"width", source.width}, {"height", source.height}, {"scale", scale},
                {"noise", utf8(args.at(L"-n"))}, {"tile", utf8(args.at(L"-t"))},
                {"models", utf8(args.at(L"-m"))}, {"gpu", utf8(args.contains(L"-g") ? args.at(L"-g") : L"auto")},
                {"tta", args.contains(L"-x")}};
            auto line = fields.dump() + "\n"; DWORD written{};
            WriteFile(file, line.data(), DWORD(line.size()), &written, nullptr); CloseHandle(file);
        }
        if (std::wstring(mode) == L"fail") throw Error("Requested failure");
        if (std::wstring(mode) == L"timeout") Sleep(15000);
        if (std::wstring(mode) == L"wrong-size") scale = 1;
        savePng(args.at(L"-o"), resized(source, source.width * scale, source.height * scale));
    } catch (...) { code = 7; }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return code;
}
