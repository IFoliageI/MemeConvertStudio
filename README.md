# MemeConvertStudio Example Plugin

This branch is a standalone build project for the sample character-identity prompt extension.

## Contents

- `plugin.cpp`: prompt hook and identity-note command implementation.
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
dist/plugins/example/example.dll
dist/plugins/example/plugin.json
```

Copy the `plugins` directory from `dist` beside a compatible MemeConvertStudio executable. In Plugin Manager, rescan, enable **Example extension**, and apply. The plugin is disabled by default.

## Behavior

The prompt hook adds a short instruction to preserve distinctive facial features. The `identity-note` command appends a character-consistency requirement to the host's additional-instructions field. This is an example extension, not a replacement for the application's image-editing pipeline.

The implementation is intentionally small. The plugin ABI contract and callback ownership rules are documented in `include/memestudio_plugin.h`; the complete application and host plugin manager are on the `main` branch.
