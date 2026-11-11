#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include "../vendor/nlohmann/json.hpp"

namespace studio {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
void check(HRESULT hr, const char* operation);
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
Bytes readFile(const fs::path& path, size_t limit = 128 * 1024 * 1024);
void writeFile(const fs::path& path, const Bytes& data);
void writeJson(const fs::path& path, const Json& data);
Json readJson(const fs::path& path);
std::string digest(const Bytes& data);
std::string base64(const Bytes& data);
Bytes unbase64(const std::string& data);
std::string protect(const std::wstring& key);
std::wstring unprotect(const std::string& data);
fs::path settingsPath();

struct Image {
    UINT width = 0, height = 0;
    Bytes pixels;
    bool empty() const { return !width || !height || pixels.empty(); }
};
struct Frame { Image image; UINT delayMs = 100; };
// 65536 denotes an absent loop extension; 0 is infinite, 1..65535 are repeats.
struct Animation { std::vector<Frame> frames; UINT loop = 65536; bool animated = false; };
struct Crop { double x = 0, y = 0, width = 1, height = 1; };
Animation decode(const fs::path& path);
Image decode(const Bytes& data);
Image resized(const Image& image, UINT width, UINT height);
Image cropped(const Image& image, const Crop& crop);
Image limited(const Image& image, UINT edge = 2048);
Bytes png(const Image& image);
void savePng(const fs::path& path, const Image& image);
void saveGif(const fs::path& path, const std::vector<Frame>& frames, UINT loop);

struct Provider {
    std::wstring baseUrl = L"https://api.openai.com/v1";
    std::wstring key;
    std::string model = "gpt-image-1";
    std::string quality = "auto";
    std::string size = "auto";
    std::string imageField = "image[]";
    std::string complexity, outputFormat;
    int compression = -1;
    bool stream = false;
    int partialImages = 2;
    Json extra = Json::object();
    int timeoutSeconds = 600;
};
struct Cancel {
    std::atomic_bool stopped{false};
    std::mutex mutex;
    void* request = nullptr;
    void reset();
    void stop();
    void attach(void* handle);
    void release();
    void verify() const;
    ~Cancel();
};
struct Upload { std::string name; Bytes data; };
struct Multipart { std::string type; Bytes data; };
std::wstring editUrl(std::wstring base);
std::wstring generationUrl(const std::wstring& base);
std::wstring modelsUrl(const std::wstring& base);
std::vector<std::string> listModels(const Provider& provider, Cancel& cancel);
Multipart multipart(const Provider& provider, const std::string& prompt,
                    const std::vector<Upload>& images);
Json generationBody(const Provider& provider, const std::string& prompt);
Image generate(const Provider& provider, const std::string& prompt,
               const std::vector<Upload>& styles, Cancel& cancel,
               const std::function<void(const Image&, int)>& preview = {});
Image edit(const Provider& provider, const std::string& prompt,
           const std::vector<Upload>& images, Cancel& cancel,
           const std::function<void(const Image&, int)>& preview = {});

class Plugins;
struct UpscaleOptions {
    fs::path executable, models;
    bool input = false, output = false, tta = false;
    int scale = 2, noise = 0, tile = 0, gpu = -2, timeout = 600;
};
struct Job {
    Provider provider;
    fs::path source, character, output;
    std::vector<fs::path> styles;
    Crop crop;
    std::wstring instruction;
    int stride = 1;
    int maxFrames = 0;
    int generationCount = 1, copyIndex = 0;
    std::shared_ptr<Plugins> plugins;
    bool consistency = true;
    bool preserveText = true;
    bool resume = true;
    bool refine = false;
    bool generate = false;
    Crop region;
    UpscaleOptions upscale;
};
struct Progress {
    int done = 0, total = 0;
    std::wstring message;
    std::shared_ptr<Image> preview;
    int frame = -1, partial = -1;
    int copy = 0, copies = 1;
    int frameTotal = 0;
};
using Reporter = std::function<void(const Progress&)>;
std::string prompt(const Job& job, int index, int total, size_t styleCount, bool anchor,
                   const std::vector<int>& neighbors = {});
void runJob(const Job& job, Cancel& cancel, const Reporter& report);
std::vector<fs::path> outputPaths(const Job& job);
Image preserveOutside(const Image& original, const Image& edited, const Crop& region);
bool editRefinement(HINSTANCE instance, HWND owner, Job& job, bool capture = false);
bool editDrawing(HINSTANCE instance, HWND owner, Job& job, bool capture = false);
void runDrawing(const Job& job, Cancel& cancel, const Reporter& report);
Json upscaleJson(const UpscaleOptions& options);
UpscaleOptions upscaleOptions(const Json& value);
bool editUpscale(HINSTANCE instance, HWND owner, UpscaleOptions& options, bool capture = false);
int runGui(HINSTANCE instance, const fs::path& demo = {}, bool capture = false);
int selfTest(const fs::path& directory);
int networkTest(const fs::path& directory, const std::wstring& base);
}
