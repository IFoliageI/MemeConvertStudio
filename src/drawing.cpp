#include "studio.hpp"
#include "api_routing.hpp"
#include "plugins.hpp"
#include "upscale.hpp"
#include <algorithm>
#include <cwctype>

namespace studio {
void runDrawing(const Job& job, Cancel& cancel, const Reporter& report) {
    cancel.verify();
    if (job.refine) throw Error("无图绘制不能同时使用局部重绘模式");
    if (job.instruction.find_first_not_of(L" \t\r\n") == std::wstring::npos)
        throw Error("请填写绘制提示词");
    if (job.styles.size() > 8) throw Error("最多支持 8 张画风参考图");
    auto paths = outputPaths(job);
    // Validate the entire destination set before making any paid requests.
    for (const auto& path : paths) {
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        if (extension != L".png") throw Error("无图绘制的本地输出扩展名必须为 .png");
        for (const auto& style : job.styles) {
            if (!fs::is_regular_file(style)) throw Error("画风参考图片不存在");
            if (_wcsicmp(fs::weakly_canonical(path).c_str(), fs::weakly_canonical(style).c_str()) == 0 ||
                (fs::exists(path) && fs::equivalent(path, style)))
                throw Error("输出文件不能覆盖画风参考图");
        }
    }
    generationBody(job.provider, utf8(job.instruction));
    const auto endpoint = job.styles.empty() ? generationUrl(job.provider.baseUrl) : editUrl(job.provider.baseUrl);
    std::vector<Upload> refs;
    report({0, job.generationCount, L"正在准备无图绘制…"});
    for (const auto& style : job.styles)
        refs.push_back({"style", png(limited(decode(style).frames.front().image))});
    std::unique_ptr<Upscaler> upscaler;
    if ((!refs.empty() && job.upscale.input) || job.upscale.output)
        upscaler = std::make_unique<Upscaler>(job.upscale, cancel, report);
    Json manifest{{"version", 1}, {"mode", "generate"}, {"endpoint", utf8(endpoint)},
        {"parameters", generationBody(job.provider, utf8(job.instruction))}, {"styles", Json::array()}};
    // Transport-only preview settings do not change the generated-image cache.
    manifest["parameters"].erase("stream"); manifest["parameters"].erase("partial_images");
    for (const auto& ref : refs) manifest["styles"].push_back(digest(ref.data));
    if (!refs.empty()) {
        manifest["field"] = job.provider.imageField;
        if (job.upscale.input) manifest["upscale_input"] = upscaler->fingerprint();
    }
    if (job.plugins && !job.plugins->fingerprint(true).empty())
        manifest["plugins"] = job.plugins->fingerprint(true);
    if (job.provider.router || job.provider.gateway) manifest["api_routing"] = routingFingerprint(job.provider);
    for (int i = 0; i < job.generationCount; ++i) {
        cancel.verify();
        auto output = fs::absolute(paths[i]).lexically_normal();
        auto copyManifest = manifest; copyManifest["variation"] = i;
        auto serialized = copyManifest.dump();
        auto signature = digest(Bytes(serialized.begin(), serialized.end()));
        auto cache = output; cache += L".memestudio";
        cache /= wide(signature.substr(0, 24));
        fs::create_directories(cache); writeJson(cache / L"manifest.json", copyManifest);
        auto cached = cache / L"image.png";
        Json context{{"mode", "generate"}, {"source", ""}, {"character", ""},
            {"output", utf8(output.wstring())}, {"instruction", utf8(job.instruction)},
            {"frame_index", 0}, {"frame_count", 1}, {"source_frame_index", 0}, {"variation_index", i}};
        auto send = [&](int done, const std::wstring& message, std::shared_ptr<Image> preview = {}, int partial = -1) {
            Progress event{done, job.generationCount, L"第 " + std::to_wstring(i + 1) + L" / " +
                std::to_wstring(job.generationCount) + L" 张：" + message, preview, 0, partial, i, job.generationCount};
            event.frameTotal = 1; report(event);
        };
        Image image;
        if (job.resume && fs::exists(cached)) {
            try { image = decode(cached).frames.front().image; } catch (...) {}
        }
        if (image.empty()) {
            auto uploads = refs;
            if (job.upscale.input) for (auto& ref : uploads)
                ref.data = png(upscaler->run(limited(decode(ref.data), std::max(1, 2048 / job.upscale.scale)), true));
            std::string text = "Create one new complete image from the user's prompt. ";
            if (!refs.empty()) text += "All uploaded images (1 through " + std::to_string(refs.size()) +
                ") are STYLE references only. Borrow their linework, palette, shading, texture and rendering. "
                "Do not copy their subjects, character identities, poses, text or compositions unless the user explicitly requests it. ";
            text += "Use the user's prompt to define the subject, scene, pose and composition. "
                "Return a single image without added panels, watermarks or borders unless requested.\nUser prompt:\n" +
                utf8(job.instruction);
            if (job.plugins) text = job.plugins->transform(context, std::move(text));
            send(i, L"正在生成…");
            image = generate(job.provider, text, uploads, cancel, [&](const Image& partial, int index) {
                cancel.verify(); send(i, L"收到局部预览 " + std::to_wstring(index + 1),
                    std::make_shared<Image>(limited(partial, 1024)), index);
            });
            cancel.verify();
            if (job.plugins) job.plugins->process(context, image);
            savePng(cached, image);
        } else send(i, L"已恢复缓存图像");
        if (job.upscale.output) {
            send(i, L"正在进行输出超分…");
            image = upscaler->run(image, false);
        }
        cancel.verify(); savePng(output, image);
        send(i + 1, L"完成：" + output.wstring(), std::make_shared<Image>(limited(image, 1024)));
    }
}
}
