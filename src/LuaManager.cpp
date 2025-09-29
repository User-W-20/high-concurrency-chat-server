//
// Created by wxx on 2025/9/29.
//
#include "../include/LuaManager.h"
#include "../include/ServerContext.h"
#include "../include/Logger.h"

LuaManager* global_lua_manager_instance = nullptr;

static std::vector<std::string>split_message(const std::string &str)
{
    std::vector<std::string>tokens;
    std::stringstream ss(str);
    std::string token;
    while (ss>>token)
    {
        tokens.push_back(token);
    }
    return  tokens;
}

LuaManager& LuaManager::initializeInstance(ServerContext& ctx)
{
    if (global_lua_manager_instance==nullptr)
    {
        global_lua_manager_instance=new LuaManager(ctx);
    }

    return  *global_lua_manager_instance;
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
    std::lock_guard<std::mutex>lock(mtx);

    L=luaL_newstate();
    if (L==nullptr)
    {
        LOG_ERROR("Lua 状态机创建失败.");
        return  false;
    }

    luaL_openlibs(L);

    register_c_functions();

    if (luaL_dofile(L,"../../src/commands.lua")!=LUA_OK)
    {
        const char* error=lua_tostring(L,-1);
        LOG_ERROR("加载 commands.lua 失败: "+std::string(error));
        lua_pop(L,1);
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

    const char* sender_nickname=lua_tostring(L,1);
    const char* message=lua_tostring(L,2);

    if (sender_nickname&&message)
    {
        std::string full_msg="["+std::string(sender_nickname)+"(lua)]: "+std::string(message);
        ctx.broadcast(full_msg,-1);
        LOG_INFO("Lua 广播成功: "+full_msg);
    }

    return  0;
}

void LuaManager::register_c_functions()
{
    lua_newtable(L);

    lua_pushcfunction(L,lua_broadcast_message);
    lua_setfield(L,-2,"broadcast");

    lua_setglobal(L,"Chat");
}


bool LuaManager::execute_command(const std::string& nickname, const std::string& raw_message)
{
    std::vector<std::string>tokens=split_message(raw_message);

    if (tokens.empty()||tokens[0].empty()||tokens[0][0]!='/')
    {
        return false;
    }

    std::string command_name=tokens[0];

    std::vector<std::string>args;
    if (tokens.size()>1)
    {
        args.insert(args.end(),tokens.begin()+1,tokens.end());
    }

    return execute_command(nickname,command_name,args);
}


bool LuaManager::execute_command(const std::string& nickname, const std::string& command, const std::vector<std::string>& args)
{
    std::lock_guard<std::mutex>lock(mtx);

    std::string lua_func_name="lua_cmd_"+command.substr(1);
    LOG_INFO("尝试调用 Lua 函数: " + lua_func_name);

    lua_getglobal(L,lua_func_name.c_str());

   if (!lua_isfunction(L,-1))
   {
       LOG_ERROR("Lua function not found: " + lua_func_name);

       lua_pop(L,1);

       lua_getglobal(L,"_G");

       lua_pushnil(L);
        while (lua_next(L,-2)!=0)
        {
            if (lua_type(L,-2)==LUA_TSTRING)
            {
                const char* key=lua_tostring(L,-2);
                if (key&&std::string(key).find("lua_cmd")!=std::string::npos)
                {
                    LOG_INFO("Lua 全局存在函数: " + std::string(key));
                }
            }
            lua_pop(L,1);
        }
        lua_pop(L,1);
       return false;
   }

    lua_pushstring(L,nickname.c_str());

    lua_newtable(L);
    for (size_t i=0;i<args.size();++i)
    {
        lua_pushnumber(L,i+1);
        lua_pushstring(L,args[i].c_str());
        lua_settable(L,-3);
    }

    if (lua_pcall(L,2,0,0)!=LUA_OK)
    {
        const char* error=lua_tostring(L,-1);
        LOG_ERROR("执行 Lua 命令 "+command+ " 失败: " + std::string(error));
        lua_pop(L,1);
        return true;
    }

    return true;
}
