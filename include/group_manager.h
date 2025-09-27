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

#include "client.h"

using MessageSender=std::function<void(int ,const std::string&)>;

class GroupManager
{
   public:
    explicit GroupManager(MessageSender sender,const std::unordered_map<int,Client>&clients_map);
    ~GroupManager();

    std::string handle_create_group(const std::string& username, const std::vector<std::string>& parts);
    std::string handle_join_group(const std::string& username, const std::vector<std::string>& parts);
    std::string handle_send_message(const std::string& username, const std::vector<std::string>& parts);
    std::string handle_list_groups();
    void remove_client_from_groups(const std::string&username);
   private:
    std::unordered_map<std::string,std::unordered_set<std::string>>groups;
    std::mutex mtx;

    MessageSender message_sender;

    const std::unordered_map<int,Client>&clients_ref;

    std::vector<std::string>split(const std::string&s,char delimiter);
};

#endif  // LITECHAT_GROUP_MANAGER_H
