Chat=Chat or {}

print("[Lua] commands.lua 已加载")

_G.lua_cmd_hello=function (nickname,args)
    
    local msg = "你好，" .. nickname .. "！你正在使用 Lua 自定义命令。"

    if Chat and Chat.broadcast then
    Chat.broadcast("Server", "你发送的参数数量: "..#args)
    end

end

_G.lua_cmd_roll= function (nickname,args)
    local max=tonumber(args[1]) or 100
    local result =math.random(1,max)
    local msg=nickname .. " 掷出了 "..result.. " 点 (最大值: " .. max .. ")"

     if Chat and Chat.broadcast then 
    Chat.broadcast("Server",msg)
    end
end

