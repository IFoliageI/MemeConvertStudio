# MemeConvertStudio

Windows 原生 C++20 表情包与 GIF 人物替换工具。程序通过可配置的 OpenAI
兼容图像接口，将原始表情中的人物替换为参考截图或立绘，同时尽量保留动作、
构图、背景、文字和动画时序。

本项目包含主程序、插件 SDK、可选插件和 Windows 自动构建流程。外部模型、
推理运行时、供应商服务和超分程序均不随主程序打包。

## 文档导航

- [使用与配置](#使用与配置)
- [插件能力](#插件能力)
- [供应商接口](#供应商接口)
- [构建与测试](#构建与测试)
- [安全、隐私与限制](#安全隐私与限制)
- [插件开发](PLUGINS.md)
- [验证记录](VALIDATION.md)
- [第三方项目与鸣谢](THIRD_PARTY.md)

## 系统要求

- Windows 10 或 Windows 11
- x64 或 ARM64
- 使用 MSVC 的 C++20 工具链和 Windows SDK（仅源码构建需要）
- 图像供应商提供 OpenAI 兼容的 `/images/edits` 或 `/images/generations`
  接口

主程序不依赖 Qt、FFmpeg、Python 或额外的 C++ 运行时。Qwen-Image 插件和
Waifu2x 插件有各自的外部运行时要求，见对应说明。

## 使用与配置

### 基本流程

1. 导入“原始表情”（PNG、JPEG、GIF、BMP、TIFF 等系统可解码格式）。
2. 导入“参考人物”，可在预览图上拖出人物裁剪区域。
3. 按需添加画风参考图和附加要求；“预设”提供可组合的中文要求模板。
4. 填写供应商 Base URL、API Key 和模型，设置质量、尺寸、流式预览、
   返回格式、压缩率及生成张数。
5. 点击“开始生成”。静态结果保存为 PNG，动画结果保存为 GIF。

原始表情和人物参考支持资源管理器拖入。启用批量转换插件后，原始表情可
一次导入多个文件或文件夹，并在预览下方用 `< 当前 / 总数 >` 切换素材。
左右侧栏之间的分隔条可拖动调整宽度，双击恢复默认值。

### 多面板和插件门控

插件面板为非模态窗口，可以与主界面及其他插件同时操作；重复打开同一面板
只会激活已有窗口。停用插件会关闭其面板并取消相关任务。

插件专属入口仅在插件成功加载后显示。主程序不会因插件未安装而显示不可用
控件，也不会将插件实现复制进主程序。

### GIF 与多帧一致性

程序在本地解析 GIF 的偏移帧、透明区域、disposal、时长和循环设置，然后
逐帧调用图像编辑接口并重新编码。启用“多帧参考”时，每一帧可附带邻近的
已重绘帧和首帧锚点，以改善人物、画风、背景和动作过渡的一致性。

抽帧间隔、最大生成帧数和生成张数均可独立设置。缓存只复用已经完整写入的
帧，取消或失败不会把局部流式图像当作成品。

## 插件能力

| 插件 | 用途 | 独立分支 |
| --- | --- | --- |
| 示例插件 | 演示提示词和像素回调 | [`plugin-example`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-example) |
| 局部重绘 | 对已有 PNG/GIF 进行局部修正并追加画风参考 | [`plugin-local-redraw`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-local-redraw) |
| Waifu2x 输入/输出超分 | 调用用户已有的 `waifu2x-ncnn-vulkan` | [`plugin-waifu2x`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-waifu2x) |
| 批量转换 | 多文件、文件夹、队列预览和批量输出 | [`plugin-batch-convert`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-batch-convert) |
| 无图绘制 | 仅用提示词生成，可选画风参考 | [`plugin-prompt-draw`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-prompt-draw) |
| API 参数支持测试 | 探测单项和组合参数，并估算 Token | [`plugin-api-parameter-test`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-api-parameter-test) |
| API 负载均衡与灾备 | 聚合网关、供应商池、权重、灾备和效果对比 | [`plugin-api-load-balancing`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-api-load-balancing) |
| 本地模型支持 - Qwen-Image | GGUF / Vulkan、Diffusers 和 OpenAI 兼容本地端点 | [`plugin-qwen-image`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-qwen-image) |

独立插件分支只包含对应插件、最小 ABI、构建脚本、说明和自动构建配置。
需要宿主服务的插件仍必须运行包含相应宿主实现的主程序。
Qwen-Image 的模型目录、环境安装和 OpenAI 兼容端点见
[插件说明](plugins/qwen-image/README.md)。

### 聚合网关与供应商池

启用 API 负载均衡与灾备后，请求先进入进程内聚合网关，再根据模型、优先级、
权重、健康状态和估算单价分流到供应商池。该网关使用
`memestudio://local-gateway/v1` 逻辑地址，不监听 HTTP 端口。

每个渠道可以独立获取 `/models`，勾选允许显示和路由的模型。同名模型跨 API
合并；仅由一个供应商支持的模型显示为 `模型ID (供应商别名)`。灾备只在支持
当前模型的渠道之间进行，不会为了切换供应商而自动切换模型。

## 供应商接口

人物替换和带参考图的操作使用 `POST /images/edits` multipart 请求；纯提示词
绘制使用 `POST /images/generations` JSON 请求。程序支持：

- `data[].b64_json` 和 `data[].url` 响应；
- `stream=true`、SSE 局部图像和最终图像事件；
- 独立的 `quality`、`complexity`、`size`、`output_format`、
  `output_compression`；
- 可配置上传字段（默认 `image[]`）和扩展 JSON 参数；
- Base URL 或完整编辑接口地址。

Quality 与 Complexity 是两个独立参数。Format 仅控制供应商返回格式，
Compression 仅在 JPEG/WebP 返回时发送；本地静态成品仍保存为 PNG，动画成品
仍保存为 GIF。供应商是否支持某个参数由其模型和接口决定，程序不会把 HTTP
接受误认为画质或计费能力。

官方协议参考：

- [OpenAI Image Generation Guide](https://developers.openai.com/api/docs/guides/image-generation)
- [OpenAI Images Edit API](https://developers.openai.com/api/reference/resources/images/methods/edit)

## 构建与测试

### GitHub Actions

推送 `main` 或提交面向 `main` 的 Pull Request 会构建 x64 和 ARM64，并上传
四个 Windows 压缩包。插件分支各自只构建对应插件。工作流不调用供应商、不需要
API Key，构建产物默认保留 14 天。

### 本地构建

在 Visual Studio Developer PowerShell 中运行：

```powershell
.\build.ps1
.\package.ps1
```

默认输出 x64。交叉构建 ARM64：

```powershell
.\build.ps1 -Architecture arm64 -HostArchitecture x64
.\package.ps1 -Architecture arm64
```

可用 `-OutputDirectory` 和 `-BinaryDirectory` 将构建与打包结果放入独立目录，
以避免正在运行的程序锁定 DLL。使用 `-Test` 运行 C++ 本地测试：

```powershell
.\build.ps1 -Test
```

模拟供应商集成测试不需要真实 API：

```powershell
node .\tests\integration.mjs
node .\tests\batch.mjs --ui-only
node .\tests\routing.mjs
```

测试不会验证供应商实际权限、计费、画质或模型兼容性。Qwen 和 Waifu2x
另有插件专属检查，详见 [验证记录](VALIDATION.md) 和各插件 README。

### 命令行任务

设置 `OPENAI_API_KEY` 后传入 JSON：

```powershell
$env:OPENAI_API_KEY = "your-key"
Start-Process .\bin\MemeStudio.exe -ArgumentList @("--job", "D:\path\job.json") -Wait
```

最小任务示例：

```json
{
  "source": "D:/images/meme.gif",
  "character": "D:/images/character.png",
  "styles": ["D:/images/style.png"],
  "output": "D:/images/meme-edited.gif",
  "base_url": "https://your-provider.example/v1",
  "model": "your-image-model",
  "api_key_env": "OPENAI_API_KEY",
  "quality": "auto",
  "size": "auto",
  "count": 1,
  "resume": true
}
```

完整字段和插件配置见 [PLUGINS.md](PLUGINS.md)。

## 安全、隐私与限制

- API Key 默认只保存在进程内存；启用记住功能后使用 Windows DPAPI。
- Key 不写入日志、缓存、效果对比报告或插件回调。
- 下载供应商返回的图片时不携带 API Key；程序不跟随接口重定向。
- 本地 Qwen 服务默认只监听回环地址；启用局域网监听前请配置防火墙和访问控制。
- 输入图像最大约 3200 万像素，单边最大 16384 像素；GIF 受帧数和解码内存限制。
- GIF 重新编码为 256 色并使用二值透明度，不能保留完整真彩和半透明。
- 逐帧生成可能产生闪烁、人物细节漂移、文字变化或动作不连续。
- 自动测试使用本机模拟服务，不代表任何第三方供应商的可用性或实际账单。

本项目当前未声明统一的项目级开源许可证。使用、再分发和修改项目自身代码
前请取得相应授权；第三方组件的许可和归属见 [THIRD_PARTY.md](THIRD_PARTY.md)。
