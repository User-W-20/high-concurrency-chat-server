add_executable(server server.cpp
        group_manager.cpp
        ServerContext.cpp
        LuaManager.cpp
        Logger.cpp
        UserManager.cpp
        DatabaseManager.cpp
)
add_executable(client client.cpp)

find_library(ARGON2_LIBRARY NAMES argon2)

if(NOT ARGON2_LIBRARY)
    message(FATAL_ERROR "无法找到 Argon2 库 (libargon2)。请确保已安装开发包。")
endif()

target_link_libraries(server PRIVATE
        ${LUA_LIBRARIES}
        lua5.3
        ${ARGON2_LIBRARY}
        mysqlcppconn
)