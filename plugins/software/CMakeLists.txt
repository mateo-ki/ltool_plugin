# ltool 插件：software
#
# 源文件用 GLOB 而不是手抄清单 —— 这个目录就是这个插件的全部本体，加一个 .cpp 不该再动第二处。
# 类名写死在这儿（和主仓库那份 ltool_add_tool(software SoftwarePlugin ...) 一致）：它是插件身份的一部分。
file(GLOB srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json)
ltool_add_plugin(software SoftwarePlugin ${srcs})
