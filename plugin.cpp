#include "include/memestudio_plugin.h"
#include <cstring>
namespace {
const char* MEMESTUDIO_CALL command(const char* id, const char*, void*) {
    return std::strcmp(id, "test-parameters") == 0 ? "{\"action\":\"api_parameter_test\"}" : nullptr;
}
const MemeStudioPluginV1 plugin{
    sizeof(MemeStudioPluginV1), MEMESTUDIO_PLUGIN_ABI, "memestudio.api-parameter-test",
    nullptr, nullptr, nullptr, command, nullptr
};
}
extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV1* MEMESTUDIO_CALL MemeStudioGetPluginV1(void) {
    return &plugin;
}
