cmake_minimum_required(VERSION 3.10)
project(CGSS_Script)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_executable(CGSS_Script
    main.c
    lookup_common.c lookup_3d.c lookup_spine.c lookup_song.c lookup_action.c
    lookup_chart.c lookup_card.c lookup_voice.c lookup_stage.c lookup_main.c
    download.c net.c acb.c util.c
    unpack.c unpack_fbx.c unpack_res.c
    GBKswapUTF8.c sqlite3.c)

target_link_libraries(CGSS_Script winhttp)

target_compile_options(CGSS_Script PRIVATE -fexec-charset=UTF-8)
