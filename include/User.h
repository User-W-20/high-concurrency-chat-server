//
// Created by X on 2025/10/4.
//

#ifndef LITECHAT_USER_H
#define LITECHAT_USER_H
#include <string>
#include "json.hpp"

struct User
{
    std::string nickname;

    std::string argon2_hash;

    bool is_admin = false;
};

inline void to_json(nlohmann::json& j, const User& u)
{
    j = nlohmann::json{
        {"nickname", u.nickname},
        {"argon2_hash", u.argon2_hash},
        {"is_admin", u.is_admin}
    };
}

inline void from_json(const nlohmann::json& j, User& u)
{
    if (j.contains("nickname"))j.at("nickname").get_to(u.nickname);
    if (j.contains("argon2_hash"))j.at("argon2_hash").get_to(u.argon2_hash);
    if (j.contains("is_admin"))j.at("is_admin").get_to(u.is_admin);
}

#endif //LITECHAT_USER_H