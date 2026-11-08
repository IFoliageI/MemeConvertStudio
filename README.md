# MemeConvertStudio Local Redraw Plugin

This branch is a standalone build project for the local-redraw command extension.

## Contents

- `plugin.cpp`: registers the `refine-result` host command.
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
dist/plugins/local-redraw/local-redraw.dll
dist/plugins/local-redraw/plugin.json
```

Copy the `plugins` directory from `dist` beside a compatible MemeConvertStudio executable. In Plugin Manager, rescan, enable **局部重绘**, and apply. The plugin is disabled by default.

## Host Requirement

The DLL registers the `refine-result` command and asks the host to open its redraw workflow. Image selection, region handling, provider calls, frame assembly, and output saving are host services, not implemented inside this small command plugin. Use a MemeConvertStudio host build that includes the local-redraw service. The full host implementation is on the `main` branch.

The ABI callback lifetime and ownership contract are documented in `include/memestudio_plugin.h`.
