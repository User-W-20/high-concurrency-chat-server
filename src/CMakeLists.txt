


add_executable(server server.cpp
group_manager.cpp
ServerContext.cpp)
add_executable(client client.cpp)

target_include_directories(server PRIVATE ${LITECHAT_INCLUDE_DIR})