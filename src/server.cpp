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
#include "../include/client.h"
#include "../include/threadpool.h"

constexpr int MAX_EVENTS = 1024;
constexpr int PORT = 5008;
constexpr int BUF_SIZE = 1024;
constexpr int HEARTBEAT_TIMEOUT = 60;  // 心跳超时
constexpr int EPOLL_TIMEOUT_MS = 1000;
const std::string ADMIN_IP = "127.0.0.1";

bool running = true;

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

struct ServerContext;

void disconnect_client(int fd, ServerContext &ctx);

void broadcast(const std::string &msg, int sender_fd, ServerContext &ctx);

using CommandHandler =
    std::function<void(int, const std::string &, ServerContext &)>;

struct ServerContext
{
    std::unordered_map<int, Client> clients{};
    std::mutex clients_mtx{};
    std::vector<int> to_remove{};
    std::mutex to_remove_mtx{};
    int epoll_fd{};
    ThreadPool &pool;
    std::unordered_set<std::string> active_names{};
    std::mutex names_mtx{};
    std::string admin_token{};
    std::string admin_nickname{};
    std::mutex token_mtx{};

    explicit ServerContext(ThreadPool &p) : pool(p) {}
};

void send_message_with_length(int fd, const std::string &message)
{
    uint32_t net_len = htonl(message.size());

    send(fd, &net_len, sizeof(net_len), 0);

    send(fd, message.data(), message.size(), 0);
}

void handle_kick(int fd, const std::string &trimmed_msg, ServerContext &ctx)
{
    std::string target_nickname;
    std::istringstream iss(trimmed_msg);
    std::string command;
    iss >> command >> target_nickname;

    if (target_nickname.empty())
    {
        std::string reply = "请指定要踢出的用户昵称。\n";
        send(fd, reply.c_str(), reply.size(), 0);
        return;
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
        std::string admin_name;
        {
            std::lock_guard<std::mutex> lock(ctx.clients_mtx);
            admin_name = ctx.clients.at(fd).nickname;
        }
        std::stringstream ss;
        ss << admin_name << " 将 " << target_nickname << " 踢出聊天室。\n";
        safe_print(ss.str());
        disconnect_client(target_fd, ctx);
        std::string reply = "您已被管理员踢出聊天室。\n";
        uint32_t reply_len = htonl(reply.size());
        std::string full_reply(reinterpret_cast<const char *>(&reply_len),
                               sizeof(reply_len));
        full_reply += reply;
        send(target_fd, full_reply.data(), full_reply.size(), 0);
    }
    else
    {
        std::string reply = "用户 '" + target_nickname + "' 不在线。\n";
        send(fd, reply.c_str(), reply.size(), 0);
    }
}

void handle_list(int fd, const std::string &, ServerContext &ctx)
{
    std::string list_str = "在线用户：";

    {
        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
        for (const auto &pair : ctx.clients)
        {
            list_str += pair.second.nickname + " ";
        }
    }

    uint32_t list_len = htonl(list_str.size());
    std::string full_list(reinterpret_cast<const char *>(&list_len),
                          sizeof(list_len));
    full_list += list_str;
    send(fd, full_list.data(), full_list.size(), 0);
}

void broadcast(const std::string &msg, int sender_fd, ServerContext &ctx)
{
    ctx.pool.enqueue(
        [msg, sender_fd, &ctx]()
        {
            std::vector<int> current_clients;

            {
                std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                for (const auto &pair : ctx.clients)
                {
                    current_clients.push_back(pair.first);
                }
            }

            uint32_t msg_len = htonl(msg.size());
            std::string full_msg(reinterpret_cast<const char *>(&msg_len),
                                 sizeof(msg_len));
            full_msg += msg;

            for (int cfd : current_clients)
            {
                if (cfd != sender_fd)
                {
                    send(cfd, full_msg.data(), full_msg.size(), 0);
                }
            }
        });
}

void disconnect_client(int fd, ServerContext &ctx)
{
    std::string name;
    {
        std::lock_guard<std::mutex> nlock(ctx.clients_mtx);
        name =
            (ctx.clients.count(fd) ? ctx.clients.at(fd).nickname : "未知用户");
    }

    std::string quit_msg = name + " 退出聊天室";
    safe_print(quit_msg + "\n");
    broadcast(quit_msg, fd, ctx);

    {
        std::lock_guard<std::mutex> lock(ctx.to_remove_mtx);
        ctx.to_remove.push_back(fd);
    }
}

void sigint_hadler(int) { running = false; }

void handle_message(
    int fd, const std::string &msg, ServerContext &ctx,
    const std::unordered_map<std::string, CommandHandler> &admin_commands,
    const std::unordered_map<std::string, CommandHandler> &user_commands)
{
    std::string nickname;
    bool is_admin = false;
    {
        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
        if (ctx.clients.count(fd))
        {
            nickname = ctx.clients.at(fd).nickname;
            is_admin = ctx.clients.at(fd).is_admin;
        }
    }

    std::string trimmed_msg = msg;
    trimmed_msg.erase(0, trimmed_msg.find_first_not_of(" \t\n\r\f\v"));
    trimmed_msg.erase(trimmed_msg.find_last_not_of(" \t\n\r\f\v") + 1);

    if (trimmed_msg.empty()) return;

    safe_print("handle_message: fd=" + std::to_string(fd) + ", nickname=" +
               nickname + ", is_admin=" + std::to_string(is_admin) +
               ", msg=" + trimmed_msg + "\n");

    if (nickname.empty())
    {
        if (trimmed_msg == ctx.admin_token)
        {
            std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
            if (ctx.clients.at(fd).ip == ADMIN_IP)
            {
                ctx.clients.at(fd).is_admin = true;
                ctx.clients.at(fd).nickname = ctx.admin_nickname;
                ctx.admin_token = "";
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
            if (!trimmed_msg.empty() && trimmed_msg[0] == '/')
            {
                std::string reply_msg = "昵称不能以'/'开头，请重新输入。\n";
                send(fd, reply_msg.c_str(), reply_msg.size(), 0);
                return;
            }

            if (trimmed_msg == ctx.admin_nickname)
            {
                std::string reply_msg = "昵称 '" + ctx.admin_nickname +
                                        "' 是保留昵称，请选择其他昵称。\n";
                send_message_with_length(fd, reply_msg);
                return;
            }

            bool is_name_taken = false;
            {
                std::lock_guard<std::mutex> names_lock(ctx.names_mtx);
                if (ctx.active_names.count(trimmed_msg))
                {
                    is_name_taken = true;
                }
            }
            if (is_name_taken)
            {
                std::string reply_msg =
                    "昵称 '" + trimmed_msg + "' 已被占用，请选择其他昵称。\n";
                // send(fd, reply_msg.c_str(), reply_msg.size(), 0);
                send_message_with_length(fd, reply_msg);
                disconnect_client(fd, ctx);
            }
            else
            {
                {
                    std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
                    ctx.clients.at(fd).nickname = trimmed_msg;
                    std::lock_guard<std::mutex> names_lock(ctx.names_mtx);
                    ctx.active_names.insert(trimmed_msg);
                }
                std::stringstream ss;
                ss << trimmed_msg << " 加入聊天室";
                std::string join_msg = ss.str();
                safe_print(join_msg + "\n");
                broadcast(join_msg, fd, ctx);
            }
        }
    }
    else
    {
        std::string command;
        std::istringstream iss(trimmed_msg);
        iss >> command;

        if (!command.empty() && command[0] == '/')
        {
            std::string actual_command = command;
            if (actual_command.size() > 1 && actual_command[1] == '/')
            {
                actual_command = actual_command.substr(1);
            }

            if (is_admin)
            {
                auto admin_cmd_iter = admin_commands.find(actual_command);
                if (is_admin && admin_cmd_iter != admin_commands.end())
                {
                    admin_cmd_iter->second(fd, trimmed_msg, ctx);
                    safe_print("客户端[" + nickname + "] 执行" +
                               actual_command + "\n");
                    return;
                }
            }

            auto user_cmd_iter = user_commands.find(actual_command);
            if (user_cmd_iter != user_commands.end())
            {
                user_cmd_iter->second(fd, trimmed_msg, ctx);
                safe_print("客户端[" + nickname + "] 执行 " + actual_command +
                           "\n");
                return;
            }

            // 私聊
            if (actual_command == "/w" || actual_command == "/whisper")
            {
                std::string target_nickname;
                std::string whisper_message;

                if (!(iss>>target_nickname))
                {
                    std::string reply = "用法: /w <昵称> <消息>。\n";
                    send_message_with_length(fd, reply);
                    return;
                }

                std::getline(iss, whisper_message);
                whisper_message.erase(
                    0, whisper_message.find_first_not_of(" \t\n\r\f\v"));

                if (whisper_message.empty())
                {
                    std::string reply = "私聊消息不能为空。\n";
                    send_message_with_length(fd, reply);
                    return;
                }

                if (target_nickname == nickname)
                {
                    std::string reply = "不能和自己私聊。\n";
                    send_message_with_length(fd, reply);
                    return;
                }

                int target_fd = -1;
                {
                    std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
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
                    std::string sender_info = "来自 " + nickname + " 的私聊：";
                    std::string whisper_reply_to_target =
                        sender_info + whisper_message;
                    send_message_with_length(target_fd,
                                             whisper_reply_to_target);
                    std::string confirm_msg =
                        "已向 " + target_nickname + " 发送私聊消息。\n";
                    send_message_with_length(fd, confirm_msg);
                }
                else
                {
                    std::string reply =
                        "用户 '" + target_nickname + "' 不在线或不存在。\n";
                    send_message_with_length(fd, reply);
                }

                return;
            }

            std::string reply = "未知命令或权限不足。\n";
            send_message_with_length(fd, reply);
        }
        else
        {
            std::stringstream ss;
            ss << nickname << ": " << msg;
            std::string out = ss.str();
            safe_print(out + "\n");
            broadcast(out, fd, ctx);
        }
    }
}
int main()
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sigint_hadler);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    ThreadPool pool(4);

    ServerContext ctx(pool);

    epoll_event events[MAX_EVENTS];

    // 映射

    std::unordered_map<std::string, CommandHandler> admin_commands;
    std::unordered_map<std::string, CommandHandler> user_commands;
    // 注册命令处理器
    admin_commands["/kick"] = handle_kick;
    admin_commands["/list"] = handle_list;
    user_commands["/list"] = handle_list;

    if (server_fd == -1)
    {
        perror("socket");
        return -1;
    }

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
    ctx.admin_token = ADMIN_TOKEN;
    const std::string ADMIN_NICKNAME = "admin";
    ctx.admin_nickname = ADMIN_NICKNAME;
    {
        std::lock_guard<std::mutex> lock(ctx.names_mtx);
        ctx.active_names.insert(ADMIN_NICKNAME);
    }

    Logger::getInstance().log("服务器启动，等待客户端连接...\n");
    safe_print("管理员口令已生成: " + ctx.admin_token + "\n");

    while (running)
    {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);

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
                std::lock_guard<std::mutex> names_lock(ctx.names_mtx);
                for (int cfd : ctx.to_remove)
                {
                    epoll_ctl(ctx.epoll_fd, EPOLL_CTL_DEL, cfd, nullptr);
                    if (ctx.clients.count(cfd))
                    {
                        ctx.active_names.erase(ctx.clients.at(cfd).nickname);
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
                while (true)
                {
                    sockaddr_in client_addr{};
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (sockaddr *)&client_addr,
                                           &client_addr_len);

                    if (client_fd == -1)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break;
                        }
                        else
                        {
                            perror("accept");
                        }
                        break;
                    }

                    set_nonblocking(client_fd);
                    epoll_event ev_client{};
                    ev_client.events = EPOLLIN || EPOLLET;
                    ev_client.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_client);
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str,
                              INET_ADDRSTRLEN);
                    std::string client_ip(ip_str);
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
    close(server_fd);
    close(epoll_fd);

    return 0;
}
