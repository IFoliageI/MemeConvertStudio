#include "../../sdk/memestudio_plugin.h"
#include <cstring>

namespace {
const char* MEMESTUDIO_CALL command(const char* id, const char*, void*) {
    if (std::strcmp(id, "permissions") == 0) return "{\"action\":\"dsh_permissions\"}";
    return std::strcmp(id, "cancel") == 0 ? "{\"action\":\"dsh_cancel\"}" : nullptr;
}
const MemeStudioPluginV1 plugin{
    sizeof(MemeStudioPluginV1), MEMESTUDIO_PLUGIN_ABI, "memestudio.deepseek-harness",
    nullptr, nullptr, nullptr, command, nullptr
};
}
extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV1* MEMESTUDIO_CALL MemeStudioGetPluginV1(void) {
    return &plugin;
}
