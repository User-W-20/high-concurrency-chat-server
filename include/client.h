//
// Created by X on 2025/9/19.
//

#ifndef LITECHAT_CLIENT_H
#define LITECHAT_CLIENT_H

#include <chrono>
#include <string>

class Client
{
public:
    int fd;
    std::string nickname;
    std::string ip;

    bool is_admin = false;
    std::chrono::steady_clock::time_point last_activity;

    Client(int file_descriptor, std::string client_ip)
        : fd(file_descriptor), ip(std::move(client_ip))
    {
        last_activity = std::chrono::steady_clock::now();
    }

    Client() : fd(-1), is_admin(false)
    {
    }

    Client(const Client&) = default;
    Client& operator=(const Client&) = default;
};

#endif  // LITECHAT_CLIENT_H