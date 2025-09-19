#include<iostream>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cerrno>
#include <csignal>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <sstream>
#include "../include/Logger.h"
#include "../include/threadpool.h"
#include "../include/client.h"

constexpr int MAX_EVENTS = 1024;
constexpr int PORT = 5008;
constexpr int BUF_SIZE = 1024;
constexpr int HEARTBEAT_TIMEOUT = 60; //心跳超时
const std::string ADMIN_IP = "127.0.0.1";

bool running = true;

void safe_print(const std::string &msg) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    std::cout << msg << std::flush;
}


void set_nonblocking(int fd) {
    int flags = fcntl(fd,F_GETFL, 0);
    fcntl(fd,F_SETFL, flags | O_NONBLOCK);
}

struct ServerContext {
    std::unordered_map<int, Client> clients{};
    std::mutex clients_mtx{};
    std::vector<int> to_remove{};
    std::mutex to_remove_mtx{};
    int epoll_fd{};
    ThreadPool &pool;
    std::unordered_set<std::string> active_names{};
    std::mutex names_mtx{};
    std::string admin_token{};
    std::mutex token_mtx{};

    explicit ServerContext(ThreadPool &p) : pool(p) {}
};

void broadcast(const std::string &msg, int sender_fd, ServerContext &ctx) {
    ctx.pool.enqueue([msg,sender_fd,&ctx]() {
        std::vector<int> current_clients;

        {
            std::lock_guard<std::mutex> lock(ctx.clients_mtx);
            for (const auto &pair: ctx.clients) {
                current_clients.push_back(pair.first);
            }
        }

        uint32_t msg_len = htonl(msg.size());
        std::string full_msg(reinterpret_cast<const char *>(&msg_len), sizeof(msg_len));
        full_msg += msg;

        for (int cfd: current_clients) {
            if (cfd != sender_fd) {
                send(cfd, full_msg.data(), full_msg.size(), 0);
            }
        }
    });
}

void disconncet_client(int fd, ServerContext &ctx) {
    std::string name;
    {
        std::lock_guard<std::mutex> nlock(ctx.clients_mtx);
        name = (ctx.clients.count(fd) ? ctx.clients.at(fd).nickname : "未知用户");
    }

    std::string quit_msg = name + " 退出聊天室";
    safe_print(quit_msg + "\n");
    broadcast(quit_msg, fd, ctx);

    {
        std::lock_guard<std::mutex> lock(ctx.to_remove_mtx);
        ctx.to_remove.push_back(fd);
    }
}

void sigint_hadler(int) {
    running = false;
}


int main() {
    signal(SIGPIPE,SIG_IGN);
    signal(SIGINT, sigint_hadler);
    int server_fd = socket(AF_INET,SOCK_STREAM, 0);

    ThreadPool pool(4);

    ServerContext ctx(pool);


    epoll_event events[MAX_EVENTS];

    if (server_fd == -1) {
        perror("socket");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    int opt = 1;
    setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("bind");
        return -1;
    }

    if (listen(server_fd,SOMAXCONN) == -1) {
        perror("listen");
        return -1;
    }

    set_nonblocking(server_fd);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return -1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd,EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl add server_fd");
        close(server_fd);
        return -1;
    }

    ctx.epoll_fd = epoll_fd;

    const std::string ADMIN_TOKEN = "admin123";
    ctx.admin_token = ADMIN_TOKEN;

    Logger::getInstance().log("服务器启动，等待客户端连接...\n");
    safe_print("管理员口令已生成: " + ctx.admin_token + "\n");
    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

        if (nfds == -1) {
            if (errno == EINTR)continue;
            perror("epoll_wait");
            break;
        }

        //心跳检测
        if (nfds == 0) {
            std::vector<int> inactive_fds;
            {
                std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                auto now = std::chrono::steady_clock::now();
                for (const auto &pair: ctx.clients) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - pair.second.last_activity).count() >
                        HEARTBEAT_TIMEOUT) {
                        inactive_fds.push_back(pair.first);
                    }
                }
            }

            for (int fd_to_disconnect: inactive_fds) {
                disconncet_client(fd_to_disconnect, ctx);
                safe_print("客户端 " + std::to_string(fd_to_disconnect) + " 因超时自动断开连接。\n");
            }
        }

        {
            std::lock_guard<std::mutex> rm_lock(ctx.to_remove_mtx);
            if (!ctx.to_remove.empty()) {
                std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
                std::lock_guard<std::mutex> names_lock(ctx.names_mtx);
                for (int cfd: ctx.to_remove) {
                    epoll_ctl(ctx.epoll_fd,EPOLL_CTL_DEL, cfd, nullptr);
                    if (ctx.clients.count(cfd)) {
                        ctx.active_names.erase(ctx.clients.at(cfd).nickname);
                        ctx.clients.erase(cfd);
                    }
                    safe_print(("[CLEAN] 客户端[" + std::to_string(cfd) + "] 已被清理\n"));
                    close(cfd);
                }
                ctx.to_remove.clear();
            }
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            bool disconnect = false;

            if (fd == server_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (sockaddr *) &client_addr, &client_addr_len);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            perror("accept");
                        }
                        break;
                    }
                    set_nonblocking(client_fd);
                    epoll_event ev_client{};
                    ev_client.events = EPOLLIN | EPOLLET;
                    ev_client.data.fd = client_fd;

                    epoll_ctl(epoll_fd,EPOLL_CTL_ADD, client_fd, &ev_client);

                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str,INET_ADDRSTRLEN);
                    std::string client_ip(ip_str);

                    std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                    ctx.clients.emplace(client_fd, Client(client_fd, client_ip));
                }
            } else if (events[i].events & EPOLLIN) {
                while (true) {
                    uint32_t net_len;
                    ssize_t n_len = recv(fd, &net_len, sizeof(net_len),MSG_PEEK);

                    if (n_len == 0) {
                        disconnect = true;
                        break;
                    }

                    if (n_len < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            disconnect = true;
                            break;
                        }
                    }

                    //不够4字节，数据不完整，跳过
                    if (n_len < sizeof(net_len)) {
                        break;
                    }

                    uint32_t msg_len = ntohl(net_len);

                    std::vector<char> buf(msg_len + sizeof(net_len));

                    ssize_t total_n = recv(fd, buf.data(), buf.size(), 0);

                    if (total_n == 0) {
                        disconnect = true;
                        break;
                    }
                    if (total_n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            disconnect = true;
                            break;
                        }
                    }

                    //不足一个完整数据包
                    if (total_n < msg_len + sizeof(net_len)) {
                        break;
                    }

                    {
                        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                        if (ctx.clients.count(fd)) {
                            ctx.clients.at(fd).last_activity = std::chrono::steady_clock::now();
                        }
                    }

                    std::string msg(buf.begin() + sizeof(net_len), buf.end());

                    if (msg == "quit") {
                        disconnect = true;
                        break;
                    }

                    std::string nickname;
                    bool is_admin = false;
                    {
                        std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                        if (ctx.clients.count(fd)) {
                            nickname = ctx.clients.at(fd).nickname;
                            is_admin = ctx.clients.at(fd).is_admin;
                        }
                    }

                    std::string trimmed_msg = msg;
                    trimmed_msg.erase(0, trimmed_msg.find_first_not_of(" \t\n\r\f\v"));
                    trimmed_msg.erase(trimmed_msg.find_last_not_of(" \t\n\r\f\v") + 1);

                    if (nickname.empty() && trimmed_msg == ctx.admin_token && !ctx.admin_token.empty()) {
                        {
                            std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
                            ctx.clients.at(fd).is_admin = true;

                            ctx.admin_token = "";
                        }
                        std::string reply = "恭喜，您已成为管理员！\n";
                        send(fd, reply.c_str(), reply.size(), 0);
                        safe_print("客户端 " + std::to_string(fd) + " 已验证为管理员。\n");
                        break;
                    }
                    //昵称注册
                    else if (nickname.empty()) {
                        bool is_name_token = false;
                        {
                            std::lock_guard<std::mutex> names_lock(ctx.names_mtx);
                            if (ctx.active_names.count(msg)) {
                                is_name_token = true;
                            }
                        }
                        if (is_name_token) {
                            std::string reply_msg = "昵称 '" + msg + "' 已被占用，请选择其他昵称。";
                            send(fd, reply_msg.c_str(), reply_msg.size(), 0);
                            Logger::getInstance().log(
                                "客户端 [" + std::to_string(fd) + "] 尝试使用已占用昵称: " + msg + "\n");
                        } else {
                            {
                                std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);
                                ctx.clients.at(fd).nickname = msg;
                                std::lock_guard<std::mutex> names_lock(ctx.names_mtx);
                                ctx.active_names.insert(msg);
                            }
                            std::string join_msg = msg + " 加入聊天室";
                            safe_print(join_msg + "\n");
                            broadcast(join_msg, fd, ctx);
                        }
                        break;
                    } else if (is_admin && trimmed_msg.rfind("/kick", 0) == 0) {
                        std::string target_nickname;
                        std::istringstream iss(trimmed_msg);
                        std::string command;
                        iss >> command >> target_nickname;

                        if (target_nickname.empty()) {
                            std::string reply = "请指定要踢出的用户昵称。\n";
                            send(fd, reply.c_str(), reply.size(), 0);
                            break;
                        }
                        int target_fd = -1;
                        {
                            std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                            for (const auto &pair: ctx.clients) {
                                if (pair.second.nickname == target_nickname) {
                                    target_fd = pair.first;
                                    break;
                                }
                            }
                        }
                        if (target_fd != -1) {
                            std::string admin_name;
                            {
                                std::lock_guard<std::mutex> lock(ctx.clients_mtx);
                                admin_name = ctx.clients.at(fd).nickname;
                            }

                            std::stringstream ss;
                            ss<<admin_name<<" 将 "<<target_nickname<< " 踢出聊天室。\n";
                            safe_print(ss.str());
                            disconncet_client(target_fd, ctx);
                            std::string reply = "您已被管理员踢出聊天室。\n";
                            uint32_t reply_len = htonl(reply.size());
                            std::string full_reply(reinterpret_cast<const char *>(&reply_len), sizeof(reply_len));
                            full_reply += reply;
                            send(target_fd, full_reply.data(), full_reply.size(), 0);
                        } else {
                            std::string reply = "用户 '" + target_nickname + "' 不在线。\n";
                            send(fd, reply.c_str(), reply.size(), 0);
                        }
                        break;
                    } else if (trimmed_msg == "list") {
                        //list 命令处理
                        std::string list = "在线用户：";
                        std::lock_guard<std::mutex> client_lock(ctx.clients_mtx);

                        for (const auto &pair: ctx.clients) {
                            list += pair.second.nickname + " ";
                        }
                        uint32_t list_len = htonl(list.size());
                        std::string full_list(reinterpret_cast<const char *>(&list_len), sizeof(list_len));
                        full_list += list;
                        send(fd, full_list.data(), full_list.size(), 0);
                        safe_print("客户端[" + nickname + "] 执行 /list\n");
                        break;
                    } else if (trimmed_msg.rfind("/kick", 0) == 0 || trimmed_msg.rfind("/other_admin_cmd", 0) == 0) {
                        std::string reply = "权限不足，无法执行此命令。\n";
                        send(fd, reply.c_str(), reply.size(), 0);
                        break;
                    } else {
                        //广播
                        std::stringstream ss;
                        ss<<nickname<<": " <<msg;
                        std::string out = ss.str();
                        safe_print(out + "\n");
                        broadcast(out, fd, ctx);
                    }
                }
            }
            if (disconnect) {
                disconncet_client(fd, ctx);
            }
        }
    }

    // 关闭所有客户端
    close(server_fd);
    close(epoll_fd);
    return 0;
}
