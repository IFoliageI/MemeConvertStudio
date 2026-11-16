// Exercise the actual launcher and Job Object with lightweight Python workers.
#include "../plugins/qwen-image/plugin.cpp"
#include <iostream>

void snapshot(HWND window, const fs::path& path) {
    RECT rect{}; GetClientRect(window, &rect);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels{}; HDC dc = GetDC(window), target = CreateCompatibleDC(dc);
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(target, bitmap);
    require(PrintWindow(window, target, PW_CLIENTONLY) != FALSE, "Dialog capture failed");
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + rect.right * rect.bottom * 4;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels), rect.right * rect.bottom * 4);
    SelectObject(target, old); DeleteObject(bitmap); DeleteDC(target); ReleaseDC(window, dc);
    require(bool(output), "Cannot save dialog capture");
}
int wmain(int argc, wchar_t** argv) {
    try {
        module = GetModuleHandleW(nullptr);
        if (argc == 3 && std::wstring(argv[1]) == L"--ui") {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            MemeStudioHostV1 api{sizeof(api), MEMESTUDIO_PLUGIN_ABI};
            require(initialize(&api) == 0, "Cannot initialize plugin");
            struct Template { DLGTEMPLATE dialog{}; WORD menu = 0, type = 0, title = 0; } resource;
            resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
            auto window = CreateDialogIndirectParamW(module, &resource.dialog, nullptr, procedure, 0);
            require(window != nullptr, "Cannot create plugin dialog");
            ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
            require(GetDlgItem(window, InspectModel) && GetDlgItem(window, GGUFOptions), "Missing model controls");
            snapshot(window, fs::path(argv[2]) / L"qwen-dialog.bmp");
            DestroyWindow(window);
            window = CreateDialogIndirectParamW(module, &resource.dialog, nullptr, nativeOptions, 0);
            require(window != nullptr, "Cannot create native settings");
            ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
            require(GetDlgItem(window, VisionFile) && GetDlgItem(window, Turbo), "Missing GGUF controls");
            snapshot(window, fs::path(argv[2]) / L"qwen-native-dialog.bmp");
            DestroyWindow(window); shutdown(); CoUninitialize();
            std::cout << "Qwen dialogs rendered\n";
            return 0;
        }
        require(argc >= 4, "Expected Python, worker, marker and optional crash mode");
        Child child;
        child.launch(argv[1], argv[2], L"", Json{{"marker",utf8(argv[3])}}, false);
        for (int i = 0; i < 100 && !fs::exists(argv[3]); ++i) Sleep(50);
        require(fs::exists(argv[3]), "Worker did not start");
        auto ids = Json::parse(read(argv[3]));
        Handle worker(OpenProcess(SYNCHRONIZE, FALSE, ids["child"].get<DWORD>()));
        Handle grandchild(OpenProcess(SYNCHRONIZE, FALSE, ids["grandchild"].get<DWORD>()));
        require(worker.value && grandchild.value, "Could not observe worker tree");
        if (argc > 4) TerminateProcess(GetCurrentProcess(), 47);
        child.stop();
        require(WaitForSingleObject(worker.value, 3000) == WAIT_OBJECT_0, "Worker leaked");
        require(WaitForSingleObject(grandchild.value, 3000) == WAIT_OBJECT_0, "Grandchild leaked");
        std::cout << "Job Object explicit stop passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
