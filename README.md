# MemeConvertStudio Waifu2x Plugin

This branch is a standalone build project for the optional Waifu2x input/output upscale command extension.

## Contents

- `plugin.cpp`: registers the `configure-upscale` host command.
- `plugin.json`: host discovery manifest.
- `include/memestudio_plugin.h`: required version 1 C ABI declarations.
- `build.ps1`: builds only this plugin and stages the DLL with its manifest.

## Build

On Windows x64, open a Visual Studio Developer PowerShell with the C++ desktop workload and Windows SDK installed, then run:

```powershell
.\build.ps1
```

The script uses the latest Visual Studio x64 C++ compiler and writes:

```text
dist/plugins/waifu2x/waifu2x.dll
dist/plugins/waifu2x/plugin.json
```

Copy the `plugins` directory from `dist` beside a compatible MemeConvertStudio executable. In Plugin Manager, rescan, enable **Waifu2x 输入/输出超分**, and apply. The plugin is disabled by default.

## Host Requirement

The DLL registers the `configure-upscale` command and asks the host to open its Waifu2x configuration workflow. Process discovery, command-line invocation, temporary files, image conversion, and input/output scaling are host services, not implemented inside this command plugin. Install `waifu2x-ncnn-vulkan` separately; this branch does not redistribute that third-party executable or model files. Use a MemeConvertStudio host build that includes the Waifu2x service. The full host implementation is on the `main` branch.

The ABI callback lifetime and ownership contract are documented in `include/memestudio_plugin.h`.
