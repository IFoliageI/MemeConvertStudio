# MemeConvertStudio

MemeConvertStudio 是 Windows 原生 C++20 表情包与 GIF 人物替换工具。它通过可配置供应商的 OpenAI 兼容图像接口，将原表情主体替换为截图或立绘中的人物，并保留原动作、构图和背景；也可添加画风参考、组合中文提示预设、逐帧处理 GIF，或通过插件进行无图绘制、局部重绘与可选超分。

## 启动

支持 Windows 10/11 x64。获取源码后，在 Visual Studio Developer PowerShell 中运行：

```powershell
.\build.ps1
```

构建需要 Visual Studio 的“使用 C++ 的桌面开发”工作负载及 Windows SDK。成功后启动 `bin\MemeStudio.exe`。程序使用静态 C++ 运行库，不需要 Qt、Python、FFmpeg 或额外运行时。仓库跟踪源码，不跟踪本地 `bin/` 构建产物；构建和打包说明见“构建与测试”。

完整应用源码、SDK 和整合构建流程位于 `main` 分支。为了让单个插件可独立查看和构建，仓库另有六个精简插件分支；它们不包含应用宿主源码或其他插件：

- [`plugin-example`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-example)：示例提示词插件。
- [`plugin-local-redraw`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-local-redraw)：局部重绘命令插件。
- [`plugin-waifu2x`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-waifu2x)：Waifu2x 输入/输出超分命令插件。
- [`plugin-batch-convert`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-batch-convert)：多源图片和 GIF 批量转换插件。
- [`plugin-prompt-draw`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-prompt-draw)：纯提示词及可选画风参考的无图绘制插件。
- [`plugin-api-parameter-test`](https://github.com/IFoliageI/MemeConvertStudio/tree/plugin-api-parameter-test)：逐项探测模型、端点及已配置图像参数的支持情况。

每个插件分支只包含对应插件的 `plugin.cpp`、`plugin.json`、最小 ABI 头文件、单插件 `build.ps1`、专属 README 和自动编译工作流。运行构建脚本会在 `dist/plugins/<插件目录>/` 生成可安装的 DLL 与清单。局部重绘、超分、批量转换、无图绘制和参数测试插件是宿主工作流的入口适配器，需使用包含对应服务的 MemeConvertStudio 主程序；它们不会将宿主实现复制进插件仓库。

1. 导入原始表情，或将一个文件拖入“原始表情”区域，支持 PNG、JPEG、GIF、BMP、TIFF；其他格式取决于本机 WIC 解码器。
2. 导入人物截图或立绘，也可以直接拖入“参考人物”区域。在人物预览上拖出矩形，可以只使用截图中的人物区域；“重置裁剪”恢复完整图片。
3. 按需添加最多 8 张画风参考图，并填写附加要求。右侧“预设…”可选择常用要求。
4. 填写供应商 Base URL、API Key 和模型名称。模型框可直接输入供应商的自定义模型名。
5. 在参数底部设置生成张数，选择输出路径，点击“开始生成”。静态图导出 PNG，动画导出 GIF。

模型框旁的“获取模型”会使用当前 Base URL 和 API Key 调用供应商的 `GET /models`，自动去重并填入下拉框，优先显示名称包含 `gpt-image` 的模型。支持输入完整编辑接口地址，保留原模型选择和手动输入；获取过程中可取消，失败时不清空已有选项。获取超时最多 30 秒。列表可能包含非图像模型，不代表所有模型都支持图片编辑。

开始任务时会在日志中显示实际模型名称，自动清理模型名首尾空白，并在发送前拦截空名称。上传分隔符使用标准 MIME 字符，兼容严格解析 multipart 的服务。

原图和生成结果可切换预览，支持播放、暂停和逐帧拖动。生成完成后可打开文件或所在目录。

## 附加要求预设

“附加要求”右侧的“预设…”提供 30 个内置预设，分为人物与构图、画风、情绪与动作、
文字、GIF 连贯性、细节修正六类。内容参考所提供文档的 Q 版半身像、服装头饰还原、
背景和情绪设计，并补充角色替换、场景保留、多帧一致性及手部修正等要求。
预设适配当前单幅图片或逐帧转换流程，不会要求生成 4×6 拼图。

选中条目即可预览完整要求，默认“追加”保留已填写内容；可多次打开组合不同要求。
“替换全部”用所选预设覆盖原内容，“取消”不修改。应用后仍可直接编辑文本或撤销，
预设不会改变供应商参数、“保留原表情文字”开关、生成张数或输出尺寸。
情绪与动作类会主动改变原表情的情绪或姿势，组合时请避免相互矛盾的要求。
预设是发送给图像模型的提示要求，实际效果取决于模型。

## 批量生成与插件

插件专属控件只在对应插件勾选、应用并成功加载后显示：
局部重绘、无图绘制与参数测试显示顶部入口，批量转换显示目录按钮和素材翻页。
停用或加载失败后隐藏对应控件，顶部入口自动紧凑排列；
任务执行时已加载的入口保持显示并暂时禁用。

### 无图绘制

在“插件管理”勾选“无图绘制”并应用，然后点击主窗口顶部“无图绘制…”；
也可在插件管理执行“通过提示词绘制…”。无需导入原始表情或人物图。

在独立窗口填写绘制提示词，按需添加最多 8 张画风参考。参考支持多选或拖入，
可移除、点击列表预览；GIF 参考只取第一帧。窗口初始带入主界面的附加要求和画风图，
本轮修改在绘制窗口内使用，取消不会发起请求。默认保存到系统“图片”目录，
可选择其他 `.png` 路径。

不添加参考时使用 `POST /images/generations` JSON 请求，仅发送提示词和生成参数。
添加参考时改用 `POST /images/edits` 多图编辑，仅上传画风参考，提示模型以文本确定
主体、姿势和构图，以参考图提供线条、配色、材质和渲染风格；没有隐藏源图或空白占位图。
画风提示是模型指令，最终风格效果取决于供应商。对应端点不受支持时直接报错，
不会自动重试付费请求或切换接口。

复用当前供应商、模型、Quality、Complexity、Format、Compression、尺寸、
流式预览和生成张数（1–10）。每张独立串行生成，保持供应商实际返回尺寸，
本地统一保存 PNG；Format/Compression 仍控制网络返回格式。
复用已完成图像缓存，关闭“复用已完成帧”可重新绘制。
已启用 Waifu2x 时，输入超分仅处理画风参考，输出超分处理成品；
纯文本模式没有输入图，不运行输入超分。

成品进入主窗口结果列表，可打开、预览，或使用“局部重绘”继续修正。
不生成动画 GIF；原表情的抽帧、保留文字、人物裁剪和时序一致性设置不参与绘制。
命令行 `mode=generate` 的示例见 [PLUGINS.md](PLUGINS.md)。

### 多源素材批量转换

在“插件管理”中勾选“批量转换”并应用。启用后，“原始表情”的“导入…”支持一次多选，
“目录”可选择素材文件夹；也可把多个图片、GIF、文件夹一起拖入源素材预览区域。
每次导入替换当前队列，重复路径自动去重；文件夹仅扫描当前层级，并按文件名排序。
支持 PNG、JPEG、GIF、BMP、TIFF 和系统可解码的 WebP。损坏图片跳过并记录日志，
全部无效时保留原队列；单次最多 500 个源文件，不扫描子文件夹。

源预览下方显示 `< 1 / 总数 >`：左右按钮切换素材，序号框可输入位置，
按 Enter 或移开焦点跳转；越界或无效序号会恢复当前值。切换素材会更新源预览、
主预览及 GIF 时间轴，不改变队列顺序、公共参考或输出目录。

队列有多份素材时，输出栏为文件夹，默认是第一份素材所在目录下的
`MemeConvertStudio-output`。点击“批量转换”按顺序处理全部素材，共用当前人物、
裁剪、画风、提示词和供应商参数；“生成张数”控制每份素材的副本数量。
输出如 `001_原文件名-edited.png`、`002_原文件名-edited.gif`，
多副本再加 `_001`、`_002`。跨文件夹同名素材通过队列序号区分，
不把多份素材合成拼图或一段 GIF。

开始前检查整个队列的输出，拒绝覆盖任何源图或参考图；已有成品先询问再替换。
任务进度累计所有素材和副本的帧数，结果下拉框按输出文件名选择成品。
失败或取消停止后续素材，保留完成的成品与帧缓存；相同队列、参数和输出目录可续作。
完成的源素材结果会出现在结果列表，当前中断素材的已完成帧保存在缓存中。
任务中禁止替换队列或切换源素材。关闭插件后恢复单源流程，保留当前预览素材。
仅导入一份素材时沿用原有单文件输出方式。

### 单源多副本

“生成张数 / GIF 份数”支持 1–10，默认 1。每份独立串行生成，GIF 每份都是完整动画。
单份沿用所选文件名，多份自动保存为 `名称_001.png`、`名称_002.png` 等，
GIF 使用 `.gif`。请求估算包含所有份数；生成完成后在“结果”旁下拉选择各份成品。
每份使用独立缓存，增加份数可复用已有的编号结果；从单份改为多份会使用新的编号路径。
多次点击生成且保留“复用已完成帧”会恢复缓存；需要全新结果时关闭该选项。

“原始表情”“参考人物”均支持从资源管理器拖入单个文件和中文路径。
任务执行中不替换素材；文件无效时保留原选择，错误记录在日志中。

参数底部“插件管理”支持扫描、启停插件及执行插件命令。插件可修改提示词、
处理最终帧像素、追加或替换附加要求。附带默认关闭的示例插件和版本化 C ABI SDK。
安装、开发和缓存规则详见 [PLUGINS.md](PLUGINS.md)。

其他默认关闭的功能插件：

- **API 参数支持测试**：从插件管理执行“测试当前供应商参数…”。先测试不带可选参数的基线，再分别测试已配置参数和完整组合；另提供 Quality、Complexity、Size、Format、Compression、流式和图片字段候选值，可勾选运行并追加自定义 JSON 参数组合。图片编辑使用当前源图与人物图首帧（未导入时使用内置诊断图）；也可切换纯文本 `/images/generations`。每项一次、不自动重试，开始前确认请求数和潜在费用；导出的报告不包含 API Key 或图像。HTTP 接受仅能验证请求格式，不能证明画质、压缩效果或完整流行为；拒绝、流式被忽略和无法判断分别标记。需使用包含宿主探测服务的新版主程序。
- **局部重绘**：启用后从顶部“局部重绘…”进入，默认读取当前选中的成品，
  可框选矩形、输入修正要求、添加本轮新画风参考并另存；支持静态图和完整 GIF。
  PNG 的选区外像素由本地合成保留。GIF 仍会重新量化颜色。
- **Waifu2x 输入/输出超分**：选择外部 `waifu2x-ncnn-vulkan.exe` 与模型目录，
  独立开启输入或输出超分，设置倍率、降噪、分块、GPU、TTA 和超时。
  不内置或安装第三方程序。输出超分放大实际成品，且可复用已有 API 帧缓存。

局部重绘、超分的配置及命令行示例见 [PLUGINS.md](PLUGINS.md)。

## 供应商兼容

人物替换和带参考图的绘制使用 `POST /images/edits` 的 `multipart/form-data` 多图编辑协议，
纯提示词绘制使用 `POST /images/generations` 的 JSON 协议，均采用 `Authorization: Bearer <key>` 认证。

- Base URL 输入 `https://your-provider.example/v1`，程序会补上 `/images/edits`；也可以填写完整编辑接口地址。
- 图片按顺序上传：原始表情帧、人物参考、画风参考、前后已重绘帧，最后可附加首张生成帧作为一致性参考。提示词逐一标明参考图对应的帧号与前后关系。
- 默认重复使用 `image[]` 上传字段；部分供应商需要重复 `image`，可在设置中切换。
- 支持 `data[0].b64_json` 和 `data[0].url` 两类响应；图像 URL 支持 HTTPS 和 base64 data URL。
- 质量和尺寸参数可以设为“不发送”。扩展 JSON 对象允许增加供应商支持的参数，例如 `{"input_fidelity":"high"}`。参数值按 multipart 字段发送，复杂值编码为 JSON 文本。
- Complexity、Format、Compression 默认“不发送”，不自动发送 `response_format` 或 `input_fidelity`。供应商是否接受某个可选参数取决于模型和接口。
- 不支持只提供 Chat Completions 图片输出、异步轮询任务或私有认证协议的服务。供应商必须支持多图编辑，单图生成接口不足以完成此任务。
- 供应商必须使用 HTTPS，本机 `localhost` / `127.0.0.1` 调试可使用 HTTP。程序不跟随接口重定向；下载结果图时不携带 API Key。

官方协议参考：

- https://developers.openai.com/api/docs/guides/image-generation
- https://platform.openai.com/docs/api-reference/images/createEdit

默认模型名为 `gpt-image-1`；请以你的供应商实际支持的名称为准。

## 流式预览与返回参数

- “流式接收局部图像”在界面中默认开启，发送 `stream=true` 和 `partial_images`（可选 1–3，默认 2）。接收 SSE 的 `image_edit.partial_image` 和 `image_edit.completed` 事件；兼容对应的 `image_generation` 事件名。局部图像会即时显示在“结果”预览中，数量由供应商决定。
- 兼容跨网络分块的事件、CRLF/LF、注释和多行 data。供应商忽略流式选项并返回普通 JSON 时，仍可读取最终图像；供应商拒绝流式参数时，请关闭该选项后重试。程序不会自动重试付费请求。
- 局部图像只作预览，收到最终完成事件后才保存帧缓存和成品。中断或服务端报错不会把局部图像当作完成帧；取消可中止流式网络等待。
- Quality 与 Complexity 分别设置。Quality 提供 `auto/low/medium/high/xhigh/max`，Complexity 提供“不发送/low/medium/high”；不要将两者混为同一参数。模型支持范围由供应商决定。
- Format 设置供应商返回的 `png/jpeg/webp` 格式。“不发送”使用供应商默认值；WebP 解码依赖系统可用的 WIC WebP 编解码器。
- Compression 是返回 JPEG/WebP 的压缩质量，范围 0–100，100 保留最多细节、压缩最少。仅在 Format 为 `jpeg` 或 `webp` 时启用；选择 PNG 或“不发送”时不发送此字段。
- Format 和 Compression 控制网络返回图像；本地成品仍按任务导出 PNG 或 GIF。原生动画 GIF 和 SVG 返回尚未接入此逐帧流程，不能在扩展 JSON 中覆盖这些专用选项。
- Complexity、Format、Compression 会影响帧缓存标识；仅切换流式预览或局部预览数量不会使已完成缓存失效。

官方字段与事件参考：

- https://developers.openai.com/api/reference/resources/images/methods/edit
- https://developers.openai.com/api/reference/resources/images/edit-streaming-events


## GIF 行为与调用量

本程序按静态帧调用图像编辑接口，在本地解析 GIF，先正确合成偏移帧、透明区域及 disposal 处置，再逐帧编辑并重新编码，不会把 GIF 当作视频直接上传。

- “抽帧间隔”默认 1；2 表示每两帧生成一张，并合并相应显示时长。
- “最多生成帧数”默认 24；超过上限时增加均匀抽帧间隔，保留整个动画的时长，而非截断尾部。0 表示不限。
- 画面中显示最多请求次数。缓存命中时实际请求数更少。
- “多帧参考：动作、画风与背景一致”默认开启。先重绘首尾帧，再逐层重绘区间中间帧，使中间帧能够同时参考已经重绘的前后画面，而不是上传尚未重绘的原始邻帧。
- 每次选择时间上最近的前方最多 2 帧、后方最多 2 帧，加上首张生成帧作为锚点；首帧与邻帧参考重复时仅上传一次。首次生成没有已重绘参考，末帧初始化时只有前向参考。参考副本最长边为 1024 像素。
- 即使添加 8 张画风参考，单次上传也最多 15 张图片。每个选中的输出帧只生成一次，不增加生成调用次数，但多图输入可能增加费用和延迟。关闭多帧参考后按时间顺序逐帧生成，不附加已重绘帧。
- 提示词要求保持动作过渡、人物设计、画风、背景和光照统一，同时跟随当前原始帧的姿态、遮挡、镜头和场景变化。独立生成仍可能发生闪烁、人物细节漂移或文字变化，不能保证完全消除。
- 生成进度按已完成数量递增；日志中的实际帧号可能跳跃。最终 GIF 始终恢复原时间顺序、帧时长和循环设置。续作时可恢复同一参考策略下的已完成关键帧和中间帧。
- 导出尺寸恢复为原表情的宽高，若供应商返回的宽高比例不同，会缩放到原尺寸。可通过尺寸参数选择最接近素材比例的输出规格。
- GIF 导出采用 256 色调色板和二值透明度；PNG 保留完整 Alpha。GIF 无法保留半透明或完整真彩。
- 保留原循环设置、帧时长；无循环扩展的 GIF 与显式循环一次的 GIF 分别处理。0 延迟输入帧按 100ms 兼容处理。

每次 API 调用可能计费。程序串行执行，不自动重试失败或超时的生成请求。取消可以中止本地网络等待，但供应商可能已开始处理或计费。

### 官方原生 GIF 支持核实

2026-11-08 核对官方文档：支持 GIF 输出的 GPT Image 模型可以通过 `POST /v1/images/generations` 或 `POST /v1/images/edits` 配合 `output_format: "gif"` 返回完整 GIF，不存在单独要求使用的 `/gifs` 端点。官方文档明确列出 GPT Image 2.6 Sunburst / Flare 的动画 GIF 能力，API 参考要求账户具备所选模型的 GIF 预览权限。

- `frame_count`：2–16，默认 9。
- `frame_duration_ms`：指南列出 20–2000 毫秒，默认 200，按 10 毫秒取整。
- 结果为 `data[].b64_json`，解码后直接得到完整 `.gif` 文件。
- 编辑接口可以用静态参考图引导动画；这不能证明第三方供应商支持上传原始 GIF 并逐帧保留其动作和时序。

以上是官方接口能力，第三方供应商是否开放取决于其实现与模型权限。本程序当前仍采用上述多帧参考的本地合成流程，尚未接入原生 GIF 响应，不能仅靠扩展参数切换为原生 GIF 模式。

核实来源：

- https://developers.openai.com/api/docs/guides/image-generation#generate-animated-gif-output
- https://developers.openai.com/api/reference/resources/images/methods/edit

## 断点续作

输出文件旁的 `<输出文件>.memestudio` 目录保存任务描述和已完成的 PNG 帧。参数、素材内容、裁剪、模型与供应商地址等共同决定缓存标识；改变这些内容会创建另一组缓存。

失败或取消后，保持相同素材、参数和输出路径，再次开始生成即可续作。只有“已生成并成功写入缓存”的帧能够复用；请求已经完成但未收到响应的帧无法恢复。可关闭“复用已完成帧”强制重新生成。

任务结束后可以自行删除不需要的 `.memestudio` 缓存。该目录包含源文件内容的哈希、生成帧和提示要求，不包含 API Key；生成帧仍属于你的图片数据。

程序不会用输出覆盖输入素材。成功写入临时文件后才替换最终输出。

## 密钥与设置

默认 API Key 只保留在进程内存中。勾选“加密记住密钥”后，使用 Windows DPAPI 加密，只能由相同 Windows 用户解密。其他设置保存在 `%LOCALAPPDATA%\MemeStudio\settings.json`。

启动时也会读取 `OPENAI_API_KEY` 环境变量，界面中已有的加密密钥优先。命令行任务只从环境变量读取密钥，配置文件不需要保存密钥。

## 构建与测试

### GitHub Actions 自动编译

`main` 分支推送、面向 `main` 的 PR 会自动运行 `Build Windows`；
也可在仓库 Actions 页面选择该工作流并点击 `Run workflow`。
使用 Windows x64 环境，直接执行 `build.ps1` 和 `package.ps1`，只编译打包，
不运行完整测试、不调用图像供应商，也不需要配置 API Key。

成功后在运行详情页的 Artifacts 下载 `MemeConvertStudio-Windows-x64`，
其中包含完整应用包 `MemeStudio-Windows-x64.zip` 和插件包 `MemeStudio-Plugins.zip`。
构建产物保存 14 天。同一分支新提交会取消旧构建。

六个 `plugin-*` 分支各自包含 `Build Plugin` 工作流，推送、PR 或手动操作时
只构建该分支对应插件。下载其 `MemeStudio-Plugin-Windows-x64`，
将解压后的 `plugins` 目录放到兼容的主程序旁。工作流不创建 GitHub Release。

### 本地构建

需要 Visual Studio 的“使用 C++ 的桌面开发”工作负载及 Windows SDK。

```powershell
.\build.ps1 -Test
```

`.\build.ps1` 构建程序和插件；添加 `-Test` 会运行 C++ 本地测试。模拟供应商集成测试需要 Node.js，仅用于测试，正式程序不依赖 Node.js：

```powershell
node .\tests\integration.mjs
```

集成测试覆盖多图上传、静态 PNG、GIF 时序、返回 URL、缓存续作、参数失效、取消及错误信息密钥隐藏。测试文件输出到 `tmp`。

只构建可运行 `.\build.ps1`，不会启动测试。程序正在使用、无法覆盖 EXE 时，可运行 `.\build.ps1 -ExecutableName MemeStudio-drawing.exe`，再用 `.\package.ps1 -Executable .\bin\MemeStudio-drawing.exe` 打包；压缩包内仍命名为 `bin\MemeStudio.exe`。本次新版单独构建为 `bin\MemeStudio-drawing.exe`。

无图绘制定向检查：`node tests/drawing.mjs`，仅用本机模拟接口检查纯文本 JSON、
画风参考上传、流式预览、返回尺寸、多张输出、缓存恢复、输入校验和原编辑路径。
`node tests/batch.mjs --exe=D:\Projects\MemeStudio\bin\MemeStudio-drawing.exe --ui-only`
检查插件入口和绘制窗口，并生成 `tmp/tests/ui-drawing.png`。

多源转换定向检查：`node tests/source-batch.mjs`，仅用本机模拟供应商验证混合图片/GIF 队列、
导入去重、编号输出、副本、累计进度、失败恢复与跨源图覆盖拦截。
`node tests/batch.mjs --exe=D:\Projects\MemeStudio\bin\MemeStudio-source-batch.exe --ui-only`
验证多文件/文件夹拖入、预览切换、输入跳转、忙碌保护及插件启停，并生成 `tmp/tests/ui-source-batch.png`。

流式与参数定向检查为 `node tests/streaming.mjs`，可通过 `--exe=D:\Projects\MemeStudio\bin\MemeStudio-next.exe` 指定构建产物；不调用真实供应商。

批量与插件定向检查：`node tests/batch.mjs`。加 `--ui-only` 仅检查拖入文件、
张数控件与插件管理命令，并生成界面截图。可通过 `--exe=绝对路径` 指定程序；
使用现有 `tmp/tests` 示例素材，不调用真实供应商。

局部重绘与外部工具接口定向检查：
先运行 `.\tests\build-waifu-mock.ps1`，再运行 `node tests/refine-upscale.mjs`。
仅构建并调用模拟工具，不下载 Waifu2x。运行
`node tests/batch.mjs --exe=D:\Projects\MemeStudio\bin\MemeStudio-plugins.exe --ui-only`
可检查两个插件窗口，并在 `tmp/tests` 生成 `ui-refine.png` 等截图。

已安装真实工具时，可运行
`node tests/waifu-real.mjs --tool=D:\Projects\waifu2x-ncnn-vulkan-20250915-windows`。
该检查会实际调用本机 Waifu2x，但图像生成仍使用本机模拟服务。

也提供 CMake 工程：

```powershell
cmake -S . -B build-cmake -A x64
cmake --build build-cmake --config Release
ctest --test-dir build-cmake -C Release --output-on-failure
```

`--demo <目录>` 可以载入 `--self-test` 生成的示例素材来检查界面，不会配置密钥或调用任何服务。

构建测试也会运行 `--ui-check <目录>`，在不显示窗口的情况下渲染标准与紧凑布局，核对默认控件值并测试人物框选处理。截图和 `ui-results.json` 写入同一测试目录。

## 命令行任务

```powershell
$env:OPENAI_API_KEY = "your-key"
Start-Process .\bin\MemeStudio.exe -ArgumentList @("--job", "D:\path\job.json") -Wait
```

`job.json` 示例：

```json
{
  "source": "D:/images/meme.gif",
  "character": "D:/images/character.png",
  "styles": ["D:/images/style.png"],
  "output": "D:/images/meme-edited.gif",
  "base_url": "https://your-provider.example/v1",
  "model": "gpt-image-1",
  "count": 2,
  "api_key_env": "OPENAI_API_KEY",
  "quality": "auto",
  "complexity": "high",
  "stream": true,
  "partial_images": 2,
  "output_format": "jpeg",
  "output_compression": 85,
  "size": "auto",
  "image_field": "image[]",
  "extra": {},
  "timeout_seconds": 600,
  "stride": 1,
  "max_frames": 24,
  "consistency": true,
  "preserve_text": true,
  "resume": true,
  "instruction": "保留原图表情与文字",
  "crop": {"x": 0, "y": 0, "width": 1, "height": 1},
  "progress_file": "D:/images/progress.json"
}
```

命令行默认不启用流式；Complexity、Format 默认省略，Compression 默认 `-1`（不发送）。可选 `preview_file` 指定实时预览 PNG 路径，`progress_file` 包含 `frame` 与 `partial_image_index`，其中 `-1` 表示非局部图像事件。

坐标均为相对于参考图尺寸的 0..1 比例。命令行任务中已有的输出文件会在完成后替换，失败时进程退出码为 1，并在配置文件旁写入 `MemeStudio-error.txt`。

可用 `MemeStudio.exe --inspect <图片路径> <报告.json>` 导出尺寸、帧时长和循环信息。`loop: 65536` 表示没有循环扩展，`0` 表示无限循环，`1..65535` 是 GIF 扩展中的重复次数。


## 限制

为控制内存，单图不超过 3200 万像素，任一边不超过 16384 像素，GIF 不超过 600 帧且解码像素合计不超过 384 MB。上传前将每张参考与源帧限制到最长边 2048 像素。

本项目的自动测试使用本机模拟接口，不验证特定供应商的可用性、模型权限、收费规则或实际生成质量。实际效果取决于供应商及所用模型。

第三方依赖: nlohmann/json，MIT 许可见 `vendor/nlohmann/LICENSE.MIT`。
