//
// Created by X on 2025/10/4.
//
#include "../include/UserManager.h"
#include "../include/Logger.h"
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <argon2.h>
#include <random>
const std::string USER_FILE = "user_data.json";

bool UserManager::hash_password(const std::string& password,
                                std::string& out_encoded_hash)
{
    static constexpr size_t HASH_LEN = 32;
    static constexpr size_t SALT_LEN = 16;
    static constexpr uint32_t T_COST = 3;
    static constexpr uint32_t M_COST = 65536;
    static constexpr uint32_t P_COST = 1;

    unsigned char salt[SALT_LEN];

    std::random_device rd;

    std::uniform_int_distribution<int> dist(0, 255);

    for (size_t i = 0; i < SALT_LEN; ++i)
    {
        salt[i] = static_cast<unsigned char>(dist(rd));
    }

    size_t encoded_len = argon2_encodedlen(T_COST, M_COST, P_COST, SALT_LEN,
                                           HASH_LEN, Argon2_id);

    char* encoded_hash = (char*)malloc(encoded_len);

    int result = argon2id_hash_encoded(T_COST, M_COST, P_COST,
                                       password.c_str(), password.length(),
                                       salt, SALT_LEN,
                                       HASH_LEN,
                                       encoded_hash, encoded_len);

    if (result == ARGON2_OK)
    {
        out_encoded_hash = std::string(encoded_hash);
        free(encoded_hash);
        return true;
    }
    else
    {
        LOG_ERROR("Argon2 哈希失败，错误码: " << result);
        free(encoded_hash);
        return false;
    }
}

UserManager::UserManager()
{

}

void UserManager::load_users_from_file(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::ifstream i(filename);

    if (!i.is_open())
    {
        LOG_WARNING("未找到用户数据文件 (" <<filename<< ")，以空用户列表启动。");
        registered_users_.clear();
        return;
    }
    try
    {
        nlohmann::json j;
        i >> j;

        if (j.is_null() || !j.is_object())
        {
            if (j.is_null())
            {
                LOG_WARNING("用户数据文件内容为空或为 'null'。以空用户列表启动。");
            }
            else
            {
                throw std::runtime_error("文件格式错误，期望一个 JSON Object {}。");
            }
            registered_users_.clear();
        }
        else
        {
            registered_users_ = j.get<std::unordered_map<std::string, User>>();
            LOG_INFO("成功从文件加载 " << registered_users_.size() << " 个用户数据。");
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        LOG_ERROR("加载用户数据失败，JSON 解析或数据结构错误: " << e.what());
        registered_users_.clear();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("加载用户数据失败，JSON 解析或数据结构错误: " <<e.what());
        registered_users_.clear();
    }
}

void UserManager::save_users_to_file(const std::string& filename) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::ofstream o(filename);

    if (!o.is_open())
    {
        LOG_ERROR("无法打开文件进行写入: " << filename);
        return;
    }

    try
    {
        nlohmann::json j;
        j = registered_users_;
        o << std::setw(4) << j << std::endl;

        if (o.fail())
        {
            LOG_ERROR("写入文件时发生流错误: " << filename);
            return;
        }

        LOG_INFO("用户数据成功保存到: " << filename);
    }
    catch (const nlohmann::json::exception& e)
    {
        LOG_ERROR("保存用户数据失败 (JSON 序列化错误): " << e.what());
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("保存用户数据失败: " << e.what());
    }
}

bool UserManager::register_user(const std::string& nickname,
                                const std::string& password)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (registered_users_.count(nickname))
    {
        return false;
    }

    User newUser;
    std::string encoded_hash;

    if (!hash_password(password, encoded_hash))
    {
        return false;
    }

    newUser.nickname = nickname;
    newUser.argon2_hash = encoded_hash;

    if (nickname == "admin" && registered_users_.empty())
    {
        newUser.is_admin = true;
    }

    registered_users_[nickname] = newUser;
    LOG_INFO("新用户注册: " << nickname <<(newUser.is_admin?" (管理员)" : ""));
    return true;
}

bool UserManager::validate_login(const std::string& nickname,
                                 const std::string& password)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = registered_users_.find(nickname);

    if (it == registered_users_.end())
    {
        return false;
    }

    const User& storeUser = it->second;

    int result = argon2_verify(
        storeUser.argon2_hash.c_str(),
        password.c_str(),
        password.length(),
        Argon2_id);

    if (result == ARGON2_OK)
    {
        return true;
    }
    else if (result == ARGON2_VERIFY_MISMATCH)
    {
        return false;
    }
    else
    {
        LOG_ERROR("Argon2 验证过程中发生错误，错误码: " << result);
        return false;
    }
}

const User* UserManager::get_user(const std::string& nickname) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = registered_users_.find(nickname);

    if (it != registered_users_.end())
    {
        return &it->second;
    }

    return nullptr;
}

bool UserManager::is_user_register(const std::string& nickname) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return registered_users_.count(nickname);
}