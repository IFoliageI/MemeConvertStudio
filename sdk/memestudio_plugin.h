#ifndef MEMESTUDIO_PLUGIN_H
#define MEMESTUDIO_PLUGIN_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMESTUDIO_PLUGIN_ABI 1u
#define MEMESTUDIO_PLUGIN_ENTRY "MemeStudioGetPluginV1"
#ifdef _WIN32
#define MEMESTUDIO_CALL __cdecl
#define MEMESTUDIO_EXPORT __declspec(dllexport)
#else
#define MEMESTUDIO_CALL
#define MEMESTUDIO_EXPORT
#endif

typedef struct MemeStudioHostV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    void (MEMESTUDIO_CALL *log)(void* context, const char* message_utf8);
} MemeStudioHostV1;

/* BGRA8, writable only during process_frame. Dimensions and storage are host-owned. */
typedef struct MemeStudioImageV1 {
    uint32_t struct_size;
    uint32_t width, height, stride;
    unsigned char* pixels;
} MemeStudioImageV1;

/*
 * Returned strings are borrowed UTF-8, valid until the next callback.
 * NULL from transform_prompt keeps the prompt; NULL from run_command means no action.
 * Callbacks must not throw exceptions across the ABI.
 */
typedef struct MemeStudioPluginV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;
    int (MEMESTUDIO_CALL *initialize)(const MemeStudioHostV1* host);
    const char* (MEMESTUDIO_CALL *transform_prompt)(const char* context_json, const char* prompt_utf8);
    int (MEMESTUDIO_CALL *process_frame)(const char* context_json, MemeStudioImageV1* image);
    const char* (MEMESTUDIO_CALL *run_command)(const char* command_id, const char* context_json, void* owner_window);
    void (MEMESTUDIO_CALL *shutdown)(void);
} MemeStudioPluginV1;

typedef const MemeStudioPluginV1* (MEMESTUDIO_CALL *MemeStudioPluginEntryV1)(void);

#ifdef __cplusplus
}
#endif
#endif
