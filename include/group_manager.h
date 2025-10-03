//
// Created by X on 2025/9/23.
//

#ifndef LITECHAT_GROUP_MANAGER_H
#define LITECHAT_GROUP_MANAGER_H
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include "ServerContext.h"


using MessageSender=std::function<void(int ,const std::string&)>;

struct Group
{
    std::string name;

    std::string owner_nickname;

    std::unordered_set<std::string>members;
};

class GroupManager
{
   public:
    explicit GroupManager(MessageSender sender, const ServerContext&ctx_ref);
    ~GroupManager()=default;

    std::string handle_create_group(const std::string& username, const std::vector<std::string>& parts);
    std::string handle_join_group(const std::string& username, const std::vector<std::string>& parts);
    std::string handle_send_message(const std::string& username, const std::vector<std::string>& parts);
    std::string handle_list_groups();
    void remove_client_from_groups(const std::string&username);
    std::string handle_group_kick(const std::string &kicker_nickname,const std::vector<std::string>&parts);
    std::string handle_group_leave(const std::string& username,const std::vector<std::string>&parts);
   private:
    std::unordered_map<std::string,Group>groups;
    std::mutex mtx;

    MessageSender message_sender;

    const ServerContext&ctx_ref;

    static std::vector<std::string>split(const std::string&s,char delimiter);
};

#endif  // LITECHAT_GROUP_MANAGER_H
