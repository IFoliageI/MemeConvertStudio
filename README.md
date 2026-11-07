# 表情重绘 · Meme Studio

Windows 原生 C++20 桌面程序，通过可自定义供应商的 OpenAI 兼容图像编辑接口，把原表情中的主体替换成参考人物，并允许加入多张画风参考图。

## 启动

双击 `bin\MemeStudio.exe`。支持 Windows 10/11 x64；使用静态 C++ 运行库，不需要 Qt、Python、FFmpeg 或额外的运行时安装。

`MemeStudio-Windows-x64.zip` 包含可执行文件、全部 C++ 源码、测试和说明。重新打包可运行 `.\package.ps1`。

1. 导入原始表情，支持 PNG、JPEG、GIF、BMP、TIFF；其他格式取决于本机 WIC 解码器。
2. 导入人物截图或立绘。在人物预览上拖出矩形，可以只使用截图中的人物区域；“重置裁剪”恢复完整图片。
3. 按需添加最多 8 张画风参考图，并填写附加要求。
4. 填写供应商 Base URL、API Key 和模型名称。模型框可直接输入供应商的自定义模型名。
5. 选择输出路径，点击“开始生成”。静态图导出 PNG，动画导出 GIF。

模型框旁的“获取模型”会使用当前 Base URL 和 API Key 调用供应商的 `GET /models`，自动去重并填入下拉框，优先显示名称包含 `gpt-image` 的模型。支持输入完整编辑接口地址，保留原模型选择和手动输入；获取过程中可取消，失败时不清空已有选项。获取超时最多 30 秒。列表可能包含非图像模型，不代表所有模型都支持图片编辑。

开始任务时会在日志中显示实际模型名称，自动清理模型名首尾空白，并在发送前拦截空名称。上传分隔符使用标准 MIME 字符，兼容严格解析 multipart 的服务。

原图和生成结果可切换预览，支持播放、暂停和逐帧拖动。生成完成后可打开文件或所在目录。

## 供应商兼容

使用 `POST /images/edits` 的 `multipart/form-data` 多图编辑协议，采用 `Authorization: Bearer <key>` 认证。

- Base URL 输入 `https://your-provider.example/v1`，程序会补上 `/images/edits`；也可以填写完整编辑接口地址。
- 图片按顺序上传：原始表情帧、人物参考、画风参考、前后已重绘帧，最后可附加首张生成帧作为一致性参考。提示词逐一标明参考图对应的帧号与前后关系。
- 默认重复使用 `image[]` 上传字段；部分供应商需要重复 `image`，可在设置中切换。
- 支持 `data[0].b64_json` 和 `data[0].url` 两类响应；图像 URL 支持 HTTPS 和 base64 data URL。
- 质量和尺寸参数可以设为“不发送”。扩展 JSON 对象允许增加供应商支持的参数，例如 `{"input_fidelity":"high"}`。参数值按 multipart 字段发送，复杂值编码为 JSON 文本。
- 不自动发送 `response_format`、`output_format` 或 `input_fidelity`，避免要求供应商必须支持这些可选字段。
- 不支持只提供 Chat Completions 图片输出、异步轮询任务或私有认证协议的服务。供应商必须支持多图编辑，单图生成接口不足以完成此任务。
- 供应商必须使用 HTTPS，本机 `localhost` / `127.0.0.1` 调试可使用 HTTP。程序不跟随接口重定向；下载结果图时不携带 API Key。

官方协议参考：

- https://developers.openai.com/api/docs/guides/image-generation
- https://platform.openai.com/docs/api-reference/images/createEdit

默认模型名为 `gpt-image-1`；请以你的供应商实际支持的名称为准。

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

需要 Visual Studio 的“使用 C++ 的桌面开发”工作负载及 Windows SDK。

```powershell
.\build.ps1 -Test
node .\tests\integration.mjs
```

第一步构建程序并执行 C++ 本地测试；第二步需要 Node.js，仅用于运行模拟供应商集成测试，正式程序不依赖 Node.js。集成测试覆盖多图上传、静态 PNG、GIF 时序、返回 URL、缓存续作、参数失效、取消及错误信息密钥隐藏。测试文件输出到 `tmp`。

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
  "api_key_env": "OPENAI_API_KEY",
  "quality": "auto",
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

坐标均为相对于参考图尺寸的 0..1 比例。命令行任务中已有的输出文件会在完成后替换，失败时进程退出码为 1，并在配置文件旁写入 `MemeStudio-error.txt`。

可用 `MemeStudio.exe --inspect <图片路径> <报告.json>` 导出尺寸、帧时长和循环信息。`loop: 65536` 表示没有循环扩展，`0` 表示无限循环，`1..65535` 是 GIF 扩展中的重复次数。

## 限制

只实现 Windows 桌面版。为控制内存，单图不超过 3200 万像素，任一边不超过 16384 像素，GIF 不超过 600 帧且解码像素合计不超过 384 MB。上传前将每张参考与源帧限制到最长边 2048 像素。

本项目的自动测试使用本机模拟接口，不验证特定供应商的可用性、模型权限、收费规则或实际生成质量。实际效果取决于供应商及所用模型。

第三方依赖只有 nlohmann/json，MIT 许可见 `vendor/nlohmann/LICENSE.MIT`。
