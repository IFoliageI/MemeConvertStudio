# 本地模型支持 - Qwen-Image

MemeConvertStudio 可选插件。原生 C++ DLL 管理本地 Qwen-Image 服务，支持
**stable-diffusion.cpp 原生 GGUF 推理**和 **Diffusers/PyTorch 标准模型推理**，
提供带 Bearer Key 的 OpenAI 兼容图片接口。
插件默认关闭；加载成功后主窗口才显示“Qwen-Image…”。

## 开始使用

1. 将完整插件目录放到 EXE 旁的 `plugins/qwen-image/`，在“插件管理”勾选并应用。
2. 打开“Qwen-Image…”，选择完整本地模型目录，以及用于创建虚拟环境的 Python
   程序或安装目录。建议使用 64 位 Python 3.12，以提高 GPU wheel 兼容性。
3. 默认在**已安装插件目录**内创建 `.env`。可以修改新环境名称；也可填写已有的
   venv/Conda 环境根目录，此时新环境名称和基础 Python 不参与创建。
4. 如使用 GGUF，点击“GGUF…”选择引擎和组件路径，再点模型目录右侧“检查”。
   `auto` 对有 `model_index.json` 的目录使用 Diffusers，其他目录使用原生后端。
   点击“一键配置环境”，检查日志。安装会访问官方运行时/Python 包源，不下载模型。
5. 设置端口、Key、设备和驻留选项，点击“启动服务”。服务就绪后点
   “应用端点到主界面”。已开启聚合网关时会添加/更新供应商池中的
   `Local Qwen-Image` 渠道，再在均衡插件里调整其权重、优先级。

“保存设置”保存当前表单；修改配置后需要停止、重新启动服务才能生效。
设置和 GGUF 面板可与主界面、其他插件面板同时操作；重复打开会激活已有面板。
新版宿主支持“应用端点”异步更新主界面，应用后面板继续保持打开。
旧版宿主缺少异步动作回调时仍使用原有模态窗口。
关闭设置窗口保持服务运行。停用插件、重新应用插件组、退出主程序或点“停止服务”
都会终止进程树。安装/推理状态与错误在日志区显示，不启动独立终端。

## 原生 GGUF 推理

推理使用 [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) 的
`sd-server.exe`，不使用 PyTorch；Python 仅承担轻量 API、进程管理和图片编码，
虚拟环境只需 Pillow。可选择已有 `sd-server.exe` 或留空让安装器下载。
原生后端同时接受生成和多参考图编辑，保持图像上传顺序。

“GGUF…”窗口支持自定义扩散模型、VAE、文本编码器、mmproj 文件路径，
可指向不同目录；相对路径以模型目录为基准。目录顶层只有一个候选的组件可自动识别，
多个量化版本并存时须明确选择。检测 GGUF 元数据时不读取权重张量。
Qwen-Image 2.1 使用其专用 VAE；GGUF 文本编码器进行编辑时还需对应 mmproj。
默认启用 **Qwen-Image 2.1 Turbo 固定 8 步 / CFG 1**，其他模型请取消。
原生 `quality` 的步数映射在 Turbo 模式不生效；显式提交不兼容的步数/CFG 会报错。
2.1 原生输出尺寸每边必须是 32 的倍数。`size=auto` 使用 1024×1024。
“显存预算 GiB”可限制原生引擎的 GPU 权重及工作缓冲预算，0 使用引擎自动检测。
内存紧张时可先设 4..6 GiB；引擎可能分段并从 CPU/磁盘换入，降低速度以减少峰值占用。

用户提供的 `AtomicChat/Qwen-Image-2.1-Turbo-Uncensored-GGUF` 是
**文本编码器仓库**；`Qwen-Image-2.1-Turbo-Uncensored-BF16.gguf` 和 mmproj
不是完整图像生成模型。还需配套的扩散模型及 `qwen_image_2.1_vae_bf16.safetensors`。
可参考 [该模型卡](https://huggingface.co/AtomicChat/Qwen-Image-2.1-Turbo-Uncensored-GGUF)
和 [Qwen 2.1 部署说明](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/models/qwen_image_2.1.md)。
本插件不会自动下载这些权重，也不会把文本编码器当作扩散模型加载。

原生安装后端：

- `auto`：系统已有 Vulkan loader 时使用 Vulkan，否则 CPU。Vulkan 可利用
  驱动提供的 AMD / Intel / NVIDIA 推理能力，不安装 ROCm/CUDA 系统工具链。
- `vulkan` / `cpu` / `cuda`：下载对应官方 Windows x64 运行时，
  固定版本 `master-1106-532d2c1`，检查 GitHub 提供的 SHA256 后解压到
  插件 `runtime/`；运行 `--help` 和 `--list-devices` 验证二进制及设备。
- `existing` 或自定义程序路径：验证已有运行时，不下载替换。自定义构建须支持
  本插件使用的 `/sdcpp/v1/models/status`、异步图片任务及预览接口。
- ARM64 原生运行时需用户提供兼容构建；自动下载目前只有 Windows x64。

设备枚举成功不代表模型完整推理成功。插件首次请求启动私有的 loopback
`sd-server` 子进程并加载模型；私有端口随机分配，不向局域网监听，
其管理接口不带 Key，仅外层兼容端点执行 Key 鉴权。
原生引擎日志可能包含推理参数/提示词，分享日志前请检查内容。

## Diffusers 模型目录

遵循 [Qwen-Image 官方部署文档](https://github.com/QwenLM/Qwen-Image) 的
Diffusers 管线，目录必须包含 `model_index.json`、组件配置、分词器和完整权重。
只读本地 safetensors，不下载缺少的权重、不执行远程模型代码。

| model_index 中的类 | 接口 | 用途 |
| --- | --- | --- |
| `QwenImagePipeline` | `/v1/images/generations` | Qwen-Image / Qwen-Image-2512 文生图 |
| `QwenImageEditPipeline` | `/v1/images/edits` | 原版单图编辑 |
| `QwenImageEditPlusPipeline` | `/v1/images/edits` | Qwen-Image-Edit-2509 / 2511 多图编辑 |

人物替换会同时提交原表情和人物参考，加上画风参考后图像更多，因此请使用
**EditPlus 多图模型**。单图模型会明确拒绝额外图片，不会忽略参考图。
纯文生图请加载文生图模型，并通过“无图绘制”插件操作。文生图模型不能直接接受
画风参考；需要参考图片时使用编辑模型和图片编辑流程。
Diffusers 后端不接受 GGUF、单独 checkpoint、任意量化变体、LoRA 或未列出的管线。

## Diffusers 环境安装范围

- `auto`：优先保留已有可实际运算的 CUDA/HIP 或 Intel XPU PyTorch。新 NVIDIA
  环境依据 `nvidia-smi` 支持的 CUDA 上限选择官方 wheel 源；支持的 Intel
  Arc/Core Ultra 适配器选择官方 XPU wheel 源。未发现 GPU 时使用 CPU。
- `cpu` / `cuda` / `xpu`：显式安装相应官方 PyTorch 与 torchvision，
  随后补齐 Diffusers、Transformers、Accelerate、Pillow 等依赖并验证。
- `existing`：保留当前环境的 PyTorch，安装缺少的推理依赖。需要自定义 GPU
  构建的用户可先准备已有环境；可用的 HIP/ROCm PyTorch 以 `cuda` 设备调用。
- 检测到 GPU 但不能确定可用自动方案时会报错，不会显示 GPU 配置成功。
  AMD 新环境请先提供官方支持且可用的 HIP/ROCm PyTorch 环境，再选择
  `auto` / `existing`；也可改用上面的原生 Vulkan 方案。
  本插件不自动部署 AMD Windows ROCm SDK。
- 不安装或更改显卡驱动、系统 CUDA Toolkit、HIP/ROCm 工具链、系统 Python、
  防火墙规则。推理 wheel 自带的用户态运行库属于虚拟环境依赖。
- 安装会修改选定的虚拟环境。已有可用 PyTorch 由版本约束保护；不兼容的
  torchvision/依赖组合会报错，需查看日志更换兼容环境。
- ARM64 DLL 可以与 ARM64 主程序使用；Python/PyTorch 的可用设备及 wheel
  取决于所选 Python 的架构与上游支持，不能据 DLL 架构推断 GPU 推理可用。

每次安装后导入三种 Qwen 管线，并实际执行 PyTorch 分配与算术操作。
这不代表完整模型已经运行成功；最终内存需求、硬件算子和速度需用用户模型验证。
完整模型可能需要大量显存和内存。GPU / CPU 内存卸载可减少 GPU 常驻占用，
但不会省去 CPU 模型内存，也不是低比特量化。

## 端点与参数

默认 `http://127.0.0.1:8189/v1`，默认模型别名 `qwen-image-local`。
端口可选 1024..65535；Key 至少 8 个可打印 ASCII 字符。初次加载生成随机 Key。
启用“允许局域网访问”后监听 `0.0.0.0`，其他设备使用此电脑的局域网 IP。
本地 HTTP 不提供 TLS；不要在不可信网络中公开监听。

| 路径 | 行为 |
| --- | --- |
| `GET /v1/models` | 返回配置的模型别名 |
| `POST /v1/images/generations` | JSON 文生图 |
| `POST /v1/images/edits` | multipart，`image` 或 `image[]`，最多 16 张 |
| `GET /health` | 管线、设备、模型是否驻留 |
| `POST /admin/unload` | 手动释放模型，仅本机可用 |

所有路径要求 `Authorization: Bearer <Key>`。单次只运行一个推理请求，
并发请求返回 429。生成返回 `data[].b64_json`。支持：

- `model`、`prompt`、`n=1`、`response_format=b64_json`。
  主程序生成多张时按既有流程逐张调用。
- `size=auto` 或 `宽x高`；每边 128..2048、16 的倍数、最多 4 百万像素。
- `quality=low/medium/high/auto` 对应 20/35/50/设置的推理步数。
- `output_format=png/jpeg/webp`；JPEG/WebP 可用 `output_compression=0..100`，
  按 Pillow 的编码 quality 语义映射。PNG 不接受该参数。
- `seed`、`negative_prompt`、`num_inference_steps=1..100`、
  `true_cfg_scale=1..30`、`guidance_scale=0..30`。
- `stream=true` 使用 SSE，包含等待心跳和最终 `.completed` 图像。
  **原生后端**通过任务预览转发最多 `partial_images=1..3` 张局部图像；
  预览是采样近似，取决于模型/运行时是否提供，快速任务可能直接返回最终图。
  **Diffusers 后端**当前只返回最终图，没有局部预览。
  慢速推理需适当增加主程序超时。

不支持的参数（如 `complexity`、mask、透明背景等）返回明确错误。
在主界面将不支持的参数设为“不发送”。不会以接受未知参数的方式伪装功能支持。
无法加载本地模型、缺少依赖或内存不足的错误通过 JSON/SSE 返回。

## 驻留与退出清理

勾选“保留模型（手动卸载）”后，首次请求加载模型，后续请求复用。
“卸载模型”释放管线、执行垃圾回收并清空设备缓存，服务继续可用。
取消勾选则每次请求结束后释放；原生后端通过结束推理子进程释放模型，
下次请求重新启动。设备驱动仍可能保留少量进程上下文内存；
“停止服务”会终止 Python 进程树，作为完整释放方式。

DLL 创建 Windows Job Object，设置 `KILL_ON_JOB_CLOSE`，在恢复 Python
执行前将其加入 Job。宿主崩溃或 DLL 正常卸载时自动结束 Python 及其子进程；
服务另有父进程存活监视；原生后端另设子进程 Job，桥接进程崩溃时也终止推理进程。
没有持久后台服务或开机启动项。
不能保证机器断电、驱动故障后的硬件恢复。

Key 在插件设置中由 Windows DPAPI 加密；启动配置通过匿名 stdin 管道传递，
不放进命令行、ready 文件或日志。主程序应用端点后按其现有规则保存连接信息。
配置文件与日志存放在插件目录，该目录须可写。

## 构建

主仓库：`.\build.ps1 -Architecture x64` 或 `-Architecture arm64`，
自动复制 DLL、清单、Python 脚本、requirements 和本说明。
Python 与模型不随程序分发。

独立 `plugin-qwen-image` 分支只包含本插件、ABI 头文件、nlohmann/json、
单插件构建脚本与 GitHub Actions，支持 x64/ARM64。
运行该分支 `.\build.ps1 -Architecture x64`，产物位于
`dist/x64/plugins/qwen-image/`，将完整目录安装到对应架构主程序旁。

接口与环境选择的轻量模拟检查：
`python tests/qwen_test.py`。无需 torch、GPU 或模型；不覆盖真实推理画质与性能。

2026-11-16 本机验证：AMD Radeon 8060S / Vulkan、Qwen-Image 2.1 Turbo
Q4_K 扩散模型、BF16 编码器及配套 mmproj / VAE，6 GiB 显存预算下完成一次
512×512、3 张合成参考图编辑，收到 3 次局部预览和最终 PNG，总耗时约 98 秒。
已确认生成后驻留及手动卸载。该结果不代表其他硬件、模型或复杂素材的表现；
Diffusers 与 ARM64 推理未做实机验证。
