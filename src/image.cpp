#include "studio.hpp"
#include <propvarutil.h>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace studio {
static ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> result;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&result)), "WIC factory");
    return result;
}
static Image allocate(UINT width, UINT height) {
    if (!width || !height || width > 16384 || height > 16384 ||
        uint64_t(width) * height > 32 * 1024 * 1024)
        throw Error("Image dimensions exceed limit (32 megapixels / 16384 px)");
    return {width, height, Bytes(size_t(width) * height * 4)};
}
static Image pixels(IWICBitmapSource* source) {
    UINT width, height;
    check(source->GetSize(&width, &height), "Image dimensions");
    auto image = allocate(width, height);
    ComPtr<IWICFormatConverter> converter;
    check(factory()->CreateFormatConverter(&converter), "Pixel converter");
    check(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "BGRA conversion");
    check(converter->CopyPixels(nullptr, width * 4,
        static_cast<UINT>(image.pixels.size()), image.pixels.data()), "Read pixels");
    return image;
}
static ComPtr<IWICBitmap> bitmap(const Image& image) {
    ComPtr<IWICBitmap> result;
    check(factory()->CreateBitmapFromMemory(image.width, image.height,
        GUID_WICPixelFormat32bppBGRA, image.width * 4, static_cast<UINT>(image.pixels.size()),
        const_cast<BYTE*>(image.pixels.data()), &result), "WIC bitmap");
    return result;
}
static UINT meta(IWICMetadataQueryReader* reader, const wchar_t* name, UINT fallback = 0) {
    PROPVARIANT value{};
    UINT result = fallback;
    if (reader && SUCCEEDED(reader->GetMetadataByName(name, &value))) {
        ULONG number;
        if (SUCCEEDED(PropVariantToUInt32(value, &number))) result = number;
    }
    PropVariantClear(&value);
    return result;
}
static Animation unpack(IWICBitmapDecoder* decoder) {
    UINT count;
    GUID container{};
    check(decoder->GetFrameCount(&count), "Frame count");
    check(decoder->GetContainerFormat(&container), "Image format");
    bool gif = container == GUID_ContainerFormatGif;
    if (count > 600) throw Error("GIF exceeds 600 frames; reduce its length first");
    Animation result;
    result.animated = gif && count > 1;
    if (!gif) count = 1;
    ComPtr<IWICMetadataQueryReader> global;
    decoder->GetMetadataQueryReader(&global);
    ComPtr<IWICBitmapFrameDecode> first;
    check(decoder->GetFrame(0, &first), "First frame");
    UINT width, height;
    first->GetSize(&width, &height);
    if (gif) {
        width = meta(global.Get(), L"/logscrdesc/Width", width);
        height = meta(global.Get(), L"/logscrdesc/Height", height);
        result.loop = 65536;
        PROPVARIANT app{}, data{};
        if (global && SUCCEEDED(global->GetMetadataByName(L"/appext/application", &app)) &&
            app.vt == (VT_VECTOR | VT_UI1) && app.caub.cElems == 11 &&
            (!memcmp(app.caub.pElems, "NETSCAPE2.0", 11) ||
             !memcmp(app.caub.pElems, "ANIMEXTS1.0", 11)) &&
            SUCCEEDED(global->GetMetadataByName(L"/appext/data", &data)) &&
            data.vt == (VT_VECTOR | VT_UI1) && data.caub.cElems >= 4 &&
            data.caub.pElems[0] == 3 && data.caub.pElems[1] == 1) {
            result.loop = data.caub.pElems[2] | (UINT(data.caub.pElems[3]) << 8);
        }
        PropVariantClear(&app); PropVariantClear(&data);
    }
    auto canvas = allocate(width, height);
    if (uint64_t(canvas.pixels.size()) * count > 384ull * 1024 * 1024)
        throw Error("Decoded animation exceeds 384 MB; resize or shorten it first");
    BYTE background[4]{};
    if (gif) {
        ComPtr<IWICPalette> palette;
        factory()->CreatePalette(&palette);
        if (SUCCEEDED(decoder->CopyPalette(palette.Get()))) {
            WICColor colors[256]{}; UINT copied = 0;
            palette->GetColors(256, colors, &copied);
            UINT index = meta(global.Get(), L"/logscrdesc/BackgroundColorIndex", 0);
            if (index < copied) memcpy(background, &colors[index], 4);
        }
        ComPtr<IWICMetadataQueryReader> firstMeta;
        first->GetMetadataQueryReader(&firstMeta);
        if (meta(firstMeta.Get(), L"/grctlext/TransparencyFlag")) memset(background, 0, 4);
        for (size_t p = 0; p < canvas.pixels.size(); p += 4) memcpy(canvas.pixels.data() + p, background, 4);
    }
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(index, &frame), "Decode frame");
        Image raw = pixels(frame.Get());
        if (!gif) { result.frames.push_back({std::move(raw), 100}); break; }
        ComPtr<IWICMetadataQueryReader> reader;
        frame->GetMetadataQueryReader(&reader);
        UINT left = meta(reader.Get(), L"/imgdesc/Left"), top = meta(reader.Get(), L"/imgdesc/Top");
        UINT disposal = meta(reader.Get(), L"/grctlext/Disposal");
        UINT delay = meta(reader.Get(), L"/grctlext/Delay", 10) * 10;
        if (!delay) delay = 100;
        Image previous;
        if (disposal == 3) previous = canvas;
        for (UINT y = 0; y < raw.height && y + top < height; ++y) {
            for (UINT x = 0; x < raw.width && x + left < width; ++x) {
                auto* src = raw.pixels.data() + (size_t(y) * raw.width + x) * 4;
                auto* dst = canvas.pixels.data() + (size_t(y + top) * width + x + left) * 4;
                if (src[3]) memcpy(dst, src, 4);
            }
        }
        result.frames.push_back({canvas, delay});
        if (disposal == 3) canvas = std::move(previous);
        if (disposal == 2) {
            BYTE clear[4]; memcpy(clear, background, 4);
            if (meta(reader.Get(), L"/grctlext/TransparencyFlag")) memset(clear, 0, 4);
            for (UINT y = top; y < height && y < top + raw.height; ++y)
                for (UINT x = left; x < width && x < left + raw.width; ++x)
                    memcpy(canvas.pixels.data() + (size_t(y) * width + x) * 4, clear, 4);
        }
    }
    return result;
}
Animation decode(const fs::path& path) {
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder), "Open image");
    return unpack(decoder.Get());
}
Image decode(const Bytes& data) {
    if (data.empty() || data.size() > 128 * 1024 * 1024) throw Error("Empty or oversized image response");
    ComPtr<IWICStream> stream;
    check(factory()->CreateStream(&stream), "Image stream");
    check(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()),
        static_cast<DWORD>(data.size())), "Image memory stream");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
        &decoder), "Decode response image");
    return unpack(decoder.Get()).frames.front().image;
}
Image resized(const Image& image, UINT width, UINT height) {
    if (image.width == width && image.height == height) return image;
    auto guard = allocate(width, height);
    ComPtr<IWICBitmapScaler> scaler;
    check(factory()->CreateBitmapScaler(&scaler), "Image scaler");
    check(scaler->Initialize(bitmap(image).Get(), width, height, WICBitmapInterpolationModeFant),
        "Resize image");
    return pixels(scaler.Get());
}
Image limited(const Image& image, UINT edge) {
    if (std::max(image.width, image.height) <= edge) return image;
    double ratio = double(edge) / std::max(image.width, image.height);
    return resized(image, std::max(1u, UINT(image.width * ratio)), std::max(1u, UINT(image.height * ratio)));
}
Image cropped(const Image& image, const Crop& crop) {
    if (!std::isfinite(crop.x) || !std::isfinite(crop.y) || !std::isfinite(crop.width) ||
        !std::isfinite(crop.height) || crop.x < 0 || crop.y < 0 || crop.width <= 0 ||
        crop.height <= 0 || crop.x + crop.width > 1.000001 || crop.y + crop.height > 1.000001)
        throw Error("Invalid character crop");
    UINT x = std::min(image.width - 1, UINT(crop.x * image.width));
    UINT y = std::min(image.height - 1, UINT(crop.y * image.height));
    UINT width = std::min(image.width - x, std::max(1u, UINT(crop.width * image.width)));
    UINT height = std::min(image.height - y, std::max(1u, UINT(crop.height * image.height)));
    auto result = allocate(width, height);
    for (UINT row = 0; row < height; ++row)
        memcpy(result.pixels.data() + size_t(row) * width * 4,
            image.pixels.data() + (size_t(row + y) * image.width + x) * 4, size_t(width) * 4);
    return result;
}
Bytes png(const Image& image) {
    ComPtr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "PNG stream");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG init");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "PNG frame");
    check(frame->Initialize(nullptr), "PNG frame init");
    check(frame->SetSize(image.width, image.height), "PNG size");
    auto format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "PNG format");
    check(frame->WriteSource(bitmap(image).Get(), nullptr), "PNG pixels");
    check(frame->Commit(), "PNG frame commit"); check(encoder->Commit(), "PNG commit");
    STATSTG stat{}; stream->Stat(&stat, STATFLAG_NONAME);
    Bytes result(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER start{}; stream->Seek(start, STREAM_SEEK_SET, nullptr);
    ULONG read = 0; check(stream->Read(result.data(), static_cast<ULONG>(result.size()), &read), "PNG read");
    if (read != result.size()) throw Error("Incomplete PNG stream");
    return result;
}
void savePng(const fs::path& path, const Image& image) { writeFile(path, png(image)); }
static void setMeta(IWICMetadataQueryWriter* writer, const wchar_t* name, USHORT number, bool boolean = false) {
    PROPVARIANT value{};
    value.vt = static_cast<VARTYPE>(boolean ? VT_BOOL : VT_UI2);
    if (boolean) value.boolVal = number ? VARIANT_TRUE : VARIANT_FALSE;
    else value.uiVal = number;
    check(writer->SetMetadataByName(name, &value), "GIF metadata");
}
void saveGif(const fs::path& path, const std::vector<Frame>& frames, UINT loop) {
    if (frames.empty()) throw Error("No GIF frames");
    fs::create_directories(path.parent_path().empty() ? fs::path(L".") : path.parent_path());
    fs::path temporary = path; temporary += L".part";
    {
        ComPtr<IWICStream> stream;
        check(factory()->CreateStream(&stream), "GIF stream");
        check(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE), "GIF output");
        ComPtr<IWICBitmapEncoder> encoder;
        check(factory()->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder), "GIF encoder");
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "GIF init");
        if (loop <= 65535) {
            ComPtr<IWICMetadataQueryWriter> writer;
            check(encoder->GetMetadataQueryWriter(&writer), "GIF global metadata");
            BYTE application[] = "NETSCAPE2.0";
            BYTE data[]{3, 1, BYTE(loop & 255), BYTE((loop >> 8) & 255), 0};
            PROPVARIANT value{}; value.vt = VT_VECTOR | VT_UI1;
            value.caub = {11, application};
            check(writer->SetMetadataByName(L"/appext/application", &value), "GIF loop application");
            value.caub = {5, data};
            check(writer->SetMetadataByName(L"/appext/data", &value), "GIF loop count");
        }
        for (const auto& item : frames) {
            if (item.image.width != frames[0].image.width || item.image.height != frames[0].image.height)
                throw Error("GIF frames must share dimensions");
            auto source = bitmap(item.image);
            ComPtr<IWICPalette> palette;
            check(factory()->CreatePalette(&palette), "GIF palette");
            check(palette->InitializeFromBitmap(source.Get(), 256, TRUE), "GIF quantization");
            ComPtr<IWICFormatConverter> indexed;
            check(factory()->CreateFormatConverter(&indexed), "GIF converter");
            check(indexed->Initialize(source.Get(), GUID_WICPixelFormat8bppIndexed,
                WICBitmapDitherTypeErrorDiffusion, palette.Get(), 50, WICBitmapPaletteTypeCustom),
                "GIF indexed conversion");
            WICColor colors[256]{}; UINT colorCount = 0;
            check(palette->GetColors(256, colors, &colorCount), "GIF colors");
            int transparent = -1;
            for (UINT i = 0; i < colorCount; ++i) if (!(colors[i] >> 24)) transparent = int(i);
            uint64_t remaining = std::max<uint64_t>(1, (uint64_t(item.delayMs) + 5) / 10);
            while (remaining) {
                USHORT delay = static_cast<USHORT>(std::min<uint64_t>(65535, remaining));
                remaining -= delay;
                ComPtr<IWICBitmapFrameEncode> frame;
                check(encoder->CreateNewFrame(&frame, nullptr), "GIF frame");
                check(frame->Initialize(nullptr), "GIF frame init");
                check(frame->SetSize(item.image.width, item.image.height), "GIF size");
                auto format = GUID_WICPixelFormat8bppIndexed;
                check(frame->SetPixelFormat(&format), "GIF format");
                check(frame->SetPalette(palette.Get()), "GIF frame palette");
                ComPtr<IWICMetadataQueryWriter> writer;
                check(frame->GetMetadataQueryWriter(&writer), "GIF frame metadata");
                setMeta(writer.Get(), L"/grctlext/Delay", delay);
                PROPVARIANT disposal{}; disposal.vt = VT_UI1; disposal.bVal = 2;
                check(writer->SetMetadataByName(L"/grctlext/Disposal", &disposal), "GIF disposal");
                if (transparent >= 0) {
                    setMeta(writer.Get(), L"/grctlext/TransparencyFlag", 1, true);
                    PROPVARIANT index{}; index.vt = VT_UI1; index.bVal = BYTE(transparent);
                    check(writer->SetMetadataByName(L"/grctlext/TransparentColorIndex", &index), "GIF alpha");
                }
                check(frame->WriteSource(indexed.Get(), nullptr), "GIF pixels");
                check(frame->Commit(), "GIF frame commit");
            }
        }
        check(encoder->Commit(), "GIF commit");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Error("Cannot finalize GIF");
}
}
