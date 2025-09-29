//
// Created by X on 2025/9/28.
//

#ifndef LITECHAT_LUAMANAGER_H
#define LITECHAT_LUAMANAGER_H
#include <mutex>
#include <queue>
#include <string>
#include <vector>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

class ServerContext;

class LuaManager
{
public:
    static LuaManager& initializeInstance(ServerContext&ctx);

    static LuaManager& getInstance();

    LuaManager(const LuaManager&) = delete;
    LuaManager& operator=(const LuaManager&) = delete;

    bool initialize();

    bool execute_command(const std::string& nickname,
                         const std::string& command,
                         const std::vector<std::string>& args);

    bool execute_command(const std::string &nickname,const std::string&raw_message);

    ~LuaManager();

private:
    LuaManager(ServerContext& ctx);

    void register_c_functions();

    static int lua_broadcast_message(lua_State* L);

    ServerContext& ctx_ref;

    lua_State* L;

    std::mutex mtx;

};


extern  LuaManager* global_lua_manager_instance;
#endif  // LITECHAT_LUAMANAGER_H