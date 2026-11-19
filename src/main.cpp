#include "studio.hpp"
#include "dsh.hpp"
#include "job_config.hpp"
#include "api_probe.hpp"

int wmain(int argc, wchar_t** argv) {
    using namespace studio;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 0;
    try {
        check(initialized, "Initialize COM");
        if (argc >= 3 && std::wstring(argv[1]) == L"--self-test") {
            exitCode = selfTest(argv[2]);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--network-test") {
            exitCode = networkTest(argv[2], argv[3]);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--parameter-probe-test") {
            exitCode = parameterProbeTest(argv[2], argv[3]);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--routing-test") {
            exitCode = routingTest(argv[2], argv[3]);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--dsh-test") {
            exitCode = runDshTest(argv[3], argv[2]);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--inspect") {
            auto animation = decode(fs::path(argv[2]));
            Json info{{"width", animation.frames.front().image.width}, {"height", animation.frames.front().image.height},
                {"animated", animation.animated}, {"loop", animation.loop}, {"delays", Json::array()}};
            for (const auto& frame : animation.frames) info["delays"].push_back(frame.delayMs);
            writeJson(argv[3], info);
        } else if (argc >= 3 && std::wstring(argv[1]) == L"--ui-check") {
            exitCode = runGui(GetModuleHandleW(nullptr), argv[2], true);
        } else if (argc >= 4 && std::wstring(argv[1]) == L"--list-models") {
            auto config = readJson(argv[2]);
            Provider provider;
            provider.baseUrl = wide(config.at("base_url").get<std::string>());
            wchar_t key[8192]{};
            auto name = wide(config.value("api_key_env", "OPENAI_API_KEY"));
            DWORD count = GetEnvironmentVariableW(name.c_str(), key, 8192);
            if (count && count < 8192) provider.key = key;
            SecureZeroMemory(key, sizeof(key));
            Cancel cancel;
            writeJson(argv[3], listModels(provider, cancel));
        } else if (argc >= 3 && std::wstring(argv[1]) == L"--job") {
            Json config = readJson(argv[2]);
            std::shared_ptr<Plugins> plugins;
            if (config.contains("plugins"))
                plugins = std::make_shared<Plugins>(discoverPlugins(configuredPluginDirectory(config)), configuredPlugins(config));
            Cancel cancel;
            runConfiguredJob(config, configuredJob(config, std::move(plugins)), cancel);
        } else {
            exitCode = runGui(GetModuleHandleW(nullptr), argc >= 3 && std::wstring(argv[1]) == L"--demo" ? argv[2] : fs::path{});
        }
    } catch (const std::exception& error) {
        if (argc > 1) {
            fs::path errorPath = fs::path(argv[argc - 1]).parent_path() / L"MemeStudio-error.txt";
            try {
                auto message = std::string(error.what()); writeFile(errorPath, Bytes(message.begin(), message.end()));
            } catch (...) {}
        } else MessageBoxW(nullptr, wide(error.what()).c_str(), L"Meme Studio", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return exitCode;
}
