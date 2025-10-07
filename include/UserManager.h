//
// Created by X on 2025/10/4.
//

#ifndef LITECHAT_USERMANAGER_H
#define LITECHAT_USERMANAGER_H
#include "User.h"
#include "Logger.h"
#include <unordered_map>
#include <mutex>
#include <random>

class UserManager
{
public:
    UserManager();

    void load_users_from_file(const std::string& filename);
    void save_users_to_file(const std::string& filename) const;

    bool register_user(const std::string& nickname,
                       const std::string& password);
    bool validate_login(const std::string& nickname,
                        const std::string& password);

    const User* get_user(const std::string& nickname) const;
    bool is_user_register(const std::string& nickname) const;

    static bool hash_password(const std::string& password,
                             std::string& out_encoded_hash);

    static bool verify_password(const std::string&hash,const std::string&password);

private:
    std::unordered_map<std::string, User> registered_users_;

    mutable std::mutex mtx_;


};

#endif //LITECHAT_USERMANAGER_H