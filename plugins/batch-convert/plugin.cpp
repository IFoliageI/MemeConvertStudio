#include "../../sdk/memestudio_plugin.h"
#include <cstring>

namespace {
const char* MEMESTUDIO_CALL command(const char* id, const char*, void*) {
    if (std::strcmp(id, "import-files") == 0) return "{\"action\":\"batch_import\"}";
    if (std::strcmp(id, "import-folder") == 0) return "{\"action\":\"batch_folder\"}";
    return nullptr;
}
const MemeStudioPluginV1 plugin{
    sizeof(MemeStudioPluginV1), MEMESTUDIO_PLUGIN_ABI, "memestudio.batch-convert",
    nullptr, nullptr, nullptr, command, nullptr
};
}
extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV1* MEMESTUDIO_CALL MemeStudioGetPluginV1(void) {
    return &plugin;
}
