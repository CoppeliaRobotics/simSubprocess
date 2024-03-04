cmake_minimum_required(VERSION 3.16.3)
project(simSubprocess)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_MACOSX_RPATH 1)

find_package(Boost REQUIRED COMPONENTS filesystem)

set(Qt Qt5 CACHE STRING "Qt version to use (e.g. Qt5)")
set_property(CACHE Qt PROPERTY STRINGS Qt5 Qt6)
find_package(${Qt} COMPONENTS Core REQUIRED)

if(NOT COPPELIASIM_INCLUDE_DIR)
    if(DEFINED ENV{COPPELIASIM_ROOT_DIR})
        set(COPPELIASIM_INCLUDE_DIR $ENV{COPPELIASIM_ROOT_DIR}/programming/include)
    else()
        set(COPPELIASIM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    endif()
endif()
list(APPEND CMAKE_MODULE_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules
    ${COPPELIASIM_INCLUDE_DIR}/cmake)
find_package(CoppeliaSim 4.5.1.7 REQUIRED)

include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)

coppeliasim_generate_stubs(${CMAKE_CURRENT_BINARY_DIR}/generated XML_FILE ${CMAKE_CURRENT_SOURCE_DIR}/sourceCode/callbacks.xml LUA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/lua/simSubprocess.lua)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sourceCode/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

coppeliasim_add_plugin(simSubprocess SOURCES sourceCode/plugin.cpp)
target_link_libraries(simSubprocess PRIVATE Boost::filesystem)
target_link_libraries(simSubprocess PRIVATE ${Qt}::Core)
