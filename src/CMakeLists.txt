


add_executable(server server.cpp)
add_executable(client client.cpp)

target_include_directories(server PRIVATE ${LITECHAT_INCLUDE_DIR})