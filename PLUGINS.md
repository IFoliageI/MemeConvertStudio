# 插件开发

插件是本机 x64 DLL，只启用你信任的插件。安装位置为程序 EXE 同级的
`plugins/<插件目录>/`，包含 `plugin.json`、DLL 和插件自身需要的资源。
扫描只读取清单，勾选并“应用”后才加载 DLL。默认关闭所有插件。

在设置底部打开“插件管理”，可启停插件、重新扫描、打开目录和执行插件命令。
关闭窗口会放弃尚未应用的勾选；“执行命令”会先应用当前勾选。
生成或获取模型期间不能修改插件或执行命令。
主界面的插件专属入口仅在对应 DLL 成功加载后显示。停用或应用失败会隐藏入口；
批量转换的目录与素材翻页控件同样按加载状态显示。任务期间入口保持显示但不可操作。

## 清单

```json
{
  "api_version": 1,
  "id": "memestudio.example",
  "name": "Example extension",
  "version": "1.0.0",
  "description": "Identity prompt extension",
  "library": "example.dll",
  "commands": [
    {"id": "identity-note", "title": "Add identity consistency instruction"}
  ]
}
```

ID 使用 ASCII 字母、数字、`-_.`，长度不超过 80；library 是当前插件目录内的
DLL 文件名。清单的 ID 必须与 DLL 导出的插件 ID 一致。加载失败会显示错误。
应用失败时停用当前插件组，修复后重新应用即可。

## C ABI

接口定义见 `sdk/memestudio_plugin.h`，示例实现见 `plugins/example/plugin.cpp`。
导出 `extern "C" __declspec(dllexport) const MemeStudioPluginV1*
__cdecl MemeStudioGetPluginV1(void)`。返回静态存储的结构，设置 `struct_size`
和 `abi_version`，不需要的回调可设为空。

- `initialize`：接收宿主日志回调，返回 0 成功；非零导致启用失败。
- `transform_prompt`：每个未缓存帧请求前修改完整提示词；返回空指针表示不修改。
- `process_frame`：供应商最终图像恢复为源尺寸后、写缓存前处理 BGRA8 像素；
  返回 0 成功。不能改变宽高、步长、像素指针或释放宿主存储。
- `run_command`：执行清单声明的命令，接收命令 ID、上下文 JSON 和父窗口 HWND。
  返回 UTF-8 JSON 对象，可设置 `instruction` 替换附加要求，
  或 `append_instruction` 在末尾追加要求；空指针表示无操作。
  宿主还提供 `{"action":"refine"}` 打开已有产物修正流程，
  `{"action":"configure_upscale"}` 打开外部 Waifu2x 设置。服务由宿主执行，
  插件无需持有供应商配置或重复实现网络传输。
  `{"action":"prompt_draw"}` 打开无图绘制窗口；需要启用 `memestudio.prompt-draw`。
  `{"action":"api_parameter_test"}` 打开参数测试窗口，仅允许 `memestudio.api-parameter-test` 调用。
- `shutdown`：卸载前释放插件资源；初始化失败时也会调用，须容忍部分初始化。

命令上下文包含 source、character、output、当前选中成品 result 路径和 instruction；帧回调包含
frame_index、frame_count、source_frame_index、variation_index（索引均从 0 开始）。
帧上下文还包含 mode（replace/refine/generate）；replace/refine 提供 region 对象
（x、y、width、height）。generate 的 source、character 为空，frame_index 为 0、
frame_count 为 1，variation_index 区分生成副本。
不会传入供应商密钥。插件自行管理配置和资源，不应依赖进程当前工作目录。

回调和宿主日志均为 `__cdecl`；返回字符串由插件持有，至少有效至下一次回调，
上限 256 KB，使用 UTF-8。禁止跨 DLL/CRT 分配释放内存，禁止异常越过 C ABI。
帧回调在任务线程串行执行；命令、初始化和卸载在界面线程执行；CLI 下均在主线程。
宿主日志可在回调期间使用，卸载后不可使用。后台线程必须在 shutdown 返回前退出。
不要在 DllMain 中调用宿主接口。

启用顺序决定提示词和图像处理顺序。缓存包含具有帧回调的插件 ID、版本及 DLL SHA256；
只声明菜单命令的插件不改变 API 帧缓存。
命中缓存时不再次执行帧回调。修改 DLL 会使缓存失效；插件外部配置或资源变化时，
也须更新清单 version，或关闭“复用已完成帧”。
局部流式预览不经过最终帧处理；最终成品才应用该回调。

## 构建

`.\build.ps1` 同时构建程序与 `example`、`local-redraw`、`waifu2x`、
`batch-convert`、`prompt-draw`、`api-parameter-test` 六个插件，分别放到 `bin/plugins/<插件目录>`。
CMake 也会复制六个插件到程序旁。
SDK 不依赖 OpenAI SDK 或第三方框架。

命令行任务可设置 `"plugins":["memestudio.example"]`；
可选 `"plugin_directory":"D:/my-plugins"` 指定其他插件目录。
不填写 plugins 时不加载插件。CLI 遇到不可用插件会直接报错。

## API 参数支持测试

插件 ID 为 `memestudio.api-parameter-test`。在插件管理中勾选并应用，
执行“测试当前供应商参数…”。该插件仅声明命令；供应商认证、请求与响应解析
由主程序完成，密钥不会传入 DLL。

窗口提供图片编辑 `/images/edits` 和纯文本生成 `/images/generations` 两种端点。
编辑默认使用当前表情与人物参考图的首帧及当前人物裁剪；图像未导入时使用内置诊断图。
纯文本生成不上传图像。参数候选覆盖当前已配置的 Quality、Size、Complexity、Format、
Compression、流式和局部预览数量、扩展 JSON 字段及多图字段，并提供常用取值。
可逐项勾选，也可输入 JSON 数组添加自定义组合：

```json
[
  {
    "name": "低质量 JPEG",
    "parameters": {
      "quality": "low",
      "output_format": "jpeg",
      "output_compression": 80
    }
  }
]
```

自定义参数遵守主程序同一参数校验规则。基线固定执行，确认请求数后才开始，
每个用例至多一次，上限 64 项，不自动重试。基线、鉴权、限流、服务器或网络失败时
停止后续请求；取消会中止当前网络等待，但不能撤回供应商已经接收的请求。

每个已接受的调用可能产生费用或保留供应商侧数据；探测使用简短诊断提示。
请求被拒绝、明确指向测试参数的错误、收到普通 JSON 而非 SSE、以及无法判断会分开记录。
HTTP 成功仅说明供应商接受该调用，不证明画质、压缩质量或模型遵循程度。
普通响应如仅返回 URL，不下载结果；报告不包含 API Key、请求图片或生成图，
仅包含模型、参数、响应状态、有限长度错误摘要和耗时。不要用真实敏感图片做探测。

报告可从窗口导出 JSON。独立 `plugin-api-parameter-test` 分支仅包含该插件及其单插件构建文件。

## 无图绘制

插件 ID 为 `memestudio.prompt-draw`，默认关闭。启用后点击主窗口顶部
“无图绘制…”或执行插件命令“通过提示词绘制…”。命令 `draw` 返回
`{"action":"prompt_draw"}`，宿主提供独立提示词、画风列表和另存窗口。

无需 source/character。画风图可选，最多 8 张，支持多选、拖入和预览；
窗口初始带入主界面附加要求及画风图，修改不写回这些公共输入。
无画风图时向 `/images/generations` 发送 JSON，有画风图时向 `/images/edits`
发送 multipart，仅上传画风图，按提示词定义新内容。完整编辑或生成端点会自动归一化，
保留 URL 查询参数；生成与模型列表使用同一 Base URL。
不兼容仅提供 Chat Completions 图片输出或异步任务轮询的供应商。

当前模型、参数、张数、流式及可选超分设置均复用。输出为 PNG，保留返回尺寸，
不缩放为源表情尺寸；GIF 画风参考只取首帧。原始表情、人物图、裁剪和动画参数不参与。
输入超分仅用于画风图；无画风图时跳过输入超分配置。
输出超分在本地缓存的生成图之后执行，尺寸上限由外部工具接口检查；
由于生成尺寸由供应商决定，输出超分超限可能在生成完成后才报错，但 API 缓存会保留。

每份成品独立编号和缓存。缓存包含模式、端点、提示词、实际生成参数、画风内容、
有帧回调的插件及输入超分指纹；只更改流式预览或输出超分可复用缓存。
失败或取消保留已保存的成品和最终图缓存，局部预览不作为缓存或成品。
可使用局部重绘继续修正输出。独立分支 `plugin-prompt-draw` 仅包含本插件代码、
ABI 头文件、构建脚本、README 和 GitHub Actions，不包含宿主实现。

CLI 示例（styles 可省略，密钥从环境变量读取）：

```json
{
  "mode": "generate",
  "instruction": "绘制一张开心挥手的 Q 版人物表情，白色背景，简洁构图",
  "styles": ["D:/references/style.png"],
  "output": "D:/images/drawing.png",
  "plugins": ["memestudio.prompt-draw"],
  "base_url": "https://your-provider.example/v1",
  "model": "your-image-model",
  "count": 2,
  "stream": true,
  "partial_images": 2
}
```

CLI 需显式启用插件；不与 sources 队列或局部重绘模式组合。
其他启用插件的提示词与最终图回调仍执行，context.mode 为 generate。
本插件只有菜单命令，不自行修改提示词或像素。

## 局部重绘

在插件管理中勾选“局部重绘”并应用，然后执行“局部重绘当前产物…”；
启用后，主窗口顶部也有“局部重绘…”快捷入口。

窗口默认使用当前选中的生成结果，亦可选择已保存的 PNG/GIF。
拖出矩形选区，填写本轮修改要求，可添加最多 8 张新的画风参考，然后另存。
“全图”恢复整张图片为可编辑区域。GIF 可拖动时间轴选择预览帧，
所有帧共用相同的归一化矩形；处理完整动画，不沿用主窗口的抽帧限制。
本轮参考图列表默认清空，不会自动带入上一步参考图。

修正使用当前供应商、模型、质量、流式、张数等设置。
提示词以已生成图像为基础，不再要求替换人物，也不强制上传原人物参考。
局部范围通过提示词描述；最终图和局部流式预览均在本地合成，只接受矩形内的新像素。
这是矩形选区保护，未使用供应商的 mask 参数，不要求其支持专有蒙版协议。
模型仍须理解修改要求，选区边缘也可能需要进一步修正。

PNG 保留选区外原像素和 Alpha。GIF 在合成后重新量化为 256 色，
选区外显示颜色仍可能因 GIF 调色板而改变。若同时开启输出超分，
会在选区合成后对整张输出放大，选区外也参与超分。

默认文件名追加 `-retouched`，已有同名文件则继续编号。
不会覆盖作为输入的产物；可逐次对新的结果继续修正。
取消窗口不会发起 API 请求。生成阶段可使用主窗口“取消”。

## 批量转换

插件 ID 为 `memestudio.batch-convert`，默认关闭。启用后，源素材支持多选文件、
拖入多个图片/GIF/文件夹，或从“目录”选择文件夹。文件夹导入当前层级，
按文件名排序，不扫描子目录；重复路径去重，损坏图片跳过并写入日志，最多 500 个源文件。
每次导入替换队列；无有效图片则保留旧队列。

预览下方 `< 当前序号 / 总数 >` 可用箭头切换，也可输入序号后按 Enter 或移开焦点。
生成时禁止更换素材。多源转换输出到文件夹，并用队列序号为每份成品命名，
“生成张数”分别应用到每个源文件。每份 GIF 保留独立的帧顺序、时长与循环。
关闭插件后以当前预览素材恢复单源流程。

命令 `import-files` 返回 `{"action":"batch_import"}`，
`import-folder` 返回 `{"action":"batch_folder"}`。导入、导航及队列调度由新版宿主实现；
DLL 不重复实现网络和图像处理，也不改变提示词或缓存标识。
独立 `plugin-batch-convert` 分支仅包含对应插件源码、清单、ABI 头文件、构建脚本和 README。

命令行也可调用相同队列流程。`sources` 替代 `source`，接受文件及文件夹路径，
`output` 必须是文件夹，显式启用批量插件：

```json
{
  "sources": ["D:/memes/folder", "D:/memes/another.gif"],
  "character": "D:/references/character.png",
  "output": "D:/converted",
  "plugins": ["memestudio.batch-convert"],
  "base_url": "https://your-provider.example/v1",
  "model": "your-image-model",
  "count": 1
}
```

队列共享当前人物、画风、参数与可选超分。失败或取消后停止后续任务，
重跑相同队列可复用缓存。整个队列会先检查输出路径，不允许覆盖任何输入素材。

## Waifu2x 输入/输出超分

插件 ID 为 `memestudio.waifu2x`，对接
[nihui/waifu2x-ncnn-vulkan](https://github.com/nihui/waifu2x-ncnn-vulkan) 的本机命令行。
本项目只提供接口，不内置、下载或安装其程序、模型、Vulkan 组件。

在插件管理中勾选并应用，执行“设置输入/输出超分…”：

- 选择你已有的 `waifu2x-ncnn-vulkan.exe`，模型目录默认使用它旁边的 `models-cunet`，
  也可选择其他目录（需含 `.param` 与 `.bin`）。
- 输入、输出两个开关独立，默认均关闭。停用插件后两个开关都不生效，
  再启用时保留先前设置。
- 倍率 1/2/4/8/16/32；1 可用于仅降噪。降噪 -1/0/1/2/3。
- 分块 0 自动，或 32–4096；GPU -2 自动（省略 `-g`），-1 CPU，0–64 指定设备。
  可启用 TTA，并设置每张图的处理超时（5–3600 秒）。

输入超分作用于源图帧、裁剪后的人物参考和画风参考；
局部重绘则处理已有产物帧与本轮新参考。GIF 逐帧处理，
已生成的时间参考不重复超分。为遵守现有最长边 2048 的上传限制，
大图先缩到 `2048 / 倍率` 内再运行超分；小图按实际倍率放大。
同一任务中相同输入只调用一次工具，API 缓存命中时跳过输入超分。

输出超分发生在所有帧生成与局部合成之后，放大实际成品尺寸；
GIF 保留帧数、时间及循环，仍有 256 色限制。
只更改输出开关不会使已有 API 帧缓存失效，但会重新运行本地超分。
仅开启输出超分时，更改倍率等参数也可复用 API 帧缓存；输入与输出共享倍率，
若输入超分同时开启，修改倍率会改变输入及其缓存标识。
开启/更改输入超分会改变 API 帧缓存，指纹包括参数、可执行文件及模型内容。
单帧不超过 3200 万像素，边长不超过 16384，动画合计不超过 384 MB；
超出时在付费生成前拒绝。

使用 `CreateProcessW` 直接调用指定 EXE，参数按 Windows 规则转义，
不经 shell。子进程隐藏运行；主窗口取消或单帧超时会终止本次子进程树，
不影响用户自行打开的工具。工具报错时保留原有成品及 API 帧缓存，
修复后可重试。临时 PNG 保存在系统临时目录，并在任务结束后清理。

实际参数形式为：

```text
waifu2x-ncnn-vulkan.exe -i <临时输入.png> -o <临时输出.png> -s 2 -n 0 -t 0 -m <模型目录> -f png
```

按需加 `-g` 与 `-x`。不同发行版本的参数支持可能不同，
若工具拒绝参数，请使用与你配置匹配的版本。

## 命令行修正与超分

命令行可直接设置以下宿主功能，无须打开插件界面：

```json
{
  "mode": "refine",
  "source": "D:/images/previous-result.png",
  "styles": ["D:/images/new-style.png"],
  "output": "D:/images/corrected.png",
  "instruction": "修正左眼，保持其他内容",
  "region": {"x": 0.2, "y": 0.1, "width": 0.4, "height": 0.5},
  "base_url": "https://your-provider.example/v1",
  "model": "your-model",
  "upscale": {
    "executable": "D:/waifu2x/waifu2x-ncnn-vulkan.exe",
    "models": "",
    "input": false,
    "output": true,
    "scale": 2,
    "noise": 0,
    "tile": 0,
    "gpu": -2,
    "tta": false,
    "timeout": 600
  }
}
```

`mode=refine` 时不需要 character，省略 region 表示全图，
默认 stride=1、max_frames=0；不得抽帧。普通人物替换任务也可使用 upscale。
CLI 显式指定的 upscale 不受 GUI 插件启停状态影响。
