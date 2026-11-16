# 第三方项目与鸣谢

MemeConvertStudio 使用少量第三方代码、外部运行时和协议参考。下表区分
“随源码分发”“由插件按需下载”与“由用户自行提供”，避免将外部项目误认为
本项目的组成部分。

## 随源码分发

| 项目 | 用途 | 许可 / 归属 | 本仓库位置 |
| --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | C++ JSON 解析与序列化 | MIT；Copyright (c) 2013–2025 Niels Lohmann | `vendor/nlohmann/`、`vendor/nlohmann/LICENSE.MIT` |

## 插件按需使用或下载

| 项目 | 用途 | 许可 / 归属 | 分发说明 |
| --- | --- | --- | --- |
| [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) | Qwen-Image GGUF 原生推理运行时 | MIT；Copyright (c) 2023 leejet | 不随仓库提交；Qwen 插件可使用已有运行时或按需下载 |
| [Qwen-Image](https://github.com/QwenLM/Qwen-Image) | Qwen-Image Diffusers 管线和部署参考 | 以其仓库及模型卡中的许可证为准 | 代码、权重和 Python 环境不随主程序打包 |
| [Pillow](https://github.com/python-pillow/Pillow) | Qwen 插件图片编码与解码 | 以其发行包许可证为准 | 安装到用户选择的 Python 环境 |
| PyTorch / Diffusers / Transformers / Accelerate | Qwen Diffusers 推理环境 | 以各自发行包及上游许可证为准 | 不随源码分发，由用户选择环境并按需安装 |

Qwen 模型权重、GGUF 文件、VAE、文本编码器和 mmproj 属于各自模型发布者；
使用前必须阅读模型卡、权重许可证和适用的使用限制。本项目不会替用户授予
模型权利，也不会将用户本地模型复制进发布包。

## 用户自行提供的外部程序

| 项目 | 用途 | 许可 / 归属 | 本项目行为 |
| --- | --- | --- | --- |
| [waifu2x-ncnn-vulkan](https://github.com/nihui/waifu2x-ncnn-vulkan) | 输入 / 输出超分 | MIT；Copyright (c) 2019 nihui | 仅调用用户指定的 EXE 和模型目录，不内置、不下载、不修改 |

使用外部程序时，应同时遵守其许可证、模型许可证以及显卡驱动和运行时的
分发要求。若将这些程序与本项目一起再分发，请保留上游版权和许可证文本。

## 协议与文档参考

- [OpenAI Image Generation Guide](https://developers.openai.com/api/docs/guides/image-generation)
- [OpenAI Images API Reference](https://developers.openai.com/api/reference/resources/images/methods/edit)
- [Qwen-Image 2.1 stable-diffusion.cpp deployment guide](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/models/qwen_image_2.1.md)

上述页面仅用于协议、参数和部署行为参考；MemeConvertStudio 不包含 OpenAI
SDK，也不代表与 OpenAI、Qwen、nihui、leejet 或任何图像供应商存在官方合作关系。

## 许可证核对说明

本文件记录的是当前仓库的集成关系，不替代第三方项目的完整许可证文本。
许可证可能随上游版本变化；发布包含外部运行时或模型的安装包前，应重新核对
对应版本的许可证、版权声明和再分发条件。
