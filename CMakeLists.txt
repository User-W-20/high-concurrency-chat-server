cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD  17)
project(LiteChat)

find_package(Lua 5.3 REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_subdirectory(src)


