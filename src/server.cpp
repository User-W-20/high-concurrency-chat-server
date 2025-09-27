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

#include "../include/Logger.h"
#include "../include/ServerContext.h"
#include "../include/client.h"
#include "../include/group_manager.h"
#include "../include/threadpool.h"

constexpr int MAX_EVENTS = 1024;
constexpr int PORT = 5008;
constexpr int BUF_SIZE = 1024;
constexpr int HEARTBEAT_TIMEOUT = 60;  // 心跳超时
constexpr int EPOLL_TIMEOUT_MS = 1000;
const std::string ADMIN_IP = "127.0.0.1";

std::atomic<bool> running = true;

using ServerCommandHandler =
    std::function<std::string(const std::vector<std::string> &, int)>;

void safe_print(const std::string &msg)
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

void send_message_with_length(int fd, const std::string &message)
{
    uint32_t net_len = htonl(message.size());

    std::string full_msg(reinterpret_cast<const char *>(&net_len),
                         sizeof(net_len));
    full_msg += message;

    const char *data = full_msg.data();
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

void disconnect_client(int fd, ServerContext &ctx)
{
    std::string name = ctx.get_username(fd);
    std::string quit_msg = name + " 退出聊天室";
    safe_print(quit_msg + "\n");

    ctx.group_manager->remove_client_from_groups(name);

    {
        std::lock_guard<std::mutex> lock(ctx.to_remove_mtx);
        ctx.to_remove.push_back(fd);
    }
}

std::vector<std::string> split(const std::string &s)
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
    int fd, const std::string &msg, ServerContext &ctx,
    const std::unordered_map<std::string, ServerCommandHandler> &admin_commands,
    const std::unordered_map<std::string, ServerCommandHandler> &user_commands)
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
        if (trimmed_msg == ctx.admin_token)
        {
            bool is_admin_ip = false;
            {
                std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
                auto it = ctx.clients.find(fd);
                if (it != ctx.clients.end() && it->second.ip == ADMIN_IP)
                {
                    it->second.is_admin = true;
                    is_admin_ip = true;
                }
            }

            if (is_admin_ip)
            {
                ctx.set_username(fd, ctx.admin_nickname);
                ctx.admin_token.clear();
                std::string reply = "恭喜，您已成为管理员！！您的昵称是：" +
                                    ctx.admin_nickname + "\n";
                send_message_with_length(fd, reply);
                safe_print("客户端 " + std::to_string(fd) +
                           " 已验证为管理员，并设置昵称: " +
                           ctx.admin_nickname + "\n");
            }
            else
            {
                std::string reply = "仅限本地管理员登录。\n";
                send_message_with_length(fd, reply);
            }
        }
        else
        {
            bool name_token = false;
            {
                std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
                for (const auto &pair : ctx.clients)
                {
                    if (pair.first < 0) continue;
                    if (pair.second.nickname == trimmed_msg)
                    {
                        name_token = true;
                        break;
                    }
                }
            }

            if (name_token)
            {
                std::string reply =
                    "Error: 昵称 '" + trimmed_msg + "' 已被占用。\n";
                send_message_with_length(fd, reply);
            }
            else
            {
                ctx.set_username(fd, trimmed_msg);
                std::string join_msg = trimmed_msg + " 加入聊天室";
                safe_print(join_msg + "\n");
                ctx.broadcast(join_msg, fd);
            }
        }
        return;
    }

    if (trimmed_msg[0] == '/')
    {
        std::vector<std::string> args = split(trimmed_msg);
        std::string command = args[0];
        std::string reply_to_client;
        bool command_executed=false;

        if (is_admin)
        {
            auto admin_cmd_iter = admin_commands.find(command);
            if (admin_cmd_iter != admin_commands.end())
            {
                reply_to_client = admin_cmd_iter->second(args, fd);
                safe_print("客户端[" + nickname + "] 执行" + command + "\n");
                command_executed=true;
            }
        }

        if (!command_executed)
        {
            auto user_cmd_iter = user_commands.find(command);
            if (user_cmd_iter != user_commands.end())
            {
                reply_to_client = user_cmd_iter->second(args, fd);
                command_executed=true;
            }
        }

        if (command_executed)
        {
            if (!reply_to_client.empty())
            {
                send_message_with_length(fd,reply_to_client);
            }
        }else
        {
            reply_to_client="未知命令或权限不足。";
            send_message_with_length(fd,reply_to_client);
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

    auto sender = [](int fd, const std::string &message)
    { send_message_with_length(fd, message); };

    ServerContext ctx(pool, sender);

    std::unordered_map<std::string, ServerCommandHandler> admin_commands;
    std::unordered_map<std::string, ServerCommandHandler> user_commands;

    // 非群组命令
    user_commands["/list"] =
        [&ctx](const std::vector<std::string> &args, int fd)
    {
        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
        std::string list_str = "在线用户：\n";
        for (const auto &pair : ctx.clients)
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
        [&ctx](const std::vector<std::string> &args, int fd)
    { return "你的昵称是：" + ctx.get_username(fd) + "\n"; };

    user_commands["/w"] = [&ctx](const std::vector<std::string> &args,
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

        std::string target_nickname = args[1];
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

        int target_fd = -1;
        {
            std::lock_guard<std::mutex> lock(ctx.clients_mtx);
            for (const auto &pair : ctx.clients)
            {
                if (pair.second.nickname == target_nickname)
                {
                    target_fd = pair.first;
                    break;
                }
            }
        }

        if (target_fd != -1)
        {
            std::string whisper_reply_to_target =
                "来自 " + sender_nickname + " 的私聊：" + whisper_message;
            send_message_with_length(target_fd, whisper_reply_to_target);
            return "已向 " + target_nickname + " 发送私聊消息。\n";
        }
        else
        {
            return "用户 '" + target_nickname + "' 不在线或不存在。\n";
        }
    };

    user_commands["/help"] =
        [&ctx](const std::vector<std::string> &args, int fd)
    {
        std::string help_msg =
            "可用的命令：\n"
            "/list - 列出所有在线用户\n"
            "/w <昵称> <消息> - 向指定用户发送私聊消息\n"
            "/whoami - 查看你的昵称\n"
            "/quit - 退出聊天室\n"
            "/help - 显示此帮助信息\n";

        bool is_admin = false;
        {
            std::lock_guard<std::mutex> lock(ctx.clients_mtx);
            if (ctx.clients.count(fd))
            {
                is_admin = ctx.clients.at(fd).is_admin;
            }
        }
        if (is_admin)
        {
            help_msg +=
                "/kick <昵称> - 踢出指定用户\n"
                "/create <群名> - 创建一个新群\n"
                "/join <群名> - 加入一个群\n"
                "/send <群名> <消息> - 向特定群发送消息\n"
                "/listgroups - 列出所有群\n";
        }
        return help_msg;
    };

    user_commands["/quit"] =
        [&ctx](const std::vector<std::string> &args, int fd)
    {
        std::string reply = "正在安全退出服务器，再见！\n";
        send_message_with_length(fd, reply);
        disconnect_client(fd, ctx);
        return "";
    };

    user_commands["/kick"] = [&ctx](const std::vector<std::string> &args,
                                    int fd) -> std::string
    {
        if (args.size() < 2)
        {
            return "用法: /kick <昵称>。\n";
        }

        std::string target_nickname = args[1];
        if (target_nickname.empty())
        {
            return "请指定要踢出的用户昵称。\n";
        }

        int target_fd = -1;
        {
            std::lock_guard<std::mutex> lock(ctx.clients_mtx);
            for (const auto &pair : ctx.clients)
            {
                if (pair.second.nickname == target_nickname)
                {
                    target_fd = pair.first;
                    break;
                }
            }
        }

        if (target_fd != -1)
        {
            std::string admin_name = ctx.get_username(fd);
            std::stringstream ss;
            ss << admin_name << " 将 " << target_nickname << " 踢出聊天室。\n";
            ctx.broadcast(ss.str(), fd);
            std::string reply_to_kick = "您已被管理员踢出聊天室。\n";
            send_message_with_length(target_fd, reply_to_kick);
            disconnect_client(target_fd, ctx);
            return "用户 " + target_nickname + " 已被踢出。\n";
        }
        else
        {
            return "用户 '" + target_nickname + "' 不在线。\n";
        }
    };

    // 群组命令
    user_commands["/create"] = [&ctx](const std::vector<std::string> &args,
                                      int fd) -> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_create_group(username, args);
    };

    user_commands["/join"] = [&ctx](const std::vector<std::string> &args,
                                    int fd) -> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_join_group(username, args);
    };

    user_commands["/send"] = [&ctx](const std::vector<std::string> &args,
                                    int fd) -> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_send_message(username, args);
    };

    user_commands["/listgroups"] = [&ctx](const std::vector<std::string> &args,
                                          int fd) -> std::string
    {
        std::string username = ctx.get_username(fd);
        if (username.empty())
        {
            return "请先设置昵称。\n";
        }
        return ctx.group_manager->handle_list_groups();
    };

    epoll_event events[MAX_EVENTS];

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) == -1)
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
    const std::string ADMIN_TOKEN = "admin123";
    const std::string ADMIN_NICKNAME = "admin";
    ctx.admin_nickname = ADMIN_NICKNAME;
    ctx.admin_token = ADMIN_TOKEN;

    Logger::getInstance().log("服务器启动，等待客户端连接...\n");
    safe_print("管理员口令已生成: " + ctx.admin_token + "\n");

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
                for (const auto &pair : ctx.clients)
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
                int client_fd = accept(server_fd, (sockaddr *)&client_addr,
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
                std::string client_ip(ip_str);
                {
                    std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                    ctx.clients.emplace(client_fd,
                                        Client(client_fd, client_ip));
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

    // 关闭所有客户端
    safe_print("正在关闭所有客户端连接...\n");
    {
        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
        for (const auto &pair : ctx.clients)
        {
            close(pair.first);
        }
    }
    close(server_fd);
    close(epoll_fd);
    safe_print("服务器已安全退出。\n");
    return 0;
}
