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
#include <string>
#include <algorithm>

class UserManager
{
public:
    UserManager();

    void load_users_from_file(const std::string& filename);
    void save_users_to_file(const std::string& filename) const;

   static std::string to_lower_nickname(const std::string& nickname) ;

    bool add_user_to_memory(const std::string& username_raw,
                            const std::string& password_hash,
                            bool is_admin);

    bool is_user_in_memory(const std::string& username_lower) const;

    const User* get_user(const std::string& nickname) const;

    bool is_user_register(const std::string& nickname) const;

    static bool hash_password(const std::string& password,
                              std::string& out_encoded_hash);

    static bool verify_password(const std::string& password,
                                const std::string& stored_hash);

private:
    std::unordered_map<std::string, User> registered_users_;

    mutable std::mutex mtx_;
};

#endif //LITECHAT_USERMANAGER_H