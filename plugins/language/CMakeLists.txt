# ltool 插件：language
file(GLOB srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json)
ltool_add_plugin(language LanguagePlugin ${srcs})
