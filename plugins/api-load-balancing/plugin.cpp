#include "../../sdk/memestudio_plugin.h"
#include <cstring>
namespace {
const char* MEMESTUDIO_CALL command(const char* id, const char*, void*) {
    return std::strcmp(id, "configure-routing") == 0 ? "{\"action\":\"api_routing\"}" : nullptr;
}
const MemeStudioPluginV1 plugin{
    sizeof(MemeStudioPluginV1), MEMESTUDIO_PLUGIN_ABI, "memestudio.api-load-balancing",
    nullptr, nullptr, nullptr, command, nullptr
};
}
extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV1* MEMESTUDIO_CALL MemeStudioGetPluginV1(void) {
    return &plugin;
}
