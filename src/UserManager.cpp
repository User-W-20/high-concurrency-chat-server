//
// Created by X on 2025/10/4.
//
#include "../include/UserManager.h"
#include "../include/Logger.h"
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <argon2.h>
#include <random>
#include <cctype>
#include <string>
#include "../include/json.hpp"
const std::string USER_FILE = "user_data.json";

bool UserManager::verify_password(const std::string& password,
                                  const std::string& stored_hash)
{
    int result = argon2_verify(
        stored_hash.c_str(),
        password.c_str(),
        password.length(),
        Argon2_id);

    if (result != ARGON2_OK && result != ARGON2_VERIFY_MISMATCH)
    {
        LOG_DEBUG("Argon2 验证过程中发生错误，错误码: " << result);
    }

    return result == ARGON2_OK;
}


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

    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> dist(0, 255);

    for (size_t i = 0; i < SALT_LEN; ++i)
    {
        salt[i] = static_cast<unsigned char>(dist(generator));
    }

    size_t encoded_len = argon2_encodedlen(T_COST, M_COST, P_COST, SALT_LEN,
                                           HASH_LEN, Argon2_id);

    std::unique_ptr<char []> encoded_hash = std::make_unique<char []>(
        encoded_len);

    int result = argon2id_hash_encoded(T_COST, M_COST, P_COST,
                                       password.c_str(), password.length(),
                                       salt, SALT_LEN,
                                       HASH_LEN,
                                       encoded_hash.get(), encoded_len);

    if (result == ARGON2_OK)
    {
        out_encoded_hash = std::string(encoded_hash.get());
        return true;
    }
    else
    {
        LOG_ERROR("Argon2 哈希失败，错误码: " << result);
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
    std::string nickname_lower = to_lower_nickname(nickname);
    std::lock_guard<std::mutex> lock(mtx_);
    return registered_users_.count(nickname_lower);
}

bool UserManager::add_user_to_memory(const std::string& username_raw,
                                     const std::string& password_hash,
                                     bool is_admin)
{
    std::string username_lower = to_lower_nickname(username_raw);

    std::lock_guard<std::mutex> lock(mtx_);

    if (registered_users_.count(username_lower))
    {
        return false;
    }

    User newUser;
    newUser.nickname = username_raw;
    newUser.argon2_hash = password_hash;
    newUser.is_admin = is_admin;

    registered_users_.emplace(username_lower, newUser);

    LOG_INFO("用户数据同步到内存缓存: " +username_raw);
    return true;
}

bool UserManager::is_user_in_memory(const std::string& username_lower) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return registered_users_.count(username_lower);
}

std::string UserManager::to_lower_nickname(const std::string& nickname)
{
    std::string lower_nickname = nickname;
    std::transform(lower_nickname.begin(), lower_nickname.end(),
                   lower_nickname.begin(), [](unsigned char c)
                   {
                       return std::tolower(c);
                   });

    return lower_nickname;
}