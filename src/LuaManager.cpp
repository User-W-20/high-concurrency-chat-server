//
// Created by wxx on 2025/9/29.
//
#include "../include/LuaManager.h"
#include "../include/ServerContext.h"
#include "../include/Logger.h"

LuaManager* global_lua_manager_instance = nullptr;

static std::vector<std::string> split_message(const std::string& str)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (ss >> token)
    {
        tokens.push_back(token);
    }
    return tokens;
}

static int kick_user_to_lua(lua_State* L)
{
    if (!global_lua_manager_instance)
    {
        LOG_ERROR("Lua API Error: LuaManager 实例未初始化。");
        lua_pushboolean(L, 0);
        return 1;
    }

    if (lua_gettop(L) != 2 || !lua_isstring(L, 1) || !lua_isstring(L, 2))
    {
        LOG_ERROR("Lua API Error: Chat.kick_user 必须提供两个字符串参数 (目标昵称, 管理员昵称)。");
        lua_pushboolean(L, 0);
        return 1;
    }

    const char* target_nickname = lua_tostring(L, 1);
    const char* admin_nickname = lua_tostring(L, 2);

    ServerContext& ctx = global_lua_manager_instance->getServerContext();

    if (!ctx.is_user_admin(admin_nickname))
    {
        LOG_ERROR(
            "安全警报：用户 [" + std::string(admin_nickname) +
            "] 绕过 Lua 权限检查，尝试踢人。已在 C++ 层面阻止。");

        lua_pushboolean(L, 0);

        return 1;
    }

    bool success = ctx.kick_user_by_nickname(target_nickname, admin_nickname);

    lua_pushboolean(L, success ? 1 : 0);
    return 1;
}


LuaManager& LuaManager::initializeInstance(ServerContext& ctx)
{
    if (global_lua_manager_instance == nullptr)
    {
        global_lua_manager_instance = new LuaManager(ctx);
    }

    return *global_lua_manager_instance;
}


LuaManager& LuaManager::getInstance()
{
    if (global_lua_manager_instance == nullptr)
    {
        LOG_FATAL("LuaManager 尚未初始化! 请在 main 函数中调用 initializeInstance()");
        throw std::runtime_error("LuaManager not initialized.");
    }

    return *global_lua_manager_instance;
}


LuaManager::LuaManager(ServerContext& ctx) : ctx_ref(ctx), L(nullptr)
{
}

LuaManager::~LuaManager()
{
    if (L)
    {
        lua_close(L);
    }
}

bool LuaManager::initialize()
{
    std::lock_guard<std::mutex> lock(mtx);

    L = luaL_newstate();
    if (L == nullptr)
    {
        LOG_ERROR("Lua 状态机创建失败.");
        return false;
    }

    luaL_openlibs(L);

    register_c_functions();

    if (luaL_dofile(L, "../../src/commands.lua") != LUA_OK)
    {
        const char* error = lua_tostring(L, -1);
        LOG_ERROR("加载 commands.lua 失败: "+std::string(error));
        lua_pop(L, 1);
        return false;
    }

    LOG_INFO("Lua 虚拟机初始化成功，并成功加载 commands.lua。");
    return true;
}


int LuaManager::lua_broadcast_message(lua_State* L)
{
    if (!global_lua_manager_instance)
    {
        return 0;
    }
    ServerContext& ctx = global_lua_manager_instance->ctx_ref;

    int n = lua_gettop(L);
    if (n < 2)
    {
        LOG_ERROR("lua broadcast 错误: 参数不足 (需要发送者昵称和消息).");
        return 0;
    }

    const char* sender_nickname = lua_tostring(L, 1);
    const char* message = lua_tostring(L, 2);

    if (sender_nickname && message)
    {
        std::string full_msg = "[" + std::string(sender_nickname) + "(lua)]: " +
                               std::string(message);
        ctx.broadcast(full_msg, -1);
        LOG_INFO("Lua 广播成功: "+full_msg);
    }
    return 1;
}

void LuaManager::register_c_functions()
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_broadcast_message);
    lua_setfield(L, -2, "broadcast");

    lua_pushcfunction(L, kick_user_to_lua);
    lua_setfield(L, -2, "kick_user");

    lua_setglobal(L, "Chat");
}

bool LuaManager::execute_command(const std::string& nickname, bool is_admin,
                                 const std::string& full_msg)
{
    std::vector<std::string> parts = split_message(full_msg);
    if (parts.empty())
    {
        return false;
    }

    std::string command = parts[0];

    if (command.length() > 0 && command[0] == '/')
    {
        command = command.substr(1);
    }

    std::string lua_func_name = "lua_cmd_" + command;
    lua_getglobal(L, lua_func_name.c_str());

    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }

    lua_pushstring(L, nickname.c_str());

    lua_pushboolean(L, is_admin);

    lua_newtable(L);

    for (size_t i = 1; i < parts.size(); ++i)
    {
        lua_pushnumber(L, i);
        lua_pushstring(L, parts[i].c_str());
        lua_settable(L, -3);
    }

    if (lua_pcall(L, 3, 1, 0) != LUA_OK)
    {
        if (lua_isstring(L, -1))
        {
            LOG_ERROR("Lua 命令执行失败: " +std::string(lua_tostring(L,-1)));
        }
        lua_pop(L, 1);
        return false;
    }

    bool handled = false;
    if (lua_isboolean(L, -1))
    {
        handled = lua_toboolean(L, -1);
    }

    lua_pop(L, 1);
    return handled;
}