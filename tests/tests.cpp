#include "../src/studio.hpp"
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>

namespace studio {
namespace {
void expect(bool value, const char* message) { if (!value) throw Error(message); }
template<class F> void rejects(F action, const char* message) {
    try { action(); } catch (const std::exception&) { return; }
    throw Error(message);
}
Image solid(UINT width, UINT height, BYTE red, BYTE green, BYTE blue, BYTE alpha = 255) {
    Image image{width, height, Bytes(size_t(width) * height * 4)};
    for (size_t p = 0; p < image.pixels.size(); p += 4) {
        image.pixels[p] = blue; image.pixels[p + 1] = green;
        image.pixels[p + 2] = red; image.pixels[p + 3] = alpha;
    }
    return image;
}
void rectangle(Image& image, int left, int top, int width, int height, BYTE r, BYTE g, BYTE b) {
    for (int y = std::max(0, top); y < std::min(int(image.height), top + height); ++y)
        for (int x = std::max(0, left); x < std::min(int(image.width), left + width); ++x) {
            auto* p = image.pixels.data() + (size_t(y) * image.width + x) * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
        }
}
Image portrait(int pose, bool reference) {
    auto image = solid(320, 240, reference ? 222 : 250, reference ? 241 : 247, reference ? 238 : 231);
    rectangle(image, 112, 125, 96, 95, reference ? 23 : 225, reference ? 134 : 106, reference ? 122 : 62);
    rectangle(image, 104, 33, 112, 91, reference ? 53 : 81, 61, reference ? 91 : 54);
    rectangle(image, 118, 55, 84, 77, 255, 209, 167);
    rectangle(image, 128, 79 + pose * 3, 12, 7, 30, 35, 40);
    rectangle(image, 179, 79 - pose * 2, 12, 7, 30, 35, 40);
    rectangle(image, 146, 105, 30, 6 + pose * 5, 120, 47, 52);
    rectangle(image, 72 - pose * 8, 139 - pose * 16, 40, 23, 255, 209, 167);
    rectangle(image, 208, 139 - pose * 16, 40 + pose * 8, 23, 255, 209, 167);
    return image;
}
// A small independent GIF fixture uses a clear code before each pixel, keeping
// the code width fixed. It exercises partial frames and disposal modes 2 and 3.
Bytes disposalFixture() {
    Bytes bytes{'G','I','F','8','9','a', 4,0,3,0, 0x81,0,0,
        0,0,0, 255,0,0, 0,255,0, 0,0,255,
        0x21,0xFF,0x0B,'N','E','T','S','C','A','P','E','2','.','0',3,1,2,0,0};
    auto frame = [&](BYTE left, BYTE top, BYTE width, BYTE height, BYTE color, BYTE disposal, BYTE delay) {
        Bytes header{0x21,0xF9,4,BYTE((disposal << 2) | 1),delay,0,0,0,
            0x2C,left,0,top,0,width,0,height,0,0,2};
        bytes.insert(bytes.end(), header.begin(), header.end());
        Bytes compressed; unsigned accumulator = 0, bits = 0;
        auto code = [&](unsigned value) {
            accumulator |= value << bits; bits += 3;
            while (bits >= 8) { compressed.push_back(BYTE(accumulator)); accumulator >>= 8; bits -= 8; }
        };
        for (int i = 0; i < width * height; ++i) { code(4); code(color); }
        code(5); if (bits) compressed.push_back(BYTE(accumulator));
        bytes.push_back(BYTE(compressed.size()));
        bytes.insert(bytes.end(), compressed.begin(), compressed.end()); bytes.push_back(0);
    };
    frame(0, 0, 4, 3, 1, 1, 8); frame(1, 1, 2, 1, 2, 2, 12);
    frame(3, 2, 1, 1, 3, 3, 20); frame(0, 0, 1, 1, 3, 1, 9);
    bytes.push_back(0x3B);
    return bytes;
}
BYTE channel(const Image& image, int x, int y, int component) {
    return image.pixels[(size_t(y) * image.width + x) * 4 + component];
}
}
int selfTest(const fs::path& directory) {
    fs::create_directories(directory);
    std::ostringstream log;
    int failures = 0, passed = 0;
    auto test = [&](const char* name, auto action) {
        try { action(); log << "PASS " << name << "\n"; ++passed; }
        catch (const std::exception& error) { log << "FAIL " << name << ": " << error.what() << "\n"; ++failures; }
    };
    test("Unicode paths, PNG roundtrip and alpha", [&] {
        auto image = solid(29, 17, 230, 80, 19);
        image.pixels[3] = 0; image.pixels[7] = 128;
        auto path = directory / L"中文路径-人物.png"; savePng(path, image);
        auto actual = decode(path).frames[0].image;
        expect(actual.pixels == image.pixels && actual.width == 29 && actual.height == 17, "PNG pixels differ");
    });
    test("Crop and resize dimensions", [&] {
        auto original = solid(100, 80, 1, 2, 3);
        auto image = cropped(original, {.25, .25, .5, .5});
        expect(image.width == 50 && image.height == 40, "Incorrect crop");
        image = resized(image, 37, 29); expect(image.width == 37 && image.height == 29, "Incorrect resize");
        rejects([&] { cropped(original, {0, 0, -1, 1}); }, "Negative crop accepted");
    });
    test("Base64, SHA-256 and malformed response", [&] {
        Bytes input{0, 1, 2, 127, 128, 255}; expect(unbase64(base64(input)) == input, "Base64 mismatch");
        expect(digest({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA mismatch");
        rejects([&] { unbase64("!!!!"); }, "Invalid base64 accepted");
        rejects([&] { decode(Bytes{1,2,3}); }, "Invalid image accepted");
    });
    test("Windows DPAPI encrypted credentials", [&] {
        auto encrypted = protect(L"test-key-not-a-real-secret");
        expect(encrypted.find("test-key") == std::string::npos, "Plain key persisted");
        expect(unprotect(encrypted) == L"test-key-not-a-real-secret", "DPAPI mismatch");
    });
    test("Endpoint normalization", [&] {
        expect(editUrl(L"https://example.test/v1/") == L"https://example.test/v1/images/edits", "Base endpoint");
        expect(editUrl(L"https://example.test/images") == L"https://example.test/images/edits", "Images endpoint");
        expect(editUrl(L"https://example.test/api/images/edits?version=1") ==
            L"https://example.test/api/images/edits?version=1", "Full endpoint changed");
    });
    test("Multipart roles, custom field, extra parameters", [&] {
        Provider provider; provider.imageField = "image"; provider.extra = {{"input_fidelity", "high"}};
        auto body = multipart(provider, "test prompt", {{"source", {1,2,3}}, {"identity", {4,5}}, {"style", {6}}});
        std::string data(body.data.begin(), body.data.end());
        expect(data.find("name=\"image\"; filename=\"image_2.png\"") != std::string::npos, "Missing third upload");
        expect(data.find("name=\"input_fidelity\"") != std::string::npos, "Extra parameter missing");
        provider.extra["n"] = 2;
        rejects([&] { multipart(provider, "", {{"source", {1}}}); }, "Reserved parameter accepted");
    });
    test("Prompt source, identity, style and anchor ordering", [&] {
        Job job; auto text = prompt(job, 1, 4, 2, true);
        expect(text.find("Images 3 through 4") != std::string::npos && text.find("LAST image") != std::string::npos,
            "Incorrect role order");
        expect(text.find("verbatim") != std::string::npos, "Text preservation missing");
    });
    test("Independent GIF fixture: offsets, disposal, transparency, loop", [&] {
        auto path = directory / L"disposal.gif"; writeFile(path, disposalFixture());
        auto animation = decode(path);
        expect(animation.frames.size() == 4 && animation.loop == 2, "GIF frame count or loop");
        auto& second = animation.frames[1].image;
        expect(channel(second, 1, 1, 1) == 255 && channel(second, 0, 0, 2) == 255, "Partial frame composition");
        auto& third = animation.frames[2].image;
        expect(channel(third, 1, 1, 3) == 0 && channel(third, 3, 2, 0) == 255, "Disposal 2 failed");
        auto& fourth = animation.frames[3].image;
        expect(channel(fourth, 3, 2, 2) == 255 && channel(fourth, 0, 0, 0) == 255, "Disposal 3 failed");
    });
    test("GIF encoder: timings, loop, palette and transparent pixels", [&] {
        auto red = solid(24, 18, 255, 0, 0); red.pixels[3] = 0;
        auto green = solid(24, 18, 0, 255, 0); green.pixels[3] = 0;
        auto path = directory / L"roundtrip.gif";
        saveGif(path, {{red, 80}, {green, 120}, {red, 200}}, 0);
        auto animation = decode(path);
        expect(animation.frames.size() == 3 && animation.loop == 0, "Encoded loop or frame count");
        expect(animation.frames[0].delayMs == 80 && animation.frames[1].delayMs == 120 &&
            animation.frames[2].delayMs == 200, "GIF timing changed");
        expect(channel(animation.frames[1].image, 0, 0, 3) == 0, "GIF transparency lost");
        expect(channel(animation.frames[1].image, 1, 0, 1) > 240, "GIF palette wrong");
    });
    test("Long GIF delays are split without losing duration", [&] {
        auto path = directory / L"long.gif"; saveGif(path, {{solid(2, 2, 23, 70, 19), 700000}}, 65536);
        auto animation = decode(path);
        UINT duration = 0; for (auto& frame : animation.frames) duration += frame.delayMs;
        expect(duration == 700000 && animation.frames.size() == 2 && animation.loop == 65536, "Long delay lost");
        saveGif(directory / L"one-repeat.gif", {{solid(2, 2, 2, 3, 4), 100}}, 1);
        expect(decode(directory / L"one-repeat.gif").loop == 1, "Explicit repeat count lost");
    });
    test("Canceled jobs and input overwrite rejected before request", [&] {
        Cancel cancel; cancel.stop(); Job job;
        rejects([&] { runJob(job, cancel, [](auto&) {}); }, "Cancellation ignored");
        auto source = directory / L"input.png"; savePng(source, solid(8, 8, 20, 30, 40));
        job.source = source; job.character = source; job.output = source; cancel.reset();
        rejects([&] { runJob(job, cancel, [](auto&) {}); }, "Input overwrite accepted");
    });
    test("Create demo fixtures", [&] {
        std::vector<Frame> frames{{portrait(0, false), 180}, {portrait(1, false), 140}, {portrait(2, false), 240}};
        saveGif(directory / L"source.gif", frames, 0);
        savePng(directory / L"source.png", frames[0].image);
        savePng(directory / L"character.png", portrait(0, true));
        auto style = solid(320, 240, 236, 228, 246);
        for (int i = 0; i < 6; ++i) rectangle(style, 24 + i * 46, 30 + i * 18, 35, 145 - i * 16,
            BYTE(25 + i * 30), BYTE(140 + i * 9), BYTE(120 + i * 13));
        savePng(directory / L"style.png", style);
    });
    log << "\n" << passed << " passed, " << failures << " failed\n";
    auto result = log.str(); writeFile(directory / L"results.txt", Bytes(result.begin(), result.end()));
    return failures ? 1 : 0;
}
int networkTest(const fs::path& directory, const std::wstring& base) {
    fs::create_directories(directory);
    std::string result;
    int failures = 0;
    auto test = [&](const char* name, auto action) {
        try { action(); result += std::string("PASS ") + name + "\n"; }
        catch (const std::exception& error) { result += std::string("FAIL ") + name + ": " + error.what() + "\n"; ++failures; }
    };
    Provider provider; provider.baseUrl = base; provider.key = L"test-token"; provider.timeoutSeconds = 5;
    std::vector<Upload> uploads{{"source", png(solid(8, 8, 10, 30, 40))}, {"identity", png(solid(8, 8, 20, 30, 40))}};
    test("Abort an in-flight synchronous WinHTTP request", [&] {
        provider.baseUrl = base + L"/slow";
        Cancel token;
        std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); token.stop(); });
        bool stopped = false;
        auto begin = std::chrono::steady_clock::now();
        try { edit(provider, "cancel", uploads, token); } catch (const std::exception&) { stopped = token.stopped; }
        stopper.join();
        expect(stopped && std::chrono::steady_clock::now() - begin < std::chrono::seconds(4), "In-flight cancel failed");
    });
    test("HTTP errors redact the API key", [&] {
        provider.baseUrl = base + L"/error"; Cancel token;
        try { edit(provider, "error", uploads, token); throw Error("Error response accepted"); }
        catch (const Error& error) {
            std::string text = error.what();
            expect(text.find("test-token") == std::string::npos &&
                text.find("[REDACTED]") != std::string::npos, "Secret leaked in error");
        }
    });
    test("HTTP redirects are rejected", [&] {
        provider.baseUrl = base + L"/redirect"; Cancel token;
        rejects([&] { edit(provider, "redirect", uploads, token); }, "Redirect accepted");
    });
    writeFile(directory / L"network-results.txt", Bytes(result.begin(), result.end()));
    return failures ? 1 : 0;
}
}
