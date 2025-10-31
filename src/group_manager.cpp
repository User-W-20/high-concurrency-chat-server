//
// Created by X on 2025/9/23.
//
#include "../include/group_manager.h"

#include <unistd.h>

#include <functional>
#include <sstream>
#include <fstream>
#include "Logger.h"
#include "../include/json.hpp"
#include "../include/UserManager.h"
using json = nlohmann::json;


GroupManager::GroupManager(MessageSender sender,
                           const ServerContext& ctx_ref) :
    message_sender(std::move(sender)), ctx_ref(ctx_ref)
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

std::string GroupManager::to_lower_nickname(const std::string& nickname)
{
    std::string lower_nickname = nickname;

    std::transform(lower_nickname.begin(), lower_nickname.end(),
                   lower_nickname.begin(), [](unsigned char c)
                   {
                       return std::tolower(c);
                   });

    return lower_nickname;
}


std::string GroupManager::handle_create_group(
    const std::string& creator_nickname_raw,
    const std::vector<std::string>& parts)
{
    if (parts.size() < 2 || parts.size() > 3)
    {
        return "用法: /creategroup <群名> [密码]";
    }

    const std::string& group_name_raw = parts[1];
    std::string group_name = to_lower_nickname(group_name_raw);
    std::string creator_nickname = to_lower_nickname(creator_nickname_raw);

    if (group_name.empty())
    {
        return "群名不能为空。\n";
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (groups.count(group_name))
    {
        return "错误：群组 '" + group_name_raw + "' 已经存在。\n";
    }

    Group new_group;
    new_group.name = group_name;
    new_group.owner_nickname = creator_nickname;
    new_group.members.insert(creator_nickname);

    if (parts.size() == 3)
    {
        const std::string& password = parts[2];

        std::string encoded_hash;

        if (UserManager::hash_password(password, encoded_hash))
        {
            new_group.password_hash = encoded_hash;

            LOG_INFO("用户 [" + creator_nickname + "] 创建了密码保护群组: " + group_name);
            groups.emplace(group_name, std::move(new_group));
            return "恭喜！群组 '" + group_name + "' 创建成功，已设置密码，您是群主。\n";
        }
        else
        {
            return "错误: 密码处理失败，群组创建中止。\n";
        }
    }
    else
    {
        new_group.password_hash = "";

        LOG_INFO("用户 [" + creator_nickname + "] 创建了公开群组: " + group_name);
        groups.emplace(group_name, std::move(new_group));
        return "恭喜！群组 '" + group_name + "' 创建成功，您已自动成为群主。\n";
    }
}


std::string GroupManager::handle_join_group(
    const std::string& username_raw, const std::vector<std::string>& parts)
{
    if (parts.size() < 2 || parts.size() > 3)
    {
        return "用法: /join <群名> [密码]";
    }

    const std::string& group_name_raw = parts[1];
    std::string group_name = to_lower_nickname(group_name_raw);
    std::string username = to_lower_nickname(username_raw);

    std::lock_guard<std::mutex> lock(mtx);

    auto it = groups.find(group_name);
    if (it == groups.end())
    {
        return "错误：群组 '" + group_name + "' 不存在。\n";
    }

    Group& group = it->second;

    if (group.banned_members.count(username))
    {
        return "错误：您已被群组 '" + group_name + "' 禁止重新加入。\n";
    }

    if (group.members.count(username))
    {
        return "您已在该群组中。\n";
    }

    if (!group.password_hash.empty())
    {
        if (parts.size() < 3)
        {
            return "错误: 群组 '" + group_name +
                   "' 是私有群组，需要密码才能加入。用法: /join <群名> <密码>\n";
        }

        const std::string& provided_password = parts[2];

        if (!UserManager::verify_password(group.password_hash,
                                          provided_password))
        {
            return "错误: 您提供的群组密码不正确。\n";
        }
    }

    group.members.insert(username);

    LOG_INFO("用户 [" + username + "] 加入了群组: " + group_name);

    return "成功加入群组 '" + group_name_raw + "'。\n";
}

std::string GroupManager::handle_list_groups() const
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
    const std::string& username_raw, const std::vector<std::string>& parts)
{
    if (parts.size() < 3)
    {
        return "用法: /send <群名> <消息>\n";
    }

    const std::string& group_name_raw = parts[1];
    std::string group_name = to_lower_nickname(group_name_raw);
    std::string username = to_lower_nickname(username_raw);

    std::string message_content;
    for (size_t i = 2; i < parts.size(); ++i)
    {
        message_content += parts[i] + (i == parts.size() - 1 ? "" : " ");
    }

    std::string full_message = "[" + group_name_raw + "]" + username + ": " +
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
            int member_fd = ctx_ref.get_fd_by_nickname(member_name);

            if (member_fd != -1)
            {
                message_sender(member_fd, full_message + "\n");
            }
        }
        return "";
    }
}

void GroupManager::remove_client_from_groups(const std::string& username_raw)
{
    std::string username = to_lower_nickname(username_raw);
    LOG_INFO("客户端 [" +username+"] 已断开连接，群组永久数据保持不变。");
}

std::string GroupManager::handle_group_leave(const std::string& username_raw,
                                             const std::vector<std::string>&
                                             parts)
{
    if (parts.size() < 2)
    {
        return "用法: /leave <群名>\n";
    }

    const std::string& group_name_raw = parts[1];
    std::string group_name = to_lower_nickname(group_name_raw);
    std::string username = to_lower_nickname(username_raw);

    std::lock_guard<std::mutex> lock(mtx);
    auto group_it = groups.find(group_name);

    if (group_it == groups.end())
    {
        return "错误：群组 '" + group_name + "' 不存在。\n";
    }

    Group& group = group_it->second;
    auto member_it = group.members.find(username);

    if (member_it == group.members.end())
    {
        return "错误：您不是群组 '" + group_name + "' 的成员。\n";
    }

    bool group_will_be_deleted = false;
    std::string broadcast_msg;
    std::string return_msg;
    std::unordered_set<std::string> members_to_notify = group.members;

    if (group.owner_nickname == username)
    {
        bool successfully_transferred = false;
        if (group.members.size() > 1)
        {
            std::string new_owner;
            for (const auto& member : group.members)
            {
                if (member != username)
                {
                    new_owner = member;
                    break;
                }
            }
            if (!new_owner.empty())
            {
                group.owner_nickname = new_owner;
                group.members.erase(member_it);

                broadcast_msg = "【系统】原群主 [" + username + "] 主动离开了群组 [" +
                                group_name + "]";
                broadcast_msg += "。群主已转让给 [" + new_owner + "]。\n";
                // 添加句号、空格和换行符

                return_msg = "您已成功退出群组 '" + group_name + "'，群主已转让给 [" +
                             new_owner + "]。\n";

                successfully_transferred = true;
            }
        }

        if (!successfully_transferred)
        {
            broadcast_msg = "【系统】群主 [" + username + "] 离开了群组 [" + group_name_raw
                            +
                            "]。群组已解散。\n";

            groups.erase(group_it);
            group_will_be_deleted = true;

            return_msg = "您已成功退出群组 '" + group_name_raw + "'，群组已解散。\n";
        }
    }
    else
    {
        members_to_notify = group.members;

        group.members.erase(member_it);
        broadcast_msg = "【系统】成员 [" + username + "] 主动离开了群组 [" + group_name_raw +
                        "]\n";
        return_msg = "您已成功退出群组 [" + group_name_raw + "]\n";

        if (group.members.empty())
        {
            LOG_INFO("群组 [" + group_name + "] 所有成员已主动退出，群组解散。");
            groups.erase(group_it);
            return_msg += "由于您是最后一位成员，群组已解散。\n";

            group_will_be_deleted = true;
        }
    }

    if (!broadcast_msg.empty())
    {
        const std::unordered_set<std::string>& target_members =
            group_will_be_deleted ? members_to_notify : group.members;

        for (const auto& member_name : target_members)
        {
            if (!group_will_be_deleted && member_name == username)
            {
                continue;
            }

            int member_fd = ctx_ref.get_fd_by_nickname(member_name);
            if (member_fd != -1)
            {
                message_sender(member_fd, broadcast_msg);
            }
        }
    }

    if (group_will_be_deleted)
    {
        LOG_ERROR("Group [" +group_name+ "] 标记解散，解散操作已执行。");
    }
    return return_msg;
}

std::string GroupManager::handle_group_kick(
    const std::string& kicker_nickname_raw,
    const std::vector<std::string>&
    parts)
{
    if (parts.size() < 3)
    {
        return "用法: /groupkick <群名> <昵称>。\n";
    }

    const std::string& group_name_raw = parts[1];
    std::string group_name = to_lower_nickname(group_name_raw);
    std::string kicker_nickname = to_lower_nickname(kicker_nickname_raw);
    std::string victim_nickname = to_lower_nickname(parts[2]);

    std::lock_guard<std::mutex> lock(mtx);

    auto group_it = groups.find(group_name);

    if (group_it == groups.end())
    {
        return "错误：群组 '" + group_name + "' 不存在。\n";
    }

    Group& group = group_it->second;

    if (group.owner_nickname != kicker_nickname)
    {
        return "错误：您不是群组 '" + group_name_raw + "' 的群主，无权执行此操作。\n";
    }

    if (kicker_nickname == victim_nickname)
    {
        return "错误：群主不能踢自己。\n";
    }

    auto member_it = group.members.find(victim_nickname);
    if (member_it == group.members.end())
    {
        return "错误：用户 '" + victim_nickname + "' 不是群组 '" + group_name_raw +
               "' 的成员。\n";
    }

    std::unordered_set<std::string> members_to_notify = group.members;

    group.members.erase(member_it);

    group.banned_members.insert(victim_nickname);

    std::string broadcast_msg = "【系统】用户 [" + victim_nickname + "] 已被群主 [" +
                                kicker_nickname + "] 踢出群组 [" + group_name_raw +
                                "]\n";
    std::string return_msg = "成功将用户 [" + victim_nickname + "] 踢出群组 [" +
                             group_name_raw + "]\n";

    bool group_was_deleted = false;

    if (group.members.empty())
    {
        LOG_INFO("群组 [" + group_name + "] 被踢后已清空，群组解散。");
        groups.erase(group_it);
        return_msg += "由于该操作导致群组成员清空，群组已解散。\n";
        group_was_deleted = true;
    }

    if (!broadcast_msg.empty())
    {
        const std::unordered_set<std::string>& target_nickname =
            group_was_deleted ? members_to_notify : group.members;

        if (!group_was_deleted)
        {
            members_to_notify.insert(victim_nickname);
        }

        for (const auto& member_name : target_nickname)
        {
            int member_fd = ctx_ref.get_fd_by_nickname(member_name);
            if (member_fd != -1)
            {
                message_sender(member_fd, broadcast_msg);
            }
        }

        int victim_fd = ctx_ref.get_fd_by_nickname(victim_nickname);
        if (victim_fd != -1)
        {
            message_sender(victim_fd, broadcast_msg);
        }
    }

    return return_msg;
}

void GroupManager::load_groups_from_file(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(mtx);

    std::ifstream i(filename.c_str());

    if (!i.is_open())
    {
        LOG_WARNING("未找到群组数据文件 (" +filename+")，以空群组列表启动。");
        return;
    }

    try
    {
        json root_json;
        i >> root_json;
        i.close();

        groups = root_json.at("groups").get<std::unordered_map<
            std::string, Group>>();

        std::stringstream log_ss;
        log_ss << "成功从文件加载 " << groups.size() << " 个群组数据。";
        LOG_INFO(log_ss.str());
    }
    catch (const json::exception& e)
    {
        std::stringstream err_ss;
        err_ss << "加载群组数据失败，JSON 解析或数据结构错误: " << e.what();
        groups.clear();
    }
    catch (const std::exception& e)
    {
        std::stringstream err_ss;
        err_ss << "加载群组数据失败: " << e.what();
        LOG_ERROR(err_ss.str());
        groups.clear();
    }
}

void GroupManager::save_groups_to_file(const std::string& filename) const
{
    std::lock_guard<std::mutex> lock(mtx);

    json root_json;

    root_json["groups"] = groups;

    std::ofstream o(filename.c_str());

    if (o.is_open())
    {
        try
        {
            o << root_json.dump(4);
            o.close();
            LOG_INFO("群组数据成功保存到: "+filename);
        }
        catch (const std::exception& e)
        {
            std::stringstream err_ss;
            err_ss << "写入群组数据时发生内部错误: " << e.what();
            LOG_ERROR(err_ss.str());
        }
    }
    else
    {
        LOG_ERROR("无法打开文件进行写入: "+filename);
    }
}

std::string GroupManager::handle_group_unban(
    const std::string& kicker_nickname_raw,
    const std::vector<std::string>& parts)
{
    if (parts.size() < 3)
    {
        return "用法: /groupunban <群名> <昵称>。\n";
    }

    const std::string& group_name_raw = parts[1];
    std::string group_name = to_lower_nickname(group_name_raw);
    std::string kicker_nickname = to_lower_nickname(kicker_nickname_raw);
    std::string target_nickname = to_lower_nickname(parts[2]);

    std::lock_guard<std::mutex> lock(mtx);

    auto group_it = groups.find(group_name);
    if (group_it == groups.end())
    {
        return "错误：群组 '" + group_name_raw + "' 不存在。\n";
    }

    Group& group = group_it->second;

    if (group.owner_nickname != kicker_nickname)
    {
        return "错误：您不是群组 '" + group_name_raw + "' 的群主，无权执行此操作。\n";
    }

    size_t removed_count = group.banned_members.erase(target_nickname);

    if (removed_count == 0)
    {
        return "错误：用户 '" + target_nickname + "' 不在群组 '" + group_name_raw +
               "' 的禁止（黑）名单中。\n";
    }

    std::string broadcast_msg = "【系统】用户 [" + target_nickname + "] 已被群主 [" +
                                kicker_nickname_raw + "] 解除了群组 [" +
                                group_name_raw +
                                "] 的加入限制。\n";

    for (const auto& member_name : group.members)
    {
        int member_fd = ctx_ref.get_fd_by_nickname(member_name);
        if (member_fd != -1)
        {
            message_sender(member_fd, broadcast_msg);
        }
    }

    return "成功将用户 [" + target_nickname + "] 从群组 [" + group_name_raw +
           "] 的限制中解除。他们现在可以重新加入。\n";
}

std::string GroupManager::handle_group_transfer(
    const std::string& kicker_nickname_raw,
    const std::vector<std::string>& parts)
{
    if (parts.size() < 3)
    {
        return "用法: /transfer <群名> <昵称>\n";
    }

    const std::string& group_name_raw = parts[1];
    std::string target_nickname_raw = parts[2];

    std::string group_name = to_lower_nickname(group_name_raw);
    std::string kicker_nickname = to_lower_nickname(kicker_nickname_raw);
    std::string target_nickname = to_lower_nickname(target_nickname_raw);

    std::lock_guard<std::mutex> lock(mtx);

    auto group_it = groups.find(group_name);
    if (group_it == groups.end())
    {
        return "错误：群组 '" + group_name_raw + "' 不存在。\n";
    }

    Group& group = group_it->second;

    if (group.owner_nickname != kicker_nickname)
    {
        return "错误：您不是群组 '" + group_name_raw + "' 的群主，无权转让所有权。\n";
    }

    if (kicker_nickname == target_nickname)
    {
        return "错误：您已经是群主了，无需转让给自己。\n";
    }

    if (group.members.find(target_nickname) == group.members.end())
    {
        return "错误：用户 '" + target_nickname_raw + "' 不是群组 '" + group_name_raw +
               "' 的成员。\n";
    }

    group.owner_nickname = target_nickname;

    std::string broadcast_msg = "【系统】群主 [" + kicker_nickname_raw + "] 已将群组 [" +
                                group_name_raw + "] 的所有权转让给了 [" +
                                target_nickname_raw + "]。\n";
    LOG_INFO(
        "群主 [" + kicker_nickname + "] 成功将群组 [" + group_name + "] 的所有权转让给 [" +
        target_nickname + "]");

    for (const auto& member_name_lower:group.members)
    {
        int member_fd=ctx_ref.get_fd_by_nickname(member_name_lower);

        if (member_fd!=-1)
        {
            message_sender(member_fd,broadcast_msg);
        }
    }

    return "成功将群组 '" + group_name_raw + "' 的所有权转让给了 [" + target_nickname_raw +
           "]。\n";
}