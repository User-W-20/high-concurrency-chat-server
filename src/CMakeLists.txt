add_executable(server server.cpp
group_manager.cpp
ServerContext.cpp
LuaManager.cpp
)
add_executable(client client.cpp)



target_link_libraries(server PRIVATE ${LUA_LIBRARIES})