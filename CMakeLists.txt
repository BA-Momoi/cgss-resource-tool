cmake_minimum_required(VERSION 3.10)
project(CGSS_Script)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(CGSS_STATIC "静态链接 MinGW 运行库（libgcc/libstdc++），发布版推荐开启" ON)
add_executable(CGSS_Script
    main.c
    lookup_common.c lookup_3d.c lookup_spine.c lookup_song.c lookup_action.c
    lookup_chart.c lookup_card.c lookup_voice.c lookup_stage.c lookup_main.c
    download.c net.c acb.c util.c
    unpack.c unpack_fbx.c unpack_res.c
    paper.c
    spine_convert.c preview.c texture_merge.cpp
    GBKswapUTF8.c sqlite3.c)

target_link_libraries(CGSS_Script winhttp gdiplus)

target_compile_options(CGSS_Script PRIVATE -fexec-charset=UTF-8)

# 独立的资源清单更新检查工具（check_update.exe）
add_executable(check_update
    check_update.c
    sqlite3.c)
target_link_libraries(check_update winhttp)
target_compile_options(check_update PRIVATE -fexec-charset=UTF-8)

if(CGSS_STATIC)
    target_link_options(CGSS_Script PRIVATE -static)
    target_link_options(check_update PRIVATE -static)
endif()
# usm解包工具
add_executable(usm usm.c)
target_compile_options(check_update PRIVATE -fexec-charset=UTF-8)
# 构建后把 spine_preview 网页资源拷到 exe 同目录（主菜单 4 预览用）
add_custom_command(TARGET CGSS_Script POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/spine_preview
        $<TARGET_FILE_DIR:CGSS_Script>/spine_preview
    COMMENT "Copying spine_preview assets next to CGSS_Script.exe")
