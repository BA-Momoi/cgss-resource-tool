get_filename_component(CGSS_GUI_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# These defaults match the current checkout layout. Every path is a cache
# variable so another machine can override it with -D...=path.
set(CGSS_GUI_CIMGUI_DIR
    "${CGSS_GUI_ROOT}/../SDL2/cimgui"
    CACHE PATH "cimgui source directory")
set(CGSS_GUI_SDL3_DIR
    "${CGSS_GUI_ROOT}/../sdl3_src/SDL3-3.4.14"
    CACHE PATH "SDL3 source directory")
set(CGSS_GUI_SDL3_STATIC
    "${CGSS_GUI_ROOT}/../sdl3_build/libSDL3.a"
    CACHE FILEPATH "SDL3 static library")
set(CGSS_GUI_SPINE_DIR
    "${CGSS_GUI_ROOT}/build/_spine36b/spine-c/spine-c"
    CACHE PATH "Spine 3.6 C runtime source directory")
set(CGSS_GUI_ASSETSTUDIO_DIR ""
    CACHE PATH "Optional AssetStudio directory copied beside CGSS_GUI.exe")
set(CGSS_GUI_ACB_RUNTIME_DIR ""
    CACHE PATH "Directory containing acb2wavs.exe and its DLL files")

if(NOT EXISTS "${CGSS_GUI_CIMGUI_DIR}/cimgui.cpp")
    message(FATAL_ERROR
        "找不到 cimgui: ${CGSS_GUI_CIMGUI_DIR}\n"
        "请用 -DCGSS_GUI_CIMGUI_DIR=... 指定目录")
endif()
if(NOT EXISTS "${CGSS_GUI_SDL3_DIR}/include/SDL3/SDL.h")
    message(FATAL_ERROR
        "找不到 SDL3: ${CGSS_GUI_SDL3_DIR}\n"
        "请用 -DCGSS_GUI_SDL3_DIR=... 指定目录")
endif()
if(NOT EXISTS "${CGSS_GUI_SDL3_STATIC}")
    message(FATAL_ERROR
        "找不到 SDL3 静态库: ${CGSS_GUI_SDL3_STATIC}\n"
        "请用 -DCGSS_GUI_SDL3_STATIC=... 指定文件")
endif()
if(NOT EXISTS "${CGSS_GUI_SPINE_DIR}/include/spine/spine.h")
    message(FATAL_ERROR
        "找不到 Spine 3.6 runtime: ${CGSS_GUI_SPINE_DIR}\n"
        "请用 -DCGSS_GUI_SPINE_DIR=... 指定目录")
endif()

set(CGSS_GUI_IMGUI_SOURCES
    "${CGSS_GUI_CIMGUI_DIR}/cimgui.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/cimgui_impl.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/imgui.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/imgui_draw.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/imgui_demo.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/imgui_widgets.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/imgui_tables.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/backends/imgui_impl_sdl3.cpp"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/backends/imgui_impl_sdlrenderer3.cpp")

file(GLOB CGSS_GUI_SPINE_SOURCES CONFIGURE_DEPENDS
    "${CGSS_GUI_SPINE_DIR}/src/spine/*.c")

add_executable(CGSS_GUI
    main.c
    resource_backend.c
    spine_scene.c
    voice_player.c
    bgm_player.c
    renderer3_bridge.cpp
    "${CGSS_GUI_ROOT}/spine_convert.c"
    "${CGSS_GUI_ROOT}/sticker.c"
    "${CGSS_GUI_ROOT}/util.c"
    "${CGSS_GUI_ROOT}/texture_merge.cpp"
    "${CGSS_GUI_ROOT}/sqlite3.c"
    ${CGSS_GUI_IMGUI_SOURCES}
    ${CGSS_GUI_SPINE_SOURCES})

target_include_directories(CGSS_GUI PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CGSS_GUI_CIMGUI_DIR}"
    "${CGSS_GUI_CIMGUI_DIR}/imgui"
    "${CGSS_GUI_CIMGUI_DIR}/imgui/backends"
    "${CGSS_GUI_SDL3_DIR}/include"
    "${CGSS_GUI_SDL3_DIR}/src/video"
    "${CGSS_GUI_SPINE_DIR}/include"
    "${CGSS_GUI_ROOT}")

target_compile_definitions(CGSS_GUI PRIVATE
    CIMGUI_USE_SDL3=1
    "-DIMGUI_IMPL_API=extern \"C\" ")
set_source_files_properties(main.c PROPERTIES
    COMPILE_DEFINITIONS "CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1;CIMGUI_USE_SDL3=1")
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(CGSS_GUI PRIVATE
        -finput-charset=UTF-8
        -fexec-charset=UTF-8)
endif()

target_link_libraries(CGSS_GUI PRIVATE
    "${CGSS_GUI_SDL3_STATIC}"
    m
    kernel32 user32 gdi32 winmm imm32
    ole32 oleaut32 version uuid advapi32 setupapi shell32 dinput8
    winhttp gdiplus)
if(CGSS_STATIC)
    target_link_options(CGSS_GUI PRIVATE -static)
endif()

set_target_properties(CGSS_GUI PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "CGSS_GUI")

add_custom_command(TARGET CGSS_GUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/game_ui"
        "$<TARGET_FILE_DIR:CGSS_GUI>/assets/game_ui")

foreach(CGSS_GUI_RUNTIME_FILE
        "${CGSS_GUI_ROOT}/master.mdb")
    if(EXISTS "${CGSS_GUI_RUNTIME_FILE}")
        add_custom_command(TARGET CGSS_GUI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CGSS_GUI_RUNTIME_FILE}"
                "$<TARGET_FILE_DIR:CGSS_GUI>")
    endif()
endforeach()

file(GLOB CGSS_GUI_MANIFEST_FILES "${CGSS_GUI_ROOT}/manifest_*.db")
foreach(CGSS_GUI_MANIFEST_FILE IN LISTS CGSS_GUI_MANIFEST_FILES)
    add_custom_command(TARGET CGSS_GUI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CGSS_GUI_MANIFEST_FILE}"
            "$<TARGET_FILE_DIR:CGSS_GUI>")
endforeach()

# acb2wavs is used by the home-page BGM player. Only its small managed
# runtime is copied; the large AssetStudio and ffmpeg payloads are unrelated.
if(NOT CGSS_GUI_ACB_RUNTIME_DIR)
    foreach(CGSS_GUI_ACB_RUNTIME_CANDIDATE
            "${CGSS_GUI_ROOT}/release/CGSS_ResourceTool"
            "${CGSS_GUI_ROOT}/build"
            "${CGSS_GUI_ROOT}/updata/CGSS_ResourceTool")
        if(EXISTS "${CGSS_GUI_ACB_RUNTIME_CANDIDATE}/acb2wavs.exe")
            set(CGSS_GUI_ACB_RUNTIME_DIR
                "${CGSS_GUI_ACB_RUNTIME_CANDIDATE}")
            break()
        endif()
    endforeach()
endif()
foreach(CGSS_GUI_ACB_RUNTIME_FILE
        acb2wavs.exe
        CommandLine.dll
        DereTore.Common.dll
        DereTore.Common.StarlightStage.dll
        DereTore.Exchange.Archive.ACB.dll
        DereTore.Exchange.Audio.HCA.dll
        DereTore.Interop.OS.dll)
    if(CGSS_GUI_ACB_RUNTIME_DIR AND
       EXISTS "${CGSS_GUI_ACB_RUNTIME_DIR}/${CGSS_GUI_ACB_RUNTIME_FILE}")
        add_custom_command(TARGET CGSS_GUI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CGSS_GUI_ACB_RUNTIME_DIR}/${CGSS_GUI_ACB_RUNTIME_FILE}"
                "$<TARGET_FILE_DIR:CGSS_GUI>/${CGSS_GUI_ACB_RUNTIME_FILE}")
    endif()
endforeach()

if(NOT CGSS_GUI_ASSETSTUDIO_DIR)
    foreach(CGSS_GUI_ASSETSTUDIO_CANDIDATE
            "${CGSS_GUI_ROOT}/AssetStudio"
            "${CGSS_GUI_ROOT}/release/CGSS_ResourceTool/AssetStudio"
            "${CGSS_GUI_ROOT}/build/AssetStudio")
        if(EXISTS "${CGSS_GUI_ASSETSTUDIO_CANDIDATE}/AssetStudio.CLI.exe")
            set(CGSS_GUI_ASSETSTUDIO_DIR
                "${CGSS_GUI_ASSETSTUDIO_CANDIDATE}")
            break()
        endif()
    endforeach()
endif()
if(CGSS_GUI_ASSETSTUDIO_DIR AND
   EXISTS "${CGSS_GUI_ASSETSTUDIO_DIR}/AssetStudio.CLI.exe")
    add_custom_command(TARGET CGSS_GUI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CGSS_GUI_ASSETSTUDIO_DIR}"
            "$<TARGET_FILE_DIR:CGSS_GUI>/AssetStudio")
endif()

add_custom_command(TARGET CGSS_GUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "CGSS GUI built: $<TARGET_FILE:CGSS_GUI>")
