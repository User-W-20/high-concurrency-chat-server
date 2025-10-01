//
// Created by X on 2025/9/23.
//
#include "../include/group_manager.h"
#include "../include/client.h"
#include <functional>
#include <sstream>


GroupManager::GroupManager(MessageSender sender, std::unordered_map<int,Client>&clients_map) : message_sender(std::move(sender)),clients_ref(std::move(clients_map))
{

}

std::vector<std::string> GroupManager::split(const std::string& s,
                                             char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter))
    {
        tokens.push_back(token);
    }

    return tokens;
}


std::string GroupManager::handle_create_group(
    const std::string& username, const std::vector<std::string>& parts)
{
    if (parts.size()<2)
    {
        return "用法: /create <群名>";
    }

    const std::string &group_name=parts[1];
    {
        std::lock_guard<std::mutex>lock(mtx);
        if (groups.count(group_name))
        {
            return "该群已存在。";
        }else
        {
            groups[group_name].insert(username);
            return "群 '" + group_name + "' 创建成功，您已加入。";
        }
    }
}


std::string GroupManager::handle_join_group(
    const std::string& username, const std::vector<std::string>& parts)
{
    if (parts.size()<2)
    {
        return "用法: /join <群名>";
    }

    const std::string &group_name=parts[1];
    {
        std::lock_guard<std::mutex>lock(mtx);
        if (!groups.count(group_name))
        {
            return "该群不存在。";
        }else if (groups[group_name].count(username))
        {
            return "您已是该群成员。";
        }else
        {
            groups[group_name].insert(username);
            return "您已成功加入群 '" + group_name + "'.";
        }
    }
}

std::string GroupManager::handle_list_groups()
{
    {
        std::lock_guard<std::mutex>lock(mtx);
        std::string group_list="所有群: ";
        bool first=true;
        for (const auto&pair:groups)
        {
            if (!first)
            {
                group_list+= ", ";
            }
            group_list+=pair.first;
            first=false;
        }
        return groups.empty()? "目前没有群。" : group_list;
    }
}

std::string GroupManager::handle_send_message(
    const std::string& username, const std::vector<std::string>& parts)
{
    if (parts.size()<3)
    {
        return "用法: /send <群名> <消息>";
    }

    const  std::string &group_name=parts[1];
    std::string message_content;
    for (size_t i=2;i<parts.size();++i)
    {
        message_content+=parts[i]+(i==parts.size()-1?"":" ");
    }

    {
        std::lock_guard<std::mutex>lock(mtx);
        if (!groups.count(group_name))
        {
            return "该群不存在。";
        }else if (!groups[group_name].count(username))
        {
            return "您不是该群的成员。";
        }else
        {
            std::string full_message="["+group_name+"]"+username+": "+message_content;
            for (const std::string& member_name:groups.at(group_name))
            {

                for (const auto &pair:clients_ref)
                {
                    if (pair.second.nickname==member_name)
                    {
                        message_sender(pair.first,full_message);
                        break;
                    }
                }
            }
        }
        return "";
    }
}

void GroupManager::remove_client_from_groups(const std::string& username)
{
    {
        std::lock_guard<std::mutex>lock(mtx);
        for (auto&pair:groups)
        {
            pair.second.erase(username);
        }
    }
}
