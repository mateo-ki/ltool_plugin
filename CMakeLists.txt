cmake_minimum_required(VERSION 3.24)
project(ltool-plugins LANGUAGES CXX)

# 这一棵是 ltool 的**插件仓库**：每个插件一个目录，能独立编出 ltool-<id>.dll。
# 主仓库（https://github.com/mateo-ki/ltool）负责宿主、core 和插件接口；这里只放插件本体，
# 加上 sdk/ —— 那是主仓库 tools/make_sdk.py 生成的一份快照（core 公开头 + ltoolcore.lib +
# 那半份构建函数），没有它这里链不出 dll（插件用的是 core 那一份 Utils/InfoRow/Archive，不是复制）。
#
# 本地怎么编（Windows，需要 MSVC + Qt6 6.10.2）：
#   走 vcpkg（和 CI 同一条路，classic 模式装 qtbase）：
#     cmake -B build -G "Visual Studio 17 2022" -A x64 \
#       -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
#   或者 Qt 已经在别处（官方包/自己编的）：
#     cmake -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=<Qt6 所在>
#   两支都要再：cmake --build build --config Release
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
    add_subdirectory(${d} ${CMAKE_BINARY_DIR}/build-${id})
    # 类名在各自 plugins/<id>/CMakeLists.txt 里显式给（ltool_add_plugin(<id> <CLASS> …)），
    # 这里只收集 target 名给下面的 index 目标当依赖。
    list(APPEND pluginTargets ltool-${id})
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
