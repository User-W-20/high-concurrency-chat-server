//
// Created by X on 2025/9/24.
//

#ifndef LITECHAT_SERVERCONTEXT_H
#define LITECHAT_SERVERCONTEXT_H
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <atomic>
#include <memory>
#include <functional>
#include "client.h"

class ThreadPool;
class GroupManager;

using MessageSender=std::function<void(int,const std::string&)>;

void send_message_with_length(int fd,const std::string &msg);

struct ServerContext
{
    std::unordered_map<int,Client>clients{};
    std::mutex  clients_mtx{};

    std::vector<int>to_remove{};
    std::mutex to_remove_mtx{};

    int epoll_fd{};
    ThreadPool &pool;
    std::string admin_token{};
    std::string admin_nickname{};
    std::mutex token_mtx{};
    std::atomic<bool>shutdown_requested=false;

    bool is_user_admin(const std::string &nickname);

    std::unique_ptr<GroupManager>group_manager;

    explicit ServerContext(ThreadPool&p,const MessageSender& sender);

    void broadcast(const std::string &msg,int sender_fd);
    std::string get_username(int fd);
    void set_username(int fd,const std::string &username);
    void remove_client(int fd);

    bool kick_user_by_nickname(const std::string&target_nickname,const std::string&kicker_nickname);
};

#endif  // LITECHAT_SERVERCONTEXT_H
