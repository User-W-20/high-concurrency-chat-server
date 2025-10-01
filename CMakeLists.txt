cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD  17)
project(LiteChat)

find_package(Lua 5.3 REQUIRED)
if(NOT LUA_FOUND)
    message(FATAL_ERROR "Lua 5.3 库未找到！")
endif ()

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${LUA_INCLUDE_DIR})
add_subdirectory(src)


