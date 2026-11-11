# MemeConvertStudio 无图绘制插件

这是 `memestudio.prompt-draw` 的独立源码分支，只包含本插件、ABI 头文件、
构建脚本和自动编译工作流。主程序及图像生成工作流位于仓库的 `main` 分支。

## 功能

- 只填提示词即可调用供应商生成图像，无需原始表情或人物图。
- 可选最多 8 张画风参考，支持多选、拖入、移除和预览；GIF 参考只取首帧。
- 不带参考图时使用 `/images/generations` JSON 请求；
  带参考图时使用 `/images/edits` multipart 请求，仅上传画风参考。
- 复用宿主当前供应商、模型、尺寸、Quality、Complexity、Format、
  Compression、流式预览和生成张数。
- 成品保留供应商返回尺寸，本地保存 PNG，支持缓存续作与可选 Waifu2x 超分。

画风参考通过提示词限定为线条、配色、渲染、材质参考，主体和构图由文本指定；
实际遵循程度取决于供应商和模型。本插件不生成动画 GIF，也不自动重试付费请求。

## 宿主要求

本 DLL 是命令入口适配器：`draw` 返回 `{"action":"prompt_draw"}`。
独立绘制窗口、API 请求、流式解析、缓存和输出由宿主提供，插件不持有供应商密钥。
必须使用包含无图绘制服务的 `MemeStudio-drawing.exe` 或更新构建；
仅将 DLL 安装到旧版主程序无法新增宿主服务。

## 构建

需要 Windows x64、Visual Studio C++ 桌面开发工具和 Windows SDK。
在本分支运行：

```powershell
.\build.ps1
```

脚本仅构建本插件，不下载依赖，不构建主程序。中间文件位于 `build/`，
可安装产物位于：

```text
dist/plugins/prompt-draw/prompt-draw.dll
dist/plugins/prompt-draw/plugin.json
```

把 `dist/plugins/prompt-draw` 放到主程序旁的 `plugins` 目录，
在“插件管理”重新扫描、勾选“无图绘制”并应用。插件默认关闭。

## 使用

1. 在主界面填写供应商、API Key、模型及生成参数、张数。
2. 点击顶部“无图绘制…”，或在插件管理执行“通过提示词绘制…”。
3. 填写绘制提示词，可添加或移除本轮画风图。初始带入主界面附加要求和画风参考。
4. 选择 `.png` 保存路径，点击“开始绘制”。生成过程和成品出现在主窗口结果预览。

绘制窗口修改不写回主界面公共输入，取消不发起请求。
默认保存到系统“图片”目录，可自行更改。多张输出使用 `_001`、`_002` 编号。
不得用输出覆盖画风参考；已有成品先询问再替换。
取消或失败保留已保存的成品与最终图缓存，重跑相同参数、输出路径可续作。
关闭主界面“复用已完成帧”可发起全新生成。
已启用 Waifu2x 时，输入超分只处理画风参考（无图时跳过），输出超分处理成品。
可用“局部重绘”对输出继续修正。

## 自动编译

推送到本分支或向本分支提交 PR 时运行 `Build Plugin`，
也可在 Actions 中选择本分支并点击 `Run workflow`。
工作流只编译本插件，不运行测试或调用图像 API。
在运行详情的 Artifacts 下载 `MemeStudio-Plugin-Windows-x64`，
解压后把 `plugins` 目录放到兼容主程序旁。产物保存 14 天。

## 文件

```text
plugin.cpp                     命令入口
plugin.json                    插件清单
include/memestudio_plugin.h     ABI v1
build.ps1                      单插件构建
.github/workflows/build.yml    自动编译
README.md                      使用说明
.gitignore                     忽略本地构建产物
```
