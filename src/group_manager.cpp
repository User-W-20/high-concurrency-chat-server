//
// Created by X on 2025/9/23.
//
#include "../include/group_manager.h"
#include <functional>
#include <sstream>

#include "Logger.h"


GroupManager::GroupManager(MessageSender sender,
                           const ServerContext&ctx_ref) :
    message_sender(std::move(sender)),ctx_ref(ctx_ref)
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
    const std::string& creator_nickname, const std::vector<std::string>& parts)
{
    if (parts.size() < 2)
    {
        return "用法: /create <群名>";
    }

    std::string group_name = parts[1];

    if (group_name.empty())
    {
        return "群名不能为空。\n";
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (groups.count(group_name))
    {
        return "错误：群组 '" + group_name + "' 已经存在。\n";
    }

    Group new_group;
    new_group.name = group_name;
    new_group.owner_nickname = creator_nickname;

    new_group.members.insert(creator_nickname);

    groups.emplace(group_name, std::move(new_group));

    LOG_INFO("用户 [" + creator_nickname + "] 创建了群组: " + group_name);

    return "恭喜！群组 '" + group_name + "' 创建成功，您已自动成为群主。\n";
}


std::string GroupManager::handle_join_group(
    const std::string& username, const std::vector<std::string>& parts)
{
    if (parts.size() < 2)
    {
        return "用法: /join <群名>";
    }

    const std::string& group_name = parts[1];

    std::lock_guard<std::mutex> lock(mtx);

    auto it = groups.find(group_name);
    if (it == groups.end())
    {
        return "错误：群组 '" + group_name + "' 不存在。\n";
    }

    Group& group = it->second;

    if (group.members.count(username))
    {
        return "您已在该群组中。\n";
    }

    group.members.insert(username);

    LOG_INFO("用户 [" + username + "] 加入了群组: " + group_name);

    return "成功加入群组 '" + group_name + "'。\n";
}

std::string GroupManager::handle_list_groups()
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::string group_list = "所有群: ";
        bool first = true;
        for (const auto& pair : groups)
        {
            if (!first)
            {
                group_list += ", ";
            }
            group_list += pair.first;
            first = false;
        }
        return groups.empty() ? "目前没有群。" : group_list;
    }
}

std::string GroupManager::handle_send_message(
    const std::string& username, const std::vector<std::string>& parts)
{
    if (parts.size() < 3)
    {
        return "用法: /send <群名> <消息>\n";
    }

    const std::string& group_name = parts[1];
    std::string message_content;
    for (size_t i = 2; i < parts.size(); ++i)
    {
        message_content += parts[i] + (i == parts.size() - 1 ? "" : " ");
    }

    std::string full_message = "[" + group_name + "]" + username + ": " +
                               message_content;

    {
        std::lock_guard<std::mutex> lock(mtx);

        auto group_it = groups.find(group_name);
        if (group_it == groups.end())
        {
            return "错误：该群不存在。\n";
        }

        const Group& group = group_it->second;

        if (group.members.find(username) == group.members.end())
        {
            return "错误：您不是该群的成员。\n";
        }

        for (const std::string& member_name : group.members)
        {
            int member_fd=ctx_ref.get_fd_by_nickname(member_name);

            if (member_fd!=-1)
            {
                message_sender(member_fd,full_message+"\n");
            }
        }
        return "";
    }
}

void GroupManager::remove_client_from_groups(const std::string& username)
{
    {
        std::lock_guard<std::mutex> lock(mtx);

        for (auto& pair : groups)
        {
            pair.second.members.erase(username);
        }

        for (auto it = groups.begin(); it != groups.end();)
        {
            const Group& group = it->second;

            bool should_disband = false;

            if (group.owner_nickname == username)
            {
                LOG_INFO(
                    "群组 [" + it->first + "] 的群主 [" + username + "] 已退出，群组自动解散。")
                ;
                should_disband = true;
            }
            else if (group.members.empty())
            {
                LOG_INFO("群组 [" + it->first + "] 成员清空，自动解散。");
                should_disband = true;
            }

            if (should_disband)
            {
                it = groups.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

std::string GroupManager::handle_group_kick(const std::string& kicker_nickname, const std::vector<std::string>& parts)
{
    if (parts.size()<3)
    {
        return "用法: /groupkick <群名> <昵称>。\n";
    }

    const std::string& group_name=parts[1];
    const std::string& victim_nickname=parts[2];

    std::lock_guard<std::mutex>lock(mtx);

    auto group_it=groups.find(group_name);

    if (group_it==groups.end())
    {
        return "错误：群组 '" + group_name + "' 不存在。\n";
    }

    Group& group=group_it->second;

    if (group.owner_nickname!=kicker_nickname)
    {
        return "错误：您不是群组 '" + group_name + "' 的群主，无权执行此操作。\n";
    }

    if (kicker_nickname==victim_nickname)
    {
        return "错误：群主不能踢自己。\n";
    }

    auto member_it=group.members.find(victim_nickname);
    if (member_it==group.members.end())
    {
        return "错误：用户 '" + victim_nickname + "' 不是群组 '" + group_name + "' 的成员。\n";
    }

    group.members.erase(member_it);

    LOG_INFO("群主 [" + kicker_nickname + "] 将 [" + victim_nickname + "] 踢出群组 [" + group_name + "]");

    int victim_fd=ctx_ref.get_fd_by_nickname(victim_nickname);

    if (victim_fd!=-1)
    {
        message_sender(victim_fd, "您已被群主 [" + kicker_nickname + "] 踢出群组 [" + group_name + "]。\n");
    }

    return "已将用户 '" + victim_nickname + "' 踢出群组 '" + group_name + "'。\n";
}
