# 命令行

`MemeStudio-cli.exe` 与 GUI 同目录，使用同一份 `plugins/`。CLI 不读取或保存 GUI 设置，不改写输入任务文件；插件只在本次进程加载。生成仍会写入指定成品、断点缓存和显式指定的报告或预览。

## 直接传参

```powershell
.\bin\MemeStudio-cli.exe replace --source meme.gif --character character.png --style style.png --output edited.gif --model image-model
.\bin\MemeStudio-cli.exe draw --prompt "画一张墨线表情" --output sketch.png --count 2
.\bin\MemeStudio-cli.exe refine --source sketch.png --prompt "修正轮廓" --output revised.png
.\bin\MemeStudio-cli.exe batch --input .\memes --input extra.gif --character character.png --output .\results
```

用 `--base-url` 指定 OpenAI 兼容供应商，用 `--api-key-env` 指定保存密钥的环境变量名（默认 `OPENAI_API_KEY`），不要把密钥放在命令行或配置文件中。`--style` 和 `--input` 可重复。输入 GIF 仍输出 GIF；静态成品保存为 PNG。`--format` 和 `--compression` 控制供应商响应格式和压缩参数，不改变本地成品扩展名。

`models` 查询模型，`inspect --source IMAGE` 查看图片或 GIF 信息。`--help` 列出常用参数。结果以 UTF-8 JSON 写到 stdout，进度和错误写到 stderr，`--quiet` 隐藏进度。失败退出码为 `1`，Ctrl+C 本地取消为 `130`；供应商已收到的请求仍可能完成或计费。

## 任务配置

```powershell
.\bin\MemeStudio-cli.exe run --config .\tasks\meme.jsonc --model another-image-model
```

支持 UTF-8 `.json`、`.jsonc`（允许注释，不允许尾逗号）、`.ini`。配置中的路径相对配置文件，命令参数中的路径相对当前工作目录。命令行选项覆盖同名配置项；重复的 `--style`/`--input` 替换配置数组，`--load` 则追加到插件列表。

```jsonc
{
  "command": "replace",
  "source": "input/meme.gif",
  "character": "input/character.png",
  "styles": ["input/style.png"],
  "output": "output/edited.gif",
  "base_url": "https://your-provider.example/v1",
  "model": "image-model",
  "api_key_env": "OPENAI_API_KEY",
  "count": 1
}
```

同一任务也可写成 INI。`[job]` 是可选的；可在 `[upscale]`、`[api_routing]` 等节中填写对象字段。注释单独占一行，以 `;` 或 `#` 开头；数组与嵌套值使用单行 JSON，布尔值为 `true` 或 `false`。

```ini
[job]
command=replace
source=input/meme.gif
character=input/character.png
styles=["input/style.png"]
output=output/edited.gif
base_url=https://your-provider.example/v1
model=image-model
api_key_env=OPENAI_API_KEY
count=1
```

可用 `command` 写入子命令。既有 JSON 任务可以继续用 `mode`（`replace`、`generate`、`refine`）；存在 `sources` 时默认走批量转换。高级字段参见 [插件说明](PLUGINS.md)。

## 插件

```powershell
.\bin\MemeStudio-cli.exe plugins --load memestudio.example
.\bin\MemeStudio-cli.exe plugin memestudio.example identity-note
.\bin\MemeStudio-cli.exe plugin memestudio.prompt-draw draw --prompt "画一张墨线表情" --output sketch.png
.\bin\MemeStudio-cli.exe probe --text-only
```

`plugins` 列出安装的插件、命令 ID 和本次加载状态。`plugin ID COMMAND` 会加载目标插件并调用已声明的命令。支持无界面运行的第三方回调可用 `--context` 传入 JSON 对象；回调没有窗口句柄。只加载可信的 DLL 和配置。

`draw`、`refine`、`batch` 和 `probe` 会自动加载对应插件。`upscale` 配置控制 Waifu2x 输入或输出超分，`api_routing` 配置供应商池、均衡和灾备；开启时自动加载对应插件。`probe` 默认只列测试组合和 Token 估算；加 `--probe-run` 才发送，`--cases` 选择额外测试，供应商池模式需指定 `--channel`。

涉及桌面窗口或主程序状态的功能仍从 GUI 使用，包括 DeepSeek Harness 权限、Qwen 服务配置和供应商效果对比窗口。已运行的 Qwen 端点可以像其他供应商一样通过 `--base-url` 调用。
