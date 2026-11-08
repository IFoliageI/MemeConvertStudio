#include "include/memestudio_plugin.h"
#include <cstring>
#include <string>

namespace {
std::string prompt;
const MemeStudioHostV1* host = nullptr;
int MEMESTUDIO_CALL initialize(const MemeStudioHostV1* value) {
    host = value;
    if (host->log) host->log(host->context, "Example plugin initialized");
    return 0;
}
const char* MEMESTUDIO_CALL transform(const char*, const char* input) {
    prompt = std::string(input) + "\nMaintain the reference character's distinctive facial features.";
    return prompt.c_str();
}
int MEMESTUDIO_CALL frame(const char*, MemeStudioImageV1*) { return 0; }
const char* MEMESTUDIO_CALL command(const char* id, const char*, void*) {
    if (std::strcmp(id, "identity-note") != 0) return nullptr;
    return "{\"append_instruction\":\"Maintain consistent character identity across all outputs.\"}";
}
void MEMESTUDIO_CALL shutdown() { prompt.clear(); host = nullptr; }
const MemeStudioPluginV1 plugin{
    sizeof(MemeStudioPluginV1), MEMESTUDIO_PLUGIN_ABI, "memestudio.example",
    initialize, transform, frame, command, shutdown
};
}
extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV1* MEMESTUDIO_CALL MemeStudioGetPluginV1(void) {
    return &plugin;
}
