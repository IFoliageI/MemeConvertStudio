# 验证记录

本文件记录当前主分支的可复现验证范围。它不是对所有供应商、模型、硬件或
生成质量的保证，也不替代第三方项目自己的测试和许可证要求。

## 最近验证

验证日期：2026-11-16
主机：Windows x64，MSVC C++20，Windows SDK
构建目标：x64、ARM64 交叉编译
供应商：本机 HTTP 模拟服务；未调用真实付费 API

| 项目 | 结果 | 范围 |
| --- | --- | --- |
| 主程序与插件构建 | 通过 | x64 / ARM64，全部八个插件，无编译警告 |
| 多面板界面 | 通过 | 六个宿主面板与 Qwen 原生面板并行打开、重复激活、独立关闭和清理 |
| 模型目录与路由 | 通过 | 模型并集、独家供应商标记、单 API 多 Key 去重、勾选保存、同模型灾备 |
| 批量与 GIF | 通过 | 多文件队列、序号切换、GIF 时长/循环、缓存续作和失败恢复 |
| 参数与流式 | 通过 | 参数组合探测、Token 估算、SSE 局部图、断流和密钥脱敏 |
| Qwen 本地插件 | 通过 | GGUF / Vulkan 一次真实本地推理、驻留、手动卸载和崩溃清理 |
| Waifu2x 接口 | 通过 | 用户提供的 `waifu2x-ncnn-vulkan` 实机超分及进程终止 |
| GitHub Actions | 已配置 | `main` 和插件分支的 Windows 构建工作流 |

## 定向命令

```powershell
node tests\batch.mjs --ui-only --exe=D:\Projects\MemeStudio\bin\panels\MemeStudio.exe
node tests\routing.mjs --exe=D:\Projects\MemeStudio\bin\panels\MemeStudio.exe
```

以下检查仅在需要验证对应插件时运行：

```powershell
node tests\drawing.mjs
node tests\source-batch.mjs
node tests\refine-upscale.mjs
node tests\api-probe.mjs
python tests\qwen_test.py
node tests\waifu-real.mjs --tool=D:\path\to\waifu2x-ncnn-vulkan-20250915-windows
```

测试输出写入 `tmp/`，不应提交到仓库。测试脚本使用模拟供应商时，不会产生
真实 API 费用。

## 已知未覆盖范围

- 未在 ARM64 实体 Windows 上运行完整 GUI 和本地模型推理；仅完成交叉编译与
  PE 架构检查。
- 未验证任意第三方供应商的模型权限、计费、真实 GIF 能力、参数兼容性或生成质量。
- Qwen Diffusers、其他 GPU 后端、复杂人物素材和长 GIF 未做完整性能基准。
- 实际精准触控板体验取决于 Windows 驱动；自动化检查覆盖消息处理和缩放逻辑，
  不等同于所有硬件手势测试。
- 多帧参考只能降低帧间漂移，不能保证动作、文字或人物细节完全一致。

## 复现原则

验证应优先使用本机模拟服务、临时输出目录和独立构建目录。不要将真实 API Key、
模型权重、虚拟环境、日志、缓存或用户素材提交到 Git。需要真实本地模型时，
在验证报告中注明模型来源、运行时版本、硬件和是否产生外部请求。
