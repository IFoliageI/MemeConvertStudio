# MemeConvertStudio 批量转换插件

这是 `memestudio.batch-convert` 插件的独立源码分支，仅包含本插件、所需 C ABI 头文件和构建脚本。
主程序源码与完整工作流位于仓库的 `main` 分支。

## 功能

- 一次选择多份图片或 GIF，也可拖入多个文件、文件夹。
- 文件夹导入当前层级，按文件名排序，不递归子文件夹。重复路径去重，损坏图片跳过并写日志。
- 源预览下显示 `< 当前序号 / 总数 >`，可点击箭头，或输入序号后按 Enter、移开焦点跳转。
- 多份源素材共用人物、画风、提示词和供应商设置，按顺序逐项转换；GIF 保持各自的动画。
- 成品按队列序号和原文件名保存，“生成张数”分别作用于每份源素材。

本插件通过命令请求新版宿主执行导入和批量工作流。预览界面、队列调度、
API 请求、缓存与输出都由宿主提供；仅向旧版主程序安装 DLL 不会增加这些能力。
使用包含批量转换服务的 `MemeStudio-source-batch.exe` 或更新构建。

## 文件

```text
plugin.cpp                     插件入口与命令回调
plugin.json                    插件清单
include/memestudio_plugin.h     ABI v1 声明
build.ps1                      单插件 Windows x64 构建脚本
README.md                      本说明
.gitignore                     忽略本地构建产物
```

## 构建

需要 Windows x64、Visual Studio C++ 桌面开发工具和 Windows SDK。
在本分支根目录运行 PowerShell：

```powershell
.\build.ps1
```

脚本自动查找本机 Visual Studio x64 编译器，使用 C++20 和静态运行库构建，
不下载依赖，不构建主程序。中间文件写入 `build/`，安装文件输出为：

```text
dist/plugins/batch-convert/batch-convert.dll
dist/plugins/batch-convert/plugin.json
```

将 `dist/plugins/batch-convert` 目录放到主程序旁的 `plugins` 目录。
打开“插件管理”，重新扫描，勾选“批量转换”并应用；插件默认关闭。

## 自动编译

推送到本插件分支或向本分支提交 PR，会运行 GitHub Actions 的 `Build Plugin`。
也可在 Actions 中选择该工作流，选择本分支并点击 `Run workflow`。
工作流只编译本插件，不构建主程序、不调用图像 API。
成功后下载运行详情页的 `MemeStudio-Plugin-Windows-x64` 构建产物，
解压后将 `plugins` 目录放到兼容的主程序旁；产物保存 14 天。

## 使用步骤

1. 在“原始表情”点击“导入…”多选文件，或“目录”选择文件夹，也可拖入源素材预览区。
2. 用预览下的箭头或序号框查看素材。一次最多导入 500 个文件，每次导入替换整个队列。
3. 设置人物、画风和附加要求。多源时输出栏为文件夹，默认位于首个素材目录的 `MemeConvertStudio-output`。
4. 点击“批量转换”。输出例如 `001_素材-edited.png`；多个副本加 `_001`、`_002`。

单份素材沿用单文件输出。任务执行时禁止切换或替换源素材；关闭插件后恢复单源流程。
已有成品先询问再覆盖，输出不能覆盖任何输入图或参考图。
失败或取消会停止后续素材，保留已有成品与帧缓存；保持队列顺序、参数和输出目录可续作。

## 命令与兼容性

插件 ID：`memestudio.batch-convert`，ABI：1。

| 命令 | 返回动作 | 宿主行为 |
| --- | --- | --- |
| `import-files` | `batch_import` | 多选源文件 |
| `import-folder` | `batch_folder` | 选择素材文件夹 |

插件不修改提示词或帧像素，不需要持有供应商密钥。返回字符串具有静态存储期，
内存所有权与回调约定见 ABI 头文件。本分支不包含其他插件、主程序或预编译 DLL。
