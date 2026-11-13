# MemeConvertStudio API 参数支持测试插件

这是 `memestudio.api-parameter-test` 的独立源码分支，只包含本插件、
ABI v1 头文件、构建脚本和 GitHub Actions。宿主程序位于 `main` 分支。

## 功能

- 基线请求、当前配置的单项参数与完整组合，逐项勾选后串行执行。
- 常见 Quality、Complexity、Size、Format、JPEG Compression、流式局部图像数量候选。
- 自定义 JSON 参数组合，最多 64 项，每项最多发送一次，不自动重试。
- 可选择图片编辑 `/images/edits` 或纯文本生成 `/images/generations`；
  编辑沿用当前源图、人物图首帧及人物裁剪，未导入时使用内置诊断图。
- 显示 HTTP 状态、判定、有限长度错误摘要与耗时；支持导出 JSON 报告。
- 检查有效图像响应、SSE 最终事件、局部图数量、普通响应的返回格式；
  图像 URL 不会下载。报告不写入密钥、请求图片或生成图。

HTTP 成功仅代表接受请求。画质、复杂度和压缩效果不能凭成功状态证明；
仅有错误明确指向被测试字段时标为“参数被拒绝”。流式被忽略、格式不符、
请求被拒绝与无法判断分别记录。结论仅适用于本轮模型、端点及实际测试的取值。

## 使用

1. 填写主界面供应商、API Key、模型和需要探测的当前参数。
2. 插件管理中重新扫描、勾选“API 参数支持测试”并应用。
3. 点击顶部“API 参数测试…”，或执行插件命令“测试当前供应商参数…”。
4. 选择端点，勾选用例，按需添加自定义组合，确认请求数量后开始。
5. 查看列表中的结果，选择条目查看详情，或导出 JSON。

每个调用都可能产生图像生成费用。基线、鉴权、限流、网络或服务端异常将
停止后续请求；取消会中止网络等待，不能撤回已被供应商接收的请求。
不会自动应用测试结果到主界面，也不会重试已发出的请求。

附加组合示例：

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

自定义组合以最小基线为起点，未填写的可选参数不发送。
可添加供应商扩展字段，保留字段和参数依赖按宿主规则校验。
只有编辑模式使用图片上传字段。

## 构建与安装

需要 Windows x64、Visual Studio C++ 桌面开发工具和 Windows SDK：

```powershell
.\build.ps1
```

可安装产物位于：

```text
dist/plugins/api-parameter-test/api-parameter-test.dll
dist/plugins/api-parameter-test/plugin.json
```

把 `dist/plugins/api-parameter-test` 放到主程序 EXE 旁的 `plugins` 目录。
本 DLL 只将命令 `test-parameters` 转换为 `{"action":"api_parameter_test"}`；
窗口、请求、认证与报告均由宿主执行，DLL 不接收密钥。
需要 `MemeStudio-api-test.exe` 或包含参数测试服务的更新宿主，
旧宿主仅安装 DLL 不会获得该功能。

## 自动编译

推送本分支、向本分支提交 PR，或手动运行 `Build Plugin` 后，
下载 `MemeStudio-Plugin-Windows-x64` artifact，解压并安装其中的 `plugins` 目录。
工作流只编译本插件，不测试或调用图像 API，产物保存 14 天。

## 文件

```text
plugin.cpp                    命令入口
plugin.json                   插件清单
include/memestudio_plugin.h    ABI v1
build.ps1                     单插件构建
.github/workflows/build.yml   自动编译
README.md                     使用说明
.gitignore                    构建产物忽略
```
