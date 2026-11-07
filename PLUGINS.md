# 插件开发

插件是本机 x64 DLL，只启用你信任的插件。安装位置为程序 EXE 同级的
`plugins/<插件目录>/`，包含 `plugin.json`、DLL 和插件自身需要的资源。
扫描只读取清单，勾选并“应用”后才加载 DLL。默认关闭所有插件。

在设置底部打开“插件管理”，可启停插件、重新扫描、打开目录和执行插件命令。
关闭窗口会放弃尚未应用的勾选；“执行命令”会先应用当前勾选。
生成或获取模型期间不能修改插件或执行命令。

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
- `shutdown`：卸载前释放插件资源；初始化失败时也会调用，须容忍部分初始化。

上下文包含 source、character、output 路径和 instruction；帧回调还包含
frame_index、frame_count、source_frame_index、variation_index（索引均从 0 开始）。
不会传入供应商密钥。插件自行管理配置和资源，不应依赖进程当前工作目录。

回调和宿主日志均为 `__cdecl`；返回字符串由插件持有，至少有效至下一次回调，
上限 256 KB，使用 UTF-8。禁止跨 DLL/CRT 分配释放内存，禁止异常越过 C ABI。
帧回调在任务线程串行执行；命令、初始化和卸载在界面线程执行；CLI 下均在主线程。
宿主日志可在回调期间使用，卸载后不可使用。后台线程必须在 shutdown 返回前退出。
不要在 DllMain 中调用宿主接口。

启用顺序决定提示词和图像处理顺序。缓存包含插件 ID、版本及 DLL SHA256；
命中缓存时不再次执行帧回调。修改 DLL 会使缓存失效；插件外部配置或资源变化时，
也须更新清单 version，或关闭“复用已完成帧”。
局部流式预览不经过最终帧处理；最终成品才应用该回调。

## 构建

`.\build.ps1` 同时构建程序和示例 DLL，将示例放到 `bin/plugins/example`。
CMake 构建也会将示例复制到程序旁。SDK 不依赖 OpenAI SDK 或第三方框架。

命令行任务可设置 `"plugins":["memestudio.example"]`；
可选 `"plugin_directory":"D:/my-plugins"` 指定其他插件目录。
不填写 plugins 时不加载插件。CLI 遇到不可用插件会直接报错。
