#include "studio.hpp"
#include "plugins.hpp"
#include "upscale.hpp"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace studio {
std::string prompt(const Job& job, int index, int total, size_t styleCount, bool anchor,
                   const std::vector<int>& neighbors) {
    std::string text =
        "Edit image 1, the source meme. Replace its main person/character with the SAME "
        "identifiable character shown in image 2 (the identity reference). "
        "Preserve image 1's exact pose, facial expression, emotion, gesture, composition, "
        "camera angle, framing, background, props, and visual joke. Recreate the character's "
        "distinctive face, hair, outfit, colors and accessories from image 2 while adapting "
        "them to the source pose; do not paste the reference pose or its background. ";
    int styleStart = job.refine ? 2 : 3;
    if (job.refine) {
        text = "Refine image 1, an already generated meme. Apply ONLY the user's requested corrections. "
            "Keep the existing character identity, pose, facial expression, camera, background, props, "
            "composition and rendering unchanged unless explicitly requested. Do not replace the character again. ";
        text += "The editable rectangle in normalized coordinates is x=" + std::to_string(job.region.x) +
            ", y=" + std::to_string(job.region.y) + ", width=" + std::to_string(job.region.width) +
            ", height=" + std::to_string(job.region.height) +
            ". Confine changes to this rectangle. Return the entire image, not a crop. ";
    }
    if (styleCount) {
        text += "Images " + std::to_string(styleStart) + " through " + std::to_string(styleStart + styleCount - 1) +
            " are STYLE references only: borrow their linework, shading, rendering and "
            "texture, NOT their character identities, subjects, poses or compositions. ";
    } else {
        text += "Match the source meme's visual rendering unless additional instructions specify a style. ";
    }
    if (job.refine) text += "Preserve existing lettering unless the requested correction changes it. ";
    else if (job.preserveText)
        text += "Preserve all source captions, lettering, symbols and their placement verbatim. ";
    else
        text += "Remove source captions and lettering. ";
    for (size_t i = 0; i < neighbors.size(); ++i) {
        text += "Image " + std::to_string(styleStart + styleCount + i) +
            " is the already REDRAWN animation frame " + std::to_string(neighbors[i] + 1) +
            (neighbors[i] < index ? " BEFORE" : " AFTER") +
            " the current frame, at a distance of " + std::to_string(std::abs(neighbors[i] - index)) +
            " output frame(s). ";
    }
    if (!neighbors.empty()) {
        text += "Use these temporal references to interpolate motion naturally and maintain the same "
            "character design, rendering, linework, palette, lighting, background layout and props. "
            "Preserve intentional movement, occlusions, camera changes and scene changes in image 1. "
            "Do not average or freeze poses, copy a neighboring frame, or add panels. "
            "Image 1 alone defines the exact current pose, expression and composition. ";
    }
    if (anchor) {
        text += "The LAST image is the already edited first animation frame. Use it only to "
            "keep character design, proportions, costume, palette, rendering, background appearance "
            "and lighting consistent. Follow image 1 for this frame's expression, pose, camera "
            "and any intentional scene change; do NOT copy the anchor pose. ";
    }
    text += "Return only one complete edited image with the source aspect ratio and no added panels, "
        "watermarks or borders. This is frame " + std::to_string(index + 1) +
        " of " + std::to_string(total) + ". ";
    if (!job.instruction.empty()) text += "\nAdditional user instructions:\n" + utf8(job.instruction);
    return text;
}
static void validateRegion(const Crop& region) {
    if (!std::isfinite(region.x) || !std::isfinite(region.y) || !std::isfinite(region.width) ||
        !std::isfinite(region.height) || region.x < 0 || region.y < 0 || region.width <= 0 ||
        region.height <= 0 || region.x + region.width > 1.00000001 ||
        region.y + region.height > 1.00000001)
        throw Error("重绘区域必须是图片范围内的有效矩形");
}
Image preserveOutside(const Image& original, const Image& edited, const Crop& region) {
    validateRegion(region);
    if (original.empty() || edited.width != original.width || edited.height != original.height ||
        edited.pixels.size() != original.pixels.size()) throw Error("重绘图像尺寸不匹配");
    Image result = original;
    UINT left = UINT(std::clamp(std::lround(region.x * original.width), 0L, long(original.width - 1)));
    UINT top = UINT(std::clamp(std::lround(region.y * original.height), 0L, long(original.height - 1)));
    UINT right = UINT(std::clamp(std::lround((region.x + region.width) * original.width),
        long(left + 1), long(original.width)));
    UINT bottom = UINT(std::clamp(std::lround((region.y + region.height) * original.height),
        long(top + 1), long(original.height)));
    for (UINT y = top; y < bottom; ++y) {
        auto offset = (size_t(y) * original.width + left) * 4;
        std::copy_n(edited.pixels.data() + offset, size_t(right - left) * 4, result.pixels.data() + offset);
    }
    return result;
}
std::vector<fs::path> outputPaths(const Job& job) {
    if (job.generationCount < 1 || job.generationCount > 10) throw Error("生成张数必须在 1 到 10 之间");
    if (job.output.empty() || !job.output.has_filename()) throw Error("请选择输出文件");
    if (job.generationCount == 1) return {job.output};
    std::vector<fs::path> paths;
    for (int i = 0; i < job.generationCount; ++i) {
        std::wostringstream name;
        name << job.output.stem().wstring() << L"_" << std::setw(3) << std::setfill(L'0') << i + 1
            << job.output.extension().wstring();
        paths.push_back(job.output.parent_path() / name.str());
    }
    return paths;
}
static void runSingleJob(const Job& job, Cancel& cancel, const Reporter& report, Upscaler* upscaler) {
    cancel.verify();
    if (job.stride < 1 || job.stride > 600 || job.maxFrames < 0 || job.maxFrames > 600)
        throw Error("无效的 GIF 抽帧参数");
    if (job.styles.size() > 8) throw Error("最多支持 8 张画风参考图");
    if (job.output.empty()) throw Error("请选择输出文件");
    const auto output = fs::absolute(job.output).lexically_normal();
    std::vector<fs::path> inputs{job.source};
    if (!job.refine) inputs.push_back(job.character);
    inputs.insert(inputs.end(), job.styles.begin(), job.styles.end());
    for (const auto& input : inputs) {
        if (input.empty() || !fs::is_regular_file(input)) throw Error("输入图片不存在");
        if (_wcsicmp(fs::absolute(input).lexically_normal().c_str(), output.c_str()) == 0 ||
            (fs::exists(output) && fs::equivalent(input, output)))
            throw Error("输出文件不能覆盖输入素材或参考图");
    }
    report({0, 0, L"正在读取素材和参考图…"});
    auto animation = decode(job.source);
    Bytes identity;
    std::vector<Upload> refs;
    if (!job.refine) {
        identity = png(limited(cropped(decode(job.character).frames.front().image, job.crop)));
        refs.push_back({"identity", identity});
    }
    for (const auto& path : job.styles) refs.push_back({"style", png(limited(decode(path).frames.front().image))});
    int count = static_cast<int>(animation.frames.size()), stride = job.stride;
    if (job.maxFrames) stride = std::max(stride, (count + job.maxFrames - 1) / job.maxFrames);
    int total = (count + stride - 1) / stride;
    if (job.upscale.output) {
        const auto& image = animation.frames.front().image;
        uint64_t width = uint64_t(image.width) * job.upscale.scale;
        uint64_t height = uint64_t(image.height) * job.upscale.scale;
        if (width > 16384 || height > 16384 || width * height > 32000000 ||
            width * height * 4 * total > 384ull * 1024 * 1024)
            throw Error("超分后的成品超出图像或动画内存上限，请降低倍率");
    }
    std::wstring extension = output.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    if (animation.animated && extension != L".gif") throw Error("动画素材的输出扩展名必须为 .gif");
    if (!animation.animated && extension != L".png") throw Error("静态素材的输出扩展名必须为 .png");
    Json manifest{
        {"version", 3}, {"source", digest(readFile(job.source))}, {"identity", digest(identity)},
        {"styles", Json::array()}, {"endpoint", utf8(editUrl(job.provider.baseUrl))},
        {"model", job.provider.model}, {"quality", job.provider.quality},
        {"complexity", job.provider.complexity}, {"output_format", job.provider.outputFormat},
        {"output_compression", job.provider.compression},
        {"size", job.provider.size}, {"extra", job.provider.extra}, {"field", job.provider.imageField},
        {"stride", stride}, {"text", job.preserveText}, {"consistency", job.consistency},
        {"instruction", utf8(job.instruction)}, {"total", total},
        {"temporal_reference", {{"per_side", 2}, {"edge", 1024}, {"order", "midpoints"}}}
    };
    for (size_t i = job.refine ? 0 : 1; i < refs.size(); ++i) manifest["styles"].push_back(digest(refs[i].data));
    if (job.refine) manifest["refinement"] = {{"version", 1},
        {"region", {job.region.x, job.region.y, job.region.width, job.region.height}}};
    if (job.copyIndex) manifest["variation"] = job.copyIndex;
    if (job.plugins && !job.plugins->fingerprint(true).empty()) manifest["plugins"] = job.plugins->fingerprint(true);
    if (job.upscale.input) manifest["upscale_input"] = upscaler->fingerprint();
    auto serialized = manifest.dump();
    std::string signature = digest(Bytes(serialized.begin(), serialized.end()));
    fs::path cache = output; cache += L".memestudio";
    cache /= wide(signature.substr(0, 24));
    fs::create_directories(cache);
    writeJson(cache / L"manifest.json", manifest);
    std::vector<int> order;
    if (animation.animated && job.consistency && total > 2) {
        // Generate interval endpoints before their midpoint so both temporal directions are available.
        order = {0, total - 1};
        std::vector<std::pair<int, int>> intervals{{0, total - 1}};
        for (size_t i = 0; i < intervals.size(); ++i) {
            auto [first, last] = intervals[i];
            if (last - first <= 1) continue;
            int middle = first + (last - first) / 2;
            order.push_back(middle);
            intervals.emplace_back(first, middle);
            intervals.emplace_back(middle, last);
        }
        report({0, total, L"多帧参考：先生成首尾和关键帧，再补齐中间帧"});
    } else {
        for (int i = 0; i < total; ++i) order.push_back(i);
    }
    std::vector<Frame> generated(total);
    Bytes anchor;
    int completed = 0;
    for (int frameIndex : order) {
        cancel.verify();
        int sourceIndex = frameIndex * stride;
        std::wostringstream filename;
        filename << L"frame_" << std::setw(5) << std::setfill(L'0') << frameIndex << L".png";
        fs::path framePath = cache / filename.str();
        Image result;
        const auto& original = animation.frames[sourceIndex].image;
        if (job.resume && fs::exists(framePath)) {
            try { result = decode(framePath).frames.front().image; } catch (...) { result = {}; }
            if (result.width != original.width || result.height != original.height) result = {};
        }
        if (result.empty()) {
            std::vector<Upload> uploads{{"source", png(limited(original))}};
            uploads.insert(uploads.end(), refs.begin(), refs.end());
            if (job.upscale.input) for (auto& upload : uploads) {
                auto image = decode(upload.data);
                image = limited(image, std::max(1, 2048 / job.upscale.scale));
                upload.data = png(upscaler->run(image, true));
            }
            bool hasAnchor = job.consistency && !anchor.empty();
            std::vector<int> neighbors;
            int before = 0, after = 0;
            if (animation.animated && job.consistency) {
                for (int i = frameIndex - 1; i >= 0 && before < 2; --i) {
                    if (generated[i].image.empty()) continue;
                    neighbors.push_back(i); ++before;
                }
                std::reverse(neighbors.begin(), neighbors.end());
                for (int i = frameIndex + 1; i < total && after < 2; ++i) {
                    if (generated[i].image.empty()) continue;
                    neighbors.push_back(i); ++after;
                }
                // The first frame is already sent as the last anchor image.
                if (hasAnchor) neighbors.erase(std::remove(neighbors.begin(), neighbors.end(), 0), neighbors.end());
                for (int i : neighbors)
                    uploads.push_back({"temporal", png(limited(generated[i].image, 1024))});
            }
            if (hasAnchor) uploads.push_back({"anchor", anchor});
            Json context{{"source", utf8(job.source.wstring())}, {"character", utf8(job.character.wstring())},
                {"output", utf8(output.wstring())}, {"frame_index", frameIndex}, {"frame_count", total},
                {"source_frame_index", sourceIndex}, {"variation_index", job.copyIndex},
                {"instruction", utf8(job.instruction)}, {"mode", job.refine ? "refine" : "replace"},
                {"region", {{"x", job.region.x}, {"y", job.region.y},
                    {"width", job.region.width}, {"height", job.region.height}}}};
            auto text = prompt(job, frameIndex, total, job.styles.size(), hasAnchor, neighbors);
            if (job.plugins) text = job.plugins->transform(context, std::move(text));
            report({completed, total, L"正在生成第 " + std::to_wstring(frameIndex + 1) +
                L" / " + std::to_wstring(total) + L" 帧（前参考 " + std::to_wstring(before) +
                L"，后参考 " + std::to_wstring(after) + L"）…", {}, frameIndex});
            result = edit(job.provider, text, uploads, cancel, [&](const Image& image, int partialIndex) {
                    cancel.verify();
                    auto preview = job.refine ?
                        preserveOutside(original, resized(image, original.width, original.height), job.region) : image;
                    report({completed, total, L"第 " + std::to_wstring(frameIndex + 1) +
                        L" 帧：收到局部预览 " + std::to_wstring(partialIndex + 1),
                        std::make_shared<Image>(limited(preview, 1024)), frameIndex, partialIndex});
                });
            cancel.verify();
            result = resized(result, original.width, original.height);
            if (job.plugins) job.plugins->process(context, result);
            if (job.refine) result = preserveOutside(original, result, job.region);
            cancel.verify();
            savePng(framePath, result);
        } else {
            report({completed, total, L"已恢复缓存帧 " + std::to_wstring(frameIndex + 1)});
        }
        if (!frameIndex && job.consistency) anchor = png(limited(result, 1024));
        uint64_t duration = 0;
        for (int i = sourceIndex; i < std::min(sourceIndex + stride, count); ++i)
            duration += animation.frames[i].delayMs;
        if (duration > UINT_MAX) throw Error("GIF duration overflow");
        generated[frameIndex] = {std::move(result), static_cast<UINT>(duration)};
        ++completed;
        report({completed, total, L"已完成 " + std::to_wstring(completed) +
            L" / " + std::to_wstring(total) + L" 帧",
            std::make_shared<Image>(limited(generated[frameIndex].image, 1024)), frameIndex});
    }
    cancel.verify();
    if (job.upscale.output) for (size_t i = 0; i < generated.size(); ++i) {
        cancel.verify();
        report({total, total, L"Waifu2x：输出超分第 " + std::to_wstring(i + 1) +
            L" / " + std::to_wstring(generated.size()) + L" 帧…"});
        generated[i].image = upscaler->run(generated[i].image, false);
    }
    cancel.verify();
    report({total, total, L"正在写入输出文件…"});
    if (animation.animated) saveGif(output, generated, animation.loop);
    else savePng(output, generated.front().image);
    report({total, total, L"完成：" + output.wstring()});
}
void runJob(const Job& job, Cancel& cancel, const Reporter& report) {
    cancel.verify();
    if (job.refine) {
        validateRegion(job.region);
        if (job.instruction.find_first_not_of(L" \t\r\n") == std::wstring::npos)
            throw Error("请填写本次重绘的修改要求");
        if (job.stride != 1 || job.maxFrames != 0)
            throw Error("局部重绘必须保留所有原始帧，请使用 stride=1、max_frames=0");
    }
    auto paths = outputPaths(job);
    std::vector<fs::path> inputs{job.source};
    if (!job.refine) inputs.push_back(job.character);
    inputs.insert(inputs.end(), job.styles.begin(), job.styles.end());
    // Check every numbered destination before generating or overwriting any result.
    for (const auto& path : paths) for (const auto& input : inputs) {
        if (input.empty() || !fs::is_regular_file(input)) throw Error("输入图片不存在");
        if (_wcsicmp(fs::absolute(path).lexically_normal().c_str(),
            fs::absolute(input).lexically_normal().c_str()) == 0 ||
            (fs::exists(path) && fs::equivalent(path, input)))
            throw Error("输出文件不能覆盖输入素材或参考图");
    }
    std::unique_ptr<Upscaler> upscaler;
    if (job.upscale.input || job.upscale.output)
        upscaler = std::make_unique<Upscaler>(job.upscale, cancel, report);
    int knownTotal = 0;
    for (int i = 0; i < job.generationCount; ++i) {
        cancel.verify();
        Job copy = job; copy.output = paths[i]; copy.copyIndex = i; copy.generationCount = 1;
        runSingleJob(copy, cancel, [&](const Progress& progress) {
            auto event = progress;
            event.copy = i; event.copies = job.generationCount;
            if (progress.total) knownTotal = progress.total;
            if (job.generationCount > 1) {
                event.done = i * knownTotal + progress.done;
                event.total = job.generationCount * knownTotal;
                event.message = L"第 " + std::to_wstring(i + 1) + L" / " +
                    std::to_wstring(job.generationCount) + L" 份：" + progress.message;
            }
            report(event);
        }, upscaler.get());
    }
}
}
