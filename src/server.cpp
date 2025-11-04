#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <stdexcept>
#include "../include/Logger.h"
#include "../include/ServerContext.h"
#include "../include/client.h"
#include "../include/group_manager.h"
#include "../include/threadpool.h"
#include "../include/LuaManager.h"
#include "../include/UserManager.h"
#include "../include/config.h"
#include "../include/DatabaseManager.h"

constexpr int MAX_EVENTS = 1024;
constexpr int PORT = 5008;
constexpr int BUF_SIZE = 1024;
constexpr int HEARTBEAT_TIMEOUT = 300; // 心跳超时
constexpr int EPOLL_TIMEOUT_MS = 1000;

std::atomic<bool> running = true;

using ServerCommandHandler =
std::function<std::string(const std::vector<std::string>&, int)>;

void safe_print(const std::string& msg)
{
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    std::cout << msg << std::flush;
}

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void send_message_with_length(int fd, const std::string& message)
{
    uint32_t net_len = htonl(message.size());

    std::string full_msg(reinterpret_cast<const char*>(&net_len),
                         sizeof(net_len));
    full_msg += message;

    const char* data = full_msg.data();
    size_t total_len = full_msg.size();
    size_t bytes_sent = 0;

    while (bytes_sent < total_len)
    {
        ssize_t s = send(fd, data + bytes_sent, total_len - bytes_sent, 0);
        if (s == -1)
        {
            perror("send failed");
            return;
        }
        if (s == 0)
        {
            safe_print("WARNING: 客户端 fd=" + std::to_string(fd) +
                       " 断开连接。\n");
            return;
        }
        bytes_sent += s;
    }
}

void disconnect_client(int fd, ServerContext& ctx)
{
    std::string name = ctx.get_username(fd);
    std::string quit_msg = name + " 退出聊天室";

    if (!name.empty())
    {
        safe_print(quit_msg + "\n");
        ctx.broadcast(quit_msg, fd);
    }

    ctx.group_manager->remove_client_from_groups(name);

    {
        std::lock_guard<std::mutex> lock(ctx.to_remove_mtx);
        ctx.to_remove.push_back(fd);
    }
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token)
    {
        tokens.push_back(token);
    }
    return tokens;
}

void sigint_hadler(int) { running = false; }

void handle_message(
    int fd, const std::string& msg, ServerContext& ctx,
    const std::unordered_map<std::string, ServerCommandHandler>& admin_commands,
    const std::unordered_map<std::string, ServerCommandHandler>& user_commands)
{
    std::string nickname = ctx.get_username(fd);
    bool is_admin = false;
    {
        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
        if (ctx.clients.count(fd))
        {
            is_admin = ctx.clients.at(fd).is_admin;
        }
    }

    std::string trimmed_msg = msg;
    trimmed_msg.erase(0, trimmed_msg.find_first_not_of(" \t\n\r\f\v"));
    trimmed_msg.erase(trimmed_msg.find_last_not_of(" \t\n\r\f\v") + 1);

    safe_print("handle_message: fd=" + std::to_string(fd) + ", nickname=" +
               nickname + ", is_admin=" + std::to_string(is_admin) +
               ", msg=" + trimmed_msg + "\n");

    if (nickname.empty())
    {
        std::vector<std::string> args = split(trimmed_msg);

        if (args.size() < 3)
        {
            send_message_with_length(
                fd, "请先使用 /register <用户名> <密码> 或 /login <用户名> <密码>。");
            return;
        }

        std::string command = args[0];

        std::transform(command.begin(), command.end(), command.begin(),
                       ::tolower);

        const std::string& user_raw = args[1];
        const std::string& pass = args[2];

        std::string user_lower = ctx.user_manager->to_lower_nickname(user_raw);

        std::string db_username_raw;
        std::string db_argon2_hash;
        bool db_is_admin = false;
        if (command == "/register")
        {
            User existing_user;

            if (ctx.db_manager.get_user_data(user_lower, db_username_raw,
                                             db_argon2_hash, db_is_admin))
            {
                send_message_with_length(fd, "注册失败: 用户名已被占用。");
                return;
            }

            std::string encoded_hash;
            if (!UserManager::hash_password(pass, encoded_hash))
            {
                send_message_with_length(fd, "注册失败: 密码处理失败。");
                return;
            }

            if (ctx.db_manager.register_user(user_raw, user_lower,
                                             encoded_hash))
            {
                send_message_with_length(fd, "注册成功! 请使用 /login 登录。");
            }
            else
            {
                send_message_with_length(fd, "注册失败: 数据库写入错误。");
            }
        }
        else if (command == "/login")
        {
            if (!ctx.db_manager.get_user_data(user_lower, db_username_raw,
                                              db_argon2_hash, db_is_admin))
            {
                send_message_with_length(fd, "登录失败: 用户名或密码错误。");
                return;
            }

            if (UserManager::verify_password(pass, db_argon2_hash))
            {
                if (ctx.get_fd_by_nickname(db_username_raw) != -1)
                {
                    send_message_with_length(fd, "错误: 该用户已在别处登录。");
                    return;
                }

                ctx.user_manager->add_user_to_memory(
                    db_username_raw,
                    db_argon2_hash,
                    db_is_admin);

                ctx.set_username(fd, db_username_raw);

                {
                    std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                    if (ctx.clients.count(fd))
                    {
                        ctx.clients.at(fd).is_admin = db_is_admin;
                    }
                }

                std::string welcome_msg = "登录成功! 欢迎回来, " + db_username_raw;

                if (db_is_admin)
                {
                    welcome_msg += " (管理员)";
                }

                send_message_with_length(fd, welcome_msg);
                ctx.broadcast(db_username_raw + " 加入聊天室", fd);
            }
            else
            {
                send_message_with_length(fd, "登录失败: 用户名或密码错误。");
            }
        }
        else
        {
            send_message_with_length(fd, "未知命令。请先使用 /register 或 /login。");
            return;
        }
        return;
    }
    if (trimmed_msg[0] == '/')
    {
        std::vector<std::string> args = split(trimmed_msg);
        std::string command = args[0];

        while (command.length() > 1 && command[0] == '/' && command[1] == '/')
        {
            command.erase(0, 1);
        }
        args[0] = command;

        std::string reply_to_client;
        bool command_executed = false;

        auto user_cmd_iter = user_commands.find(command);
        if (user_cmd_iter != user_commands.end())
        {
            reply_to_client = user_cmd_iter->second(args, fd);
            command_executed = true;
        }

        else
        {
            auto admin_cmd_iter = admin_commands.find(command);
            if (admin_cmd_iter != admin_commands.end())
            {
                if (is_admin)
                {
                    reply_to_client = admin_cmd_iter->second(args, fd);
                    command_executed = true;
                }
                else
                {
                    reply_to_client = "错误：'" + command + "' 命令需要管理员权限。";
                    command_executed = true;
                }
            }
        }
        safe_print("DEBUG: Client FD " + std::to_string(fd) +
                   " (Nickname: " + nickname +
                   ") C++ state is_admin=" + (is_admin ? "TRUE" : "FALSE") +
                   "\n");

        if (!command_executed)
        {
            safe_print(
                "DEBUG: Command '" + command +
                "' NOT found in C++ maps. Attempting Lua.\n");

            if (LuaManager::getInstance().execute_command(
                nickname, is_admin, trimmed_msg))
            {
                safe_print(
                    "客户端[" + nickname + "] 执行 Lua 命令: " + command + "\n");
                command_executed = true;
            }
        }

        if (command_executed)
        {
            if (!reply_to_client.empty())
            {
                send_message_with_length(fd, reply_to_client);
            }
        }
        else
        {
            reply_to_client = "未知命令。";
            send_message_with_length(fd, reply_to_client);
        }
    }
    else
    {
        std::stringstream ss;
        ss << nickname << ": " << trimmed_msg;
        std::string out = ss.str();
        safe_print(out + "\n");
        ctx.broadcast(out, fd);
    }
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sigint_hadler);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("socket");
        return -1;
    }

    ThreadPool pool(4);

    auto sender = [](int fd, const std::string& message)
    {
        send_message_with_length(fd, message);
    };

    std::map<std::string, std::string> env_config = load_env();

    if (env_config.empty())
    {
        LOG_FATAL("无法加载 .env 配置文件。服务器退出。");
        return 1;
    }

    DatabaseManager& db_manager = DatabaseManager::getInstance();
    if (!db_manager.connect(env_config))
    {
        LOG_FATAL("数据库连接失败。服务器退出。");
        return 1;
    }
    LOG_INFO("数据库连接成功。");

    ServerContext ctx(pool, sender, db_manager);
    if (env_config.empty())
    {
        LOG_FATAL("无法加载 .env 配置文件。服务器退出。");
        return 1;
    }

    const std::string GROUP_FILE = "groups_data.json";
    try
    {
        ctx.group_manager->load_groups_from_file(GROUP_FILE);
        LOG_INFO("成功加载群组数据。");
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("加载群组数据失败: " << e.what() << "。将从空状态启动。");
    }

    try
    {
        LuaManager& lua_manager = LuaManager::initializeInstance(ctx);

        if (!lua_manager.initialize())
        {
            LOG_FATAL("LuaManager 初始化失败 (加载 commands.lua 失败)，服务器退出。");
            return 1;
        }
        LOG_INFO("Lua 命令系统加载成功。");
    }
    catch (const std::exception& e)
    {
        LOG_FATAL("LuaManager 初始化时发生致命错误: "+std::string(e.what()));
        return 1;
    }

    std::unordered_map<std::string, ServerCommandHandler> admin_commands;
    std::unordered_map<std::string, ServerCommandHandler> user_commands;

    // 非群组命令
    user_commands["/list"] =
        [&ctx](const std::vector<std::string>& args, int fd)
        {
            std::lock_guard<std::mutex> lock(ctx.clients_mtx);
            std::string list_str = "在线用户：\n";
            for (const auto& pair : ctx.clients)
            {
                if (!pair.second.nickname.empty())
                {
                    list_str += "fd=" + std::to_string(pair.first) +
                        " nickname=" + pair.second.nickname + "\n";
                }
            }
            return list_str;
        };

    user_commands["whoami"] =
        [&ctx](const std::vector<std::string>& args, int fd)
        {
            return "你的昵称是：" + ctx.get_username(fd) + "\n";
        };

    user_commands["/w"] = [&ctx](const std::vector<std::string>& args,
                                 int fd) -> std::string
    {
        if (args.size() < 3)
        {
            return "用法: /w <昵称> <消息>。\n";
        }

        std::string sender_nickname = ctx.get_username(fd);
        if (sender_nickname.empty())
        {
            return "无法获取您的昵称。\n";
        }

        const std::string& target_nickname = args[1];
        if (target_nickname == sender_nickname)
        {
            return "不能和自己私聊。\n";
        }

        std::string whisper_message;
        for (size_t i = 2; i < args.size(); ++i)
        {
            whisper_message += args[i] + " ";
        }

        if (!whisper_message.empty())
        {
            whisper_message.pop_back();
        }

        int target_fd = ctx.get_fd_by_nickname(target_nickname);

        if (target_fd != -1)
        {
            std::string whisper_reply_to_target =
                "来自 " + sender_nickname + " 的私聊：" + whisper_message;

            send_message_with_length(target_fd, whisper_reply_to_target + "\n");
            return "已向 " + target_nickname + " 发送私聊消息。\n";
        }
        else
        {
            return "用户 '" + target_nickname + "' 不在线或不存在。\n";
        }
    };

    user_commands["/help"] =
        [&ctx](const std::vector<std::string>& args, int fd)
        {
            std::string help_msg =
                "--- 认证命令 ---\n"
                "/register <用户> <密码> - 注册新用户\n"
                "/login <用户> <密码> - 登录\n"
                "--- 可用的命令 ---\n"
                "/list - 列出所有在线用户\n"
                "/w <昵称> <消息> - 向指定用户发送私聊消息\n"
                "/whoami - 查看你的昵称\n"
                "/help - 显示此帮助信息\n"
                "/create <群名> - 创建一个新群（您将成为群主）\n"
                "/join <群名> - 加入一个群\n"
                "/send <群名> <消息> - 向特定群发送消息\n"
                "/listgroups - 列出所有群\n"
                "/hello - Lua 脚本示例命令\n"
                "/roll [max] - 掷骰子（Lua 脚本）\n"
                "/quit - 退出聊天室\n"
                "/leave <群名> - 退出群聊\n";

            bool is_server_admin = false;
            {
                std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                if (ctx.clients.count(fd))
                {
                    is_server_admin = ctx.clients.at(fd).is_admin;
                }
            }

            if (is_server_admin)
            {
                help_msg +=
                    "\n--- 服务器管理员命令（全局）---\n"
                    "/kick <昵称> - 踢出指定用户（全局）\n";
            }

            help_msg +=
                "\n--- 群组管理命令（需群主身份）---\n"
                "/groupkick <群名> <昵称> - 将群成员踢出群组\n"
                "/groupunban <群名> <昵称> - 解除群组对某成员的加入限制\n"
                "/transfer <群名> <昵称> - 将群主身份转让给指定成员\n"
                "-----------------------\n";
            return help_msg;
        };

    user_commands["/quit"] =
        [&ctx](const std::vector<std::string>& args, int fd)
        {
            std::string reply = "正在安全退出服务器，再见！\n";
            send_message_with_length(fd, reply);
            disconnect_client(fd, ctx);
            return "";
        };

    user_commands["/create"] = [&ctx](const std::vector<std::string>& args,
                                      int fd)-> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_create_group(username, args);
    };

    user_commands["/join"] = [&ctx](const std::vector<std::string>& args,
                                    int fd) -> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_join_group(username, args);
    };

    user_commands["/send"] = [&ctx](const std::vector<std::string>& args,
                                    int fd) -> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_send_message(username, args);
    };

    user_commands["/listgroups"] = [&ctx](const std::vector<std::string>& args,
                                          int fd) -> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_list_groups();
    };

    user_commands["/groupkick"] = [&ctx](const std::vector<std::string>& args,
                                         int fd)-> std::string
    {
        std::string username = ctx.get_username(fd);

        if (username.empty())
        {
            return "请先设置昵称。\n";
        }

        return ctx.group_manager->handle_group_kick(username, args);
    };

    user_commands["/leave"] = [&ctx](const std::vector<std::string>& args,
                                     int fd)-> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }

        return ctx.group_manager->handle_group_leave(username, args);
    };

    user_commands["/transfer"] = [&ctx](const std::vector<std::string>& args,
                                        int fd)-> std::string
    {
        std::string kicker_nickname = ctx.get_username(fd);

        if (kicker_nickname.empty())
        {
            return "请先设置昵称。\n";
        }

        if (args.size() < 3)
        {
            return "用法: /transfer <群名> <昵称>\n";
        }

        return ctx.group_manager->handle_group_transfer(kicker_nickname, args);
    };

    user_commands["/groupunban"] = [&ctx](const std::vector<std::string>& args,
                                          int fd)-> std::string
    {
        std::string username = ctx.get_username(fd);

        if (username.empty())
        {
            return "请先设置昵称。\n";
        }

        if (args.size() < 3)
        {
            return "用法: /groupunban <群名> <昵称>。\n";
        }

        return ctx.group_manager->handle_group_unban(username, args);
    };

    admin_commands["/kick"] = [&ctx](const std::vector<std::string>& args,
                                     int fd)-> std::string
    {
        if (args.size() < 2)
        {
            return "用法: /kick <昵称>。\n";
        }

        const std::string& target_nickname_raw = args[1];

        std::string admin_name = ctx.get_username(fd);

        int target_fd = ctx.get_fd_by_nickname(target_nickname_raw);

        if (target_fd != -1)
        {
            std::stringstream ss;
            ss << admin_name << " 将 " << target_nickname_raw << " 踢出聊天室。\n";

            ctx.broadcast(ss.str(), fd);

            std::string reply_to_kick = "您已被管理员踢出聊天室。\n";
            send_message_with_length(target_fd, reply_to_kick);

            disconnect_client(target_fd, ctx);

            return "用户 " + target_nickname_raw + " 已被踢出。\n";
        }
        else
        {
            return "用户 '" + target_nickname_raw + "' 不在线。\n";
        }
    };

    epoll_event events[MAX_EVENTS];

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) == -1)
    {
        perror("bind");
        return -1;
    }

    if (listen(server_fd, SOMAXCONN) == -1)
    {
        perror("listen");
        return -1;
    }

    set_nonblocking(server_fd);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        return -1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1)
    {
        perror("epoll_ctl add server_fd");
        close(server_fd);
        return -1;
    }

    ctx.epoll_fd = epoll_fd;

    LOG_INFO("服务器启动，等待客户端连接...\n");

    while (running)
    {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);

        if (ctx.shutdown_requested)
        {
            safe_print("收到安全退出请求，服务器即将关闭...\n");
            break;
        }

        if (nfds == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        // 心跳检测
        if (nfds == 0)
        {
            std::vector<int> inactive_fds;
            {
                std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                auto now = std::chrono::steady_clock::now();
                for (const auto& pair : ctx.clients)
                {
                    if (std::chrono::duration_cast<std::chrono::seconds>(
                            now - pair.second.last_activity)
                        .count() > HEARTBEAT_TIMEOUT)
                    {
                        inactive_fds.push_back(pair.first);
                    }
                }
            }

            for (int fd_to_disconnect : inactive_fds)
            {
                disconnect_client(fd_to_disconnect, ctx);
                safe_print("客户端 " + std::to_string(fd_to_disconnect) +
                           " 因超时自动断开连接。\n");
            }
        }

        {
            std::lock_guard<std::mutex> rm_lock(ctx.to_remove_mtx);
            if (!ctx.to_remove.empty())
            {
                std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
                for (int cfd : ctx.to_remove)
                {
                    epoll_ctl(ctx.epoll_fd, EPOLL_CTL_DEL, cfd, nullptr);
                    if (ctx.clients.count(cfd))
                    {
                        ctx.clients.erase(cfd);
                    }
                    safe_print("[CLEAN] 客户端[" + std::to_string(cfd) +
                               "] 已被清理\n");
                    close(cfd);
                }
                ctx.to_remove.clear();
            }
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            if (fd == server_fd)
            {
                sockaddr_in client_addr{};
                socklen_t client_addr_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (sockaddr*)&client_addr,
                                       &client_addr_len);

                if (client_fd == -1)
                {
                    if (errno != EAGAIN || errno != EWOULDBLOCK)
                    {
                        perror("accept");
                    }
                    continue;
                }

                set_nonblocking(client_fd);
                epoll_event ev_client{};
                ev_client.events = EPOLLIN | EPOLLET;
                ev_client.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_client);
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip_str,
                          INET_ADDRSTRLEN);

                {
                    std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                    ctx.clients.emplace(client_fd,
                                        Client(client_fd, std::string(ip_str)));
                }
            }
            else if (events[i].events & EPOLLIN)
            {
                bool disconnect = false;
                while (true)
                {
                    uint32_t net_len;
                    ssize_t n_len =
                        recv(fd, &net_len, sizeof(net_len), MSG_PEEK);

                    if (n_len <= 0)
                    {
                        if (n_len == 0 ||
                            (errno != EAGAIN && errno != EWOULDBLOCK))
                        {
                            disconnect = true;
                        }
                        break;
                    }

                    if (n_len < sizeof(net_len))
                    {
                        break;
                    }

                    uint32_t msg_len = ntohl(net_len);

                    std::vector<char> buf(msg_len + sizeof(net_len));

                    ssize_t total_n = recv(fd, buf.data(), buf.size(), 0);

                    if (total_n <= 0)
                    {
                        if (total_n == 0 ||
                            (errno != EAGAIN && errno != EWOULDBLOCK))
                        {
                            disconnect = true;
                        }
                        break;
                    }

                    if (total_n < msg_len + sizeof(net_len))
                    {
                        break;
                    }

                    {
                        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                        if (ctx.clients.count(fd))
                        {
                            ctx.clients.at(fd).last_activity =
                                std::chrono::steady_clock::now();
                        }
                    }

                    std::string msg(buf.begin() + sizeof(net_len), buf.end());
                    handle_message(fd, msg, ctx, admin_commands, user_commands);
                }
                if (disconnect)
                {
                    disconnect_client(fd, ctx);
                }
            }
        }
    }

    LOG_INFO("服务器关闭流程：保存数据...");

    db_manager.disconnect();
    LOG_INFO("数据库已断开连接。");

    try
    {
        ctx.group_manager->save_groups_to_file(JSON_FILE);
        LOG_INFO("群组数据保存完成。");
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("致命错误：保存群组数据失败！数据可能丢失。"<<e.what());
    }

    // 关闭所有客户端
    safe_print("正在关闭所有客户端连接...\n");
    {
        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
        for (const auto& pair : ctx.clients)
        {
            close(pair.first);
        }
    }

    close(server_fd);
    close(epoll_fd);
    safe_print("服务器已安全退出。\n");
    return 0;
}