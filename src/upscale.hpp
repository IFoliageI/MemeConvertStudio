#pragma once
#include "studio.hpp"
#include <map>

namespace studio {
class Upscaler {
    UpscaleOptions options;
    Cancel& cancel;
    Reporter report;
    fs::path temporary;
    Json signature;
    std::map<std::string, fs::path> inputCache;
    unsigned index = 0;
public:
    Upscaler(const UpscaleOptions& options, Cancel& cancel, Reporter report);
    ~Upscaler();
    const Json& fingerprint() const { return signature; }
    Image run(const Image& image, bool input);
};
}
