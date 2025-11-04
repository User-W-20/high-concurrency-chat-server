cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD  17)
project(LiteChat)

find_package(Lua 5.3 REQUIRED)
if(NOT LUA_FOUND)
    message(FATAL_ERROR "Lua 5.3 库未找到！")
endif ()

find_path(MYSQLCPP_INCLUDE_DIR
        NAMES mysql_connection.h
        PATHS /usr/include /usr/local/include
        PATH_SUFFIXES mysql jdbc
)

find_library(MYSQLCPP_LIB
        NAMES mysqlcppconn
        PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
)
if (NOT MYSQLCPP_INCLUDE_DIR OR NOT MYSQLCPP_LIB)
    message(FATAL_ERROR "未找到 MySQL Connector/C++ 库，请确认已安装 libmysqlcppconn-dev")
else()
    message(STATUS "    找到 MySQL Connector/C++ 库：")
    message(STATUS "    头文件路径: ${MYSQLCPP_INCLUDE_DIR}")
    message(STATUS "    库文件路径: ${MYSQLCPP_LIB}")
endif()

include_directories(
        ${CMAKE_SOURCE_DIR}/include
        ${MYSQLCPP_INCLUDE_DIR}
        ${LUA_INCLUDE_DIR}
)

add_subdirectory(src)


