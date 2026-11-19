cmake_minimum_required(VERSION 3.20)
project(MemeStudio LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "MemeStudio requires Windows 10 or newer.")
endif()
add_library(MemeStudioCore STATIC src/util.cpp src/image.cpp src/api.cpp src/api_routing.cpp src/api_probe.cpp
    src/job.cpp src/job_config.cpp src/batch.cpp src/drawing.cpp src/plugins.cpp src/refine.cpp src/upscale.cpp)
add_executable(MemeStudio WIN32 src/main.cpp src/gui.cpp src/dsh.cpp src/dsh_dialog.cpp src/api_routing_dialog.cpp
    src/api_comparison.cpp src/api_probe_dialog.cpp src/drawing_dialog.cpp tests/tests.cpp tests/api_probe_test.cpp
    tests/routing_test.cpp tests/dsh_test.cpp)
add_executable(MemeStudio-cli src/cli.cpp src/cli_config.cpp)
target_compile_features(MemeStudioCore PUBLIC cxx_std_20)
target_compile_definitions(MemeStudioCore PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(MemeStudioCore PUBLIC user32 gdi32 msimg32 windowscodecs ole32 oleaut32 uuid comctl32 comdlg32 shell32 shlwapi winhttp wintrust crypt32 bcrypt propsys advapi32)
target_link_libraries(MemeStudio PRIVATE MemeStudioCore)
target_link_libraries(MemeStudio-cli PRIVATE MemeStudioCore)
add_dependencies(MemeStudio-cli MemeStudio)
if(MSVC)
    foreach(target IN ITEMS MemeStudioCore MemeStudio MemeStudio-cli)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /EHsc)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
    target_link_options(MemeStudio PRIVATE /ENTRY:wmainCRTStartup "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
    target_link_options(MemeStudio-cli PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
endif()
foreach(plugin IN ITEMS example local-redraw waifu2x batch-convert prompt-draw api-parameter-test api-load-balancing qwen-image deepseek-harness)
    set(plugin_target "MemeStudioPlugin-${plugin}")
    add_library(${plugin_target} SHARED plugins/${plugin}/plugin.cpp)
    target_compile_features(${plugin_target} PRIVATE cxx_std_20)
    target_compile_definitions(${plugin_target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(plugin STREQUAL "qwen-image")
        target_link_libraries(${plugin_target} PRIVATE user32 gdi32 ole32 uuid winhttp crypt32 bcrypt)
        foreach(resource IN ITEMS server.py environment.py model_info.py sdcpp_backend.py requirements.txt README.md)
            add_custom_command(TARGET MemeStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MemeStudio>/plugins/qwen-image"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugins/qwen-image/${resource}"
                    "$<TARGET_FILE_DIR:MemeStudio>/plugins/qwen-image/${resource}")
        endforeach()
    endif()
    set_target_properties(${plugin_target} PROPERTIES PREFIX "" OUTPUT_NAME "${plugin}")
    if(MSVC)
        target_compile_options(${plugin_target} PRIVATE /utf-8 /W4 /EHsc)
        set_property(TARGET ${plugin_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_dependencies(MemeStudio ${plugin_target})
    add_custom_command(TARGET MemeStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MemeStudio>/plugins/${plugin}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${plugin_target}>" "$<TARGET_FILE_DIR:MemeStudio>/plugins/${plugin}/${plugin}.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugins/${plugin}/plugin.json" "$<TARGET_FILE_DIR:MemeStudio>/plugins/${plugin}/plugin.json")
endforeach()
add_custom_command(TARGET MemeStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MemeStudio>/plugins/deepseek-harness/locale")
foreach(resource IN ITEMS package.json cordis.patch.yml index.js icon.svg README.md locale/en.json locale/zh.json)
    add_custom_command(TARGET MemeStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugins/deepseek-harness/${resource}"
            "$<TARGET_FILE_DIR:MemeStudio>/plugins/deepseek-harness/${resource}")
endforeach()
enable_testing()
add_test(NAME studio-self-test COMMAND MemeStudio --self-test "${CMAKE_CURRENT_BINARY_DIR}/test-output")
add_test(NAME studio-cli-help COMMAND MemeStudio-cli --help)
