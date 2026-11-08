#include "../../sdk/memestudio_plugin.h"
#include <cstring>

namespace {
const char* MEMESTUDIO_CALL command(const char* id, const char*, void*) {
    return std::strcmp(id, "refine-result") == 0 ? "{\"action\":\"refine\"}" : nullptr;
}
const MemeStudioPluginV1 plugin{
    sizeof(MemeStudioPluginV1), MEMESTUDIO_PLUGIN_ABI, "memestudio.local-redraw",
    nullptr, nullptr, nullptr, command, nullptr
};
}
extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV1* MEMESTUDIO_CALL MemeStudioGetPluginV1(void) {
    return &plugin;
}
