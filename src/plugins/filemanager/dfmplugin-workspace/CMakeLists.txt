cmake_minimum_required(VERSION 3.10)

project(dfmplugin-workspace)
set(BIN_NAME dfm-workspace-plugin)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

FILE(GLOB_RECURSE WORKSPACE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.json"
    )

add_library(${BIN_NAME}
    SHARED
    ${WORKSPACE_FILES}
)

find_package(Dtk6 COMPONENTS Widget REQUIRED)

set_target_properties(${BIN_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${DFM_BUILD_PLUGIN_FILEMANAGER_DIR})

target_include_directories(${BIN_NAME} PRIVATE
    ${Qt6Widgets_PRIVATE_INCLUDE_DIRS}
)

target_link_libraries(${BIN_NAME}
    DFM6::base
    DFM6::framework
)

#install library file
install(TARGETS
    ${BIN_NAME}
    LIBRARY
    DESTINATION
    ${DFM_PLUGIN_FILEMANAGER_CORE_DIR}
)
