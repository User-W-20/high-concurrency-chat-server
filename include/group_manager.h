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
#include "json.hpp"

struct ServerContext;

using MessageSender = std::function<void(int, const std::string&)>;

using json = nlohmann::json;

inline const std::string JSON_FILE = "groups_data.json";

struct Group
{
    std::string name;

    std::string owner_nickname;

    std::unordered_set<std::string> members;

    std::string password_hash;
};

inline void to_json(json& j, const Group& g)
{
    j = json{
        {"name", g.name},
        {"owner", g.owner_nickname},
        {"members", g.members},
        {"password_hash",g.password_hash}
    };
}

inline void from_json(const json& j, Group& g)
{
    j.at("name").get_to(g.name);
    j.at("owner").get_to(g.owner_nickname);
    j.at("members").get_to(g.members);

    if (j.count("password_hash"))
    {
        j.at("password_hash").get_to(g.password_hash);
    }else
    {
        g.password_hash="";
    }
}

class GroupManager
{
public:
    explicit GroupManager(MessageSender sender, const ServerContext& ctx_ref);
    ~GroupManager() = default;

    std::string handle_create_group(const std::string& username,
                                    const std::vector<std::string>& parts);
    std::string handle_join_group(const std::string& username,
                                  const std::vector<std::string>& parts);
    std::string handle_send_message(const std::string& username,
                                    const std::vector<std::string>& parts);
    std::string handle_list_groups() const;
    static void remove_client_from_groups(const std::string& username);
    std::string handle_group_kick(const std::string& kicker_nickname,
                                  const std::vector<std::string>& parts);
    std::string handle_group_leave(const std::string& username,
                                   const std::vector<std::string>& parts);

    void load_groups_from_file(const std::string& filename);
    void save_groups_to_file(const std::string& filename) const;

private:
    std::unordered_map<std::string, Group> groups;
    mutable std::mutex mtx;

    MessageSender message_sender;

    const ServerContext& ctx_ref;

    static std::vector<std::string> split(const std::string& s, char delimiter);
};

#endif  // LITECHAT_GROUP_MANAGER_H