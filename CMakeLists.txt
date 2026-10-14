cmake_minimum_required(VERSION 3.24)
project(ltool-plugins LANGUAGES CXX)

# 这一棵是 ltool 的**插件仓库**：每个插件一个目录，能独立编出 ltool-<id>.dll。
# 主仓库（https://github.com/mateo-ki/ltool）负责宿主、core 和插件接口；这里只放插件本体，
# 加上 sdk/ —— 那是主仓库 tools/make_sdk.py 生成的一份快照（core 公开头 + ltoolcore.lib +
# 那半份构建函数），没有它这里链不出 dll（插件用的是 core 那一份 Utils/InfoRow/Archive，不是复制）。
#
# 本地怎么编（Windows，需要 MSVC + Qt6）：
#   cmake -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=<Qt6 所在>
#   cmake --build build --config Release
#   → build/plugins/ltool-<id>.dll，拷到 ltool.exe 旁边的 ltool-plugins\ 里就生效
# 或者只编一个：cmake --build build --config Release --target ltool-json

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network)

set(LTOOL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "ltool 插件 SDK（core 头 + 导入库）")
if(NOT EXISTS "${LTOOL_SDK_DIR}/lib/ltoolcore.lib")
    message(FATAL_ERROR
        "找不到 ${LTOOL_SDK_DIR}/lib/ltoolcore.lib —— sdk/ 是主仓库 tools/make_sdk.py 生成的快照，"
        "要么提交进来，要么 -DLTOOL_SDK_DIR=<那份目录>")
endif()
set(LTOOL_PLUGIN_OUT "${CMAKE_BINARY_DIR}/plugins")
include("${LTOOL_SDK_DIR}/ltool-plugin.cmake")

file(GLOB pluginDirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*)
foreach(d ${pluginDirs})
    if(NOT IS_DIRECTORY ${d} OR NOT EXISTS ${d}/CMakeLists.txt)
        continue()
    endif()
    get_filename_component(id ${d} NAME)
    # 类名按约定推：<首字母大写的 id>Plugin（repair→RepairPlugin、base64→Base64Plugin、
    # json→JsonPlugin…）。约定而不是表：表会和插件自己漂移，而插件的 CLASS_NAME 在源码里只有一处。

    add_subdirectory(${d} ${CMAKE_BINARY_DIR}/build-${id})
    set_target_properties(ltool-${id} PROPERTIES
        LTOOL_PLUGIN_CLASS "${clsBase}Plugin")
endforeach()

# ---- index.json：插件商店那一面读的就是它 ----
# 每 dll 的 sha256 / 大小 / 元数据都从**产物和 plugin.json** 现算，不另抄一份（抄的会过期）。
# CI 用 --base-url 指到 GitHub Release 的下载前缀；本地跑就留默认，只当自检用。
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(index
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_index.py
                --build ${CMAKE_BINARY_DIR}/plugins
                --src ${CMAKE_CURRENT_SOURCE_DIR}/plugins
                --out ${CMAKE_BINARY_DIR}/index.json
                --base-url "$CACHE{LTOOL_INDEX_BASE_URL}"
        DEPENDS ${pluginTargets}
        COMMENT "生成 dist/index.json")
endif()
