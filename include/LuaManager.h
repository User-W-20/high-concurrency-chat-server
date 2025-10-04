//
// Created by X on 2025/9/28.
//

#ifndef LITECHAT_LUAMANAGER_H
#define LITECHAT_LUAMANAGER_H
#include <mutex>
#include <string>

extern "C"
{
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

struct ServerContext;

class LuaManager
{
public:
    static LuaManager& initializeInstance(ServerContext& ctx);

    static LuaManager& getInstance();

    LuaManager(const LuaManager&) = delete;
    LuaManager& operator=(const LuaManager&) = delete;

    bool initialize();

    [[nodiscard]] ServerContext& getServerContext() const
    {
        return ctx_ref;
    }

    bool execute_command(const std::string& nickname, bool is_admin,
                         const std::string& full_msg);
    ~LuaManager();

private:
    explicit LuaManager(ServerContext& ctx);

    void register_c_functions();

    static int lua_broadcast_message(lua_State* L);

    ServerContext& ctx_ref;

    lua_State* L;

    std::mutex mtx;
};


extern LuaManager* global_lua_manager_instance;
#endif  // LITECHAT_LUAMANAGER_H