# DeepSeek Harness 调用兼容

将正在运行的 MemeConvertStudio 接入同一台 Windows 电脑上的 DeepSeek Harness。
此目录同时是 MemeConvertStudio DLL 插件和 Harness Cordis 插件包，不需要再部署 HTTP 服务。

## 安装

1. 启动新版 MemeConvertStudio，在设置底部的「插件管理」启用「DeepSeek Harness 调用兼容」并应用。确认日志显示本机桥接已启动。
2. 在 DeepSeek Harness 的插件管理器中从本地目录安装 `plugins/deepseek-harness`。选包含 `package.json` 和 `cordis.patch.yml` 的整个目录，而不是 DLL 或外层 `plugins` 目录。也可让 Harness 使用 `plugin_manager` 的 `install_bundle` 安装该绝对路径。
3. 保持 MemeConvertStudio 运行。在顶部插件按钮区打开「Harness 权限…」，或在插件管理中运行「Harness 权限设置」。在 Harness 中先调用 `memestudio_status` 检查连通性和权限。

## 工具

| 工具 | 用途 |
| --- | --- |
| `memestudio_status` | 查看已启用插件、当前模型、权限、当日额度及可见的供应商池渠道编号。 |
| `memestudio_inspect` | 查看本地图片或 GIF 的尺寸、帧数、时长和循环设置。 |
| `memestudio_models` | 查询当前供应商或指定渠道的模型；聚合网关开启且未指定渠道时返回已勾选模型。 |
| `memestudio_replace` | 使用人物参考图替换单个表情，可附画风参考图。 |
| `memestudio_batch` | 批量处理文件或文件夹；需启用批量转换。 |
| `memestudio_draw` | 文生图，可附画风参考；需启用无图绘制。 |
| `memestudio_refine` | 修正图片或 GIF，可限定选区；需启用局部重绘。 |
| `memestudio_probe` | 查看测试组合和 Token 估算，或在确认后实际发送；需启用 API 参数支持测试。实际探测最多八次，供应商池模式需要指定渠道。 |

## 权限与限额

首次启用时，生成、其他插件调用、插件设置覆盖和模型切换均不授权。状态、本机图片信息和直连远程供应商的模型列表仍可查询。选择权限后点击「保存」；主程序会取消正在进行的 Harness 任务（已经提交的请求仍可能计费）。权限只约束 Harness，不影响在界面中手动操作。

- 批量转换、无图绘制、局部重绘、参数测试、Waifu2x、聚合网关、本机回环端点（含 Qwen-Image），以及已安装插件的提示词和帧处理回调均有独立开关。使用对应功能前仍需在主程序启用并配置插件。若当前供应商池或候选渠道需要未授权的权限，调用会被拒绝，不会自动切换到直连供应商。
- 开启「切换模型」后，可通过 `model` 使用白名单中的精确模型 ID，只影响本次任务。聚合网关仍要求至少有一个已启用渠道支持该模型。
- 同时开启「覆盖插件设置」和目标插件权限后，生成工具可传入 `plugin_settings.waifu2x` 的 `input`、`output`、`scale`、`noise`，或 `plugin_settings.gateway.failover`。只修改本次任务的选项，不写回配置，不能读写 API Key、地址、服务配置、程序路径或权限。Harness 也不能安装模型或启动 Qwen 服务。
- 单任务默认 8 次、每日默认 32 次，`0` 禁止发送。额度按实际尝试发送的图像请求计数，包括 GIF 帧、批量、多份输出、实际参数探测及每一次灾备尝试。任务开始前会核对基本请求量，灾备次数超出余量时会停止。已扣除的请求不因失败或取消退还；读取缓存或查询模型不扣额度。每日额度按 **UTC 日期**重置，重启保留当日用量。这不是 Token 或金额上限。

生成和实际参数测试还要求 `confirmed: true`。该字段表示对本次请求的确认，不能代替权限面板的本机授权。

文件参数使用本机绝对路径。每次生成在 `%LOCALAPPDATA%\MemeStudio\dsh-output` 或指定的 `output_parent` 下创建独立子目录，返回成品路径，不覆盖原图。任务不会替换主窗口当前素材或预览。取消 Harness 调用、停用插件或关闭主程序会停止本地等待，已提交到供应商的请求仍可能完成或计费。

命名管道仅接受当前 Windows 用户的本机请求，不提供 HTTP 或局域网入口；同一 Windows 账号下的其他进程也可以调用，故只在信任本机环境时启用。Harness 不接收 API Key，也不能指定供应商 URL、超分程序或 DLL。
