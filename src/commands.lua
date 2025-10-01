Chat=Chat or {}

print("[Lua] commands.lua 已加载")

local function require_admin(nickname,is_admin)
    if not is_admin then
         Chat.broadcast("Server", "错误：" .. nickname .. "，该命令需要管理员权限。")
        error("权限不足，终止命令执行", 0)
        return false
    end
    return true
end

_G.lua_cmd_hello=function (nickname,args)
    local msg= "你好，" .. nickname .. "！你正在使用 Lua 自定义命令。"

    if Chat and Chat.broadcast then
        Chat.broadcast("Server",msg)
        Chat.broadcast("Server","你发送的参数数量"..#args)
    end
    return true
end

_G.lua_cmd_roll=function (nickname,args)
    local max=tonumber(args[1]) or 100
    local result=math.random(1,max)
    local msg=nickname.. " 掷出了 " .. result .." 点 (最大值: " .. max .. ")"

    if Chat and Chat.broadcast then
        Chat.broadcast("Server",msg)
    end

    return true
end

_G.lua_cmd_kick=function (admin_nickname,is_admin,args)
    if not require_admin(admin_nickname,is_admin) then
        return false
    end

    if #args<1 then
        Chat.broadcast("Server","用法: /kick <目标昵称>")
        return false
    end

    local target_nickname=args[1]

    if target_nickname==admin_nickname then
        Chat.broadcast("Server", "管理员不能使用该命令踢出自己。")
        return false
    end

    local success=Chat.kick_user(target_nickname,admin_nickname)

    if success then
        return true
    else
        Chat.broadcast("Server", "踢出失败: 未找到用户 [" .. target_nickname .. "]。")
        return false
    end
end