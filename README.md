# MemeConvertStudio API 负载均衡与灾备插件

本分支只包含 `memestudio.api-load-balancing` 插件入口、ABI 头、单插件构建脚本和
GitHub Actions。完整应用及路由服务位于同仓库的 `main` 分支。
需要包含 API 路由服务的新版宿主；旧宿主仅安装 DLL 无法获得此功能。

## 使用

1. 把构建产物安装到主程序旁的 `plugins/api-load-balancing/`。
2. 在插件管理勾选“API 负载均衡与灾备”并应用，顶部出现“API 均衡灾备…”。
3. 每个 API / Key 添加一个渠道，填写地址、Key、可选模型、权重、优先级和估算单价。
4. 修改已有渠道后点击“更新选中渠道”；勾选“生成任务启用路由”并保存。
5. 正常生成人物替换、GIF、局部重绘或无图绘制；批量任务共享同一路由状态。

单 API 多 Key 使用相同地址、不同 Key；多 API 使用不同地址。
“从主界面带入”可带入现有地址、模型和 Key。
停用插件或关闭路由后恢复主界面单供应商流程。
渠道池最多 64 项，同一接口和 Key 不允许重复，URL 使用 HTTPS，
不允许查询参数或内嵌凭据；本机模拟服务可使用 HTTP。
各渠道共享主界面的 Quality、Size、Complexity、Format、Compression、
流式和扩展参数，模型可单独覆盖；不会自动改写不兼容参数。

## 比例与灾备

- 默认采用平滑加权轮询，同优先级权重 3:1 表示长期请求份额约 75%:25%。
- “按估算花销比例”使用 `权重 / 单次估算费用` 分流请求。
  单价 1 和 3、相同权重时，请求约 3:1，成功请求估算花销约 1:1。
  单价须使用相同货币和计量单位；这不等于实际账单，也不是预算上限。
- 优先级越小越优先。同级不可用后使用下一备用层，不等待渠道冷却。
- 开启灾备时，HTTP 401/403 使渠道在本任务内停用，429 或明确的 DNS/连接建立失败
  使渠道进入可设置的冷却期。
- 可选对 502/503/504 切换，默认关闭，因为服务端可能已开始生成并计费。
- 超时、网络或流中断、成功响应解码失败、成品下载失败及其他 HTTP 状态停止任务。
  取消不触发灾备；同一次图像请求每个渠道最多尝试一次，不无限重试。

分配单位为未命中缓存的图像请求或 GIF 帧，现有生成仍串行执行，不额外增加并发。
每次新任务重置健康和份额统计；同一批量队列共享路由状态。
跨模型处理 GIF 可能出现画风变化，可先对比，或将不同模型仅设为备用。

## 效果对比与统计

选择加入对比的启用渠道，点击“效果对比…”。
可对比当前表情首帧、人物和画风参考，或相同提示词及可选画风参考。
相同 API / 模型只比较一次；每组一次请求，开始按钮显示总请求数。
对比不执行灾备、其他插件提示词修改或超分；取消不能撤回已发送请求。
各结果可在左右两栏选择并排查看，列表展示模型、状态、耗时和估算单价。
原尺寸 PNG 和 `report.json` 写入系统“图片”目录中新建的 `MemeStudio-compare-<ID>` 文件夹。
生成随机性可能影响画质和速度比较。

正常任务结束后重新打开配置，可以查看各渠道请求、成功、失败、累计耗时、
估算花销、冷却和停用状态。估算花销仅累计成功请求的配置单价，
不含失败计费、缓存、Token 用量和动态计费差异。

## 凭据

Key 默认只保留在内存中。“本机加密记住 Keys”使用当前 Windows 用户的 DPAPI 加密保存；
取消勾选并保存会移除已保存的 Key。与主界面的单 Key 记住选项独立。
Key 不传入插件 DLL，不写入帧缓存或报告；命令行按环境变量分别读取 Key。
CLI 的 `api_routing` 配置和 `routing_report` 示例见 `main` 分支的 `PLUGINS.md`。

## 构建与安装

需要 Windows x64、Visual Studio C++ 桌面开发工具和 Windows SDK：

```powershell
.\build.ps1
```

安装产物：

```text
dist/plugins/api-load-balancing/api-load-balancing.dll
dist/plugins/api-load-balancing/plugin.json
```

将该文件夹放到主程序 EXE 同级的 `plugins` 下，重新扫描、勾选并应用。
本 DLL 把 `configure-routing` 命令映射为 `{"action":"api_routing"}`，
实际窗口、请求、凭据及统计由宿主执行。

## 自动编译

推送本分支、提交 PR 或手动运行 `Build Plugin`，可下载
`MemeStudio-Plugin-Windows-x64` artifact，包含可安装的 `plugins` 目录，保存 14 天。
工作流只编译插件，不调用图像 API。

```text
plugin.cpp                    插件命令入口
plugin.json                   清单
include/memestudio_plugin.h    ABI v1
build.ps1                     单插件构建
.github/workflows/build.yml   自动编译
README.md                     使用说明
.gitignore                    构建产物忽略
```
