# ltool 插件的公共构建片段 —— 主仓库和插件仓库**共用这一份**（各写一套迟早漂）。
#
#   ltool_add_plugin(<id> <CLASS> <源文件...> <plugin.json>)
#
# 差别只在"core 从哪来"，用变量说清楚，不让两边各抄一遍：
#   · 主仓库：设 LTOOL_CORE_TARGET（同一棵树里的 ltoolcore target）+ LTOOL_DEPLOY_DIR，
#     产物 POST_BUILD 拷进 build\Release\ltool-plugins —— ltool.iss 按那一棵收包、
#     交付闭环也按 SHA256 比那里。编译选项仍由主仓库那份 ltool_target_setup 给（PCH、WinRT 头都在它里面）
#   · 插件仓库：只有 SDK 快照，设 LTOOL_SDK_DIR → 链 ${LTOOL_SDK_DIR}/lib/ltoolcore.lib，
#     并且这里必须**自己补齐**那套定义/系统库/UTF-8 开关（那边是靠 ltool_target_setup 带的）
# 为什么插件一定要链 core：页面要用的是**那一份** Utils / InfoRow / Archive / CaptureHistory，
# 复制一份就等于同一个后台状态出现两份（界面里关了广告拦截、主程序那份还在拦）。

# 插件里调到的 Win32 API 都由这里统一给（截图要 dwmapi、右键那一路要 rstrtmgr、录屏要 mf*）。
# 少一个就是"在这个仓库能编、在那个仓库链接报 unresolved"。
# 这份清单和主仓库 ltool_target_setup 里那份**必须一致** —— 只给 SDK 模式用，
# 主仓库模式走 ltool_target_setup，两边不会各半份。
set(LTOOL_PLUGIN_WINLIBS
    user32 shell32 ole32 advapi32 uuid runtimeobject version dwmapi rstrtmgr
    iphlpapi crypt32 wintrust ws2_32 winhttp mfplat mf mfreadwrite mfuuid d3d11 dxgi)

function(ltool_add_plugin id cls)
    # 判据放在函数里而不是文件末尾：include 这一份的时候，调用方的那几个变量还没设呢
    # （主仓库就是先 include 再 set 的顺序，写在文件级会当场 FATAL）
    if(NOT DEFINED LTOOL_PLUGIN_OUT)
        message(FATAL_ERROR "ltool_add_plugin(${id})：调用方必须先设 LTOOL_PLUGIN_OUT（dll 落哪儿）")
    endif()
    set(tgt ltool-${id})
    qt_add_plugin(${tgt} CLASS_NAME ${cls})
    target_sources(${tgt} PRIVATE ${ARGN})
    set_target_properties(${tgt} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${LTOOL_PLUGIN_OUT}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${LTOOL_PLUGIN_OUT})

    if(DEFINED LTOOL_SDK_DIR)
        if(NOT EXISTS "${LTOOL_SDK_DIR}/include/ltool-plugin.h")
            message(FATAL_ERROR
                "${tgt}：LTOOL_SDK_DIR=${LTOOL_SDK_DIR} 里没有 include/ltool-plugin.h —— "
                "那份快照是主仓库 tools/make_sdk.py 生成的，重新生成或把路径指对")
        endif()
        target_include_directories(${tgt} PRIVATE
            ${LTOOL_SDK_DIR}/include ${LTOOL_SDK_DIR}/include/onnxruntime)
        target_link_libraries(${tgt} PRIVATE
            ${LTOOL_SDK_DIR}/lib/ltoolcore.lib
            Qt6::Widgets Qt6::Concurrent Qt6::Network ${LTOOL_PLUGIN_WINLIBS})
        target_compile_definitions(${tgt} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(MSVC)
            # /utf-8 不是风格：源码里全是中文，默认按 GBK 读会把中文字面量当场编坏
            target_compile_options(${tgt} PRIVATE /utf-8 /permissive- /W3)
            # C++/WinRT 的投影头在 Windows SDK 的 cppwinrt 目录里，不在 MSVC 默认包含路径上。
            # 录屏（按窗口取帧）要用；主仓库那一侧是 ltool_target_setup 里同样的循环。
            foreach(_root "C:/Program Files (x86)/Windows Kits/10" "C:/Program Files/Windows Kits/10")
                set(_inc "${_root}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
                if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND EXISTS "${_inc}")
                    target_include_directories(${tgt} PRIVATE "${_inc}")
                    break()
                endif()
            endforeach()
        endif()
    elseif(DEFINED LTOOL_CORE_TARGET)
        target_link_libraries(${tgt} PRIVATE ${LTOOL_CORE_TARGET})
    else()
        message(FATAL_ERROR
            "${tgt}：LTOOL_CORE_TARGET（主仓库）和 LTOOL_SDK_DIR（插件仓库）都没设 —— "
            "插件必须链到 core 的那一份实现，不是复制一份")
    endif()

    # 只有主仓库做这一步：把 dll 摆进 exe 旁边那棵树，安装包按 build\Release\* 递归收
    if(DEFINED LTOOL_DEPLOY_DIR)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${LTOOL_DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${tgt}> ${LTOOL_DEPLOY_DIR}/$<TARGET_FILE_NAME:${tgt}>
            COMMENT "${tgt}: 拷进 ${LTOOL_DEPLOY_DIR}")
    endif()
endfunction()
