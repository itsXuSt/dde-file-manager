cmake_minimum_required(VERSION 3.10)

project(dde-filemanager-textindex)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(TextIndex_XML ${DFM_DBUS_XML_DIR}/org.deepin.Filemanager.TextIndex.xml)
set(FULL_TEXT_PATH "${CMAKE_SOURCE_DIR}/3rdparty/fulltext")

FILE(GLOB_RECURSE SRC_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.policy"
    "${FULL_TEXT_PATH}/*.cpp"
    "${FULL_TEXT_PATH}/*.h"
    "${CMAKE_SOURCE_DIR}/include/dfm-base/dfm_log_defines.h"
    )

find_package(PkgConfig REQUIRED)
find_package(Qt6 COMPONENTS Core DBus Gui REQUIRED)
find_package(Dtk6 COMPONENTS Core)
find_package(dfm6-search REQUIRED)

qt6_generate_dbus_interface(
   textindexdbus.h
   ${TextIndex_XML}
   OPTIONS -M -S
)

qt6_add_dbus_adaptor(SRC_FILES ${TextIndex_XML}
    textindexdbus.h TextIndexDBus)

#full-text search dependences
pkg_check_modules(Lucene REQUIRED IMPORTED_TARGET liblucene++ liblucene++-contrib)
pkg_check_modules(Docparser REQUIRED IMPORTED_TARGET docparser)
pkg_check_modules(GLIB REQUIRED glib-2.0)

add_library(${PROJECT_NAME}
    SHARED
    ${SRC_FILES}
)

set_target_properties(${PROJECT_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ../../)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GLIB_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/include
        ${FULL_TEXT_PATH}/..
)

target_link_libraries(${PROJECT_NAME}
    DFM6::base
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Dtk6::Core
    dfm6-search
    ${GLIB_LIBRARIES}
    PkgConfig::Lucene
    PkgConfig::Docparser
)

install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_LIBDIR}/deepin-service-manager/)
install(FILES ${PROJECT_NAME}.json DESTINATION share/deepin-service-manager/user/)
INSTALL_DBUS_SERVICE(org.deepin.Filemanager.TextIndex)
INSTALL_DCONFIG("org.deepin.dde.file-manager.textindex.json")
