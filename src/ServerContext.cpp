//
// Created by X on 2025/9/24.
//
#include "../include/ServerContext.h"
#include <sys/epoll.h>
#include "../include/group_manager.h"
#include <iostream>
#include <unistd.h>
#include <sys/epoll.h>
#include "Logger.h"

ServerContext::ServerContext(ThreadPool& p, const  MessageSender& sender)
     :pool(p)
 {
    group_manager=std::make_unique<GroupManager>(sender,clients);
 }

std::string ServerContext::get_username(int fd)
 {
     {
         std::lock_guard<std::mutex>lock(clients_mtx);
         if (clients.count(fd))
         {
             return  clients.at(fd).nickname;
         }
         return "";
     }
 }

void ServerContext::set_username(int fd, const std::string& username)
 {
    std::lock_guard<std::mutex>lock(clients_mtx);
    auto it=clients.find(fd);
    if (it!=clients.end())
    {
        it->second.nickname=username;
        if (username!=this->admin_nickname)
        {
            it->second.is_admin=false;
        }
    }
 }

void ServerContext::remove_client(int fd)
 {
     {
         std::lock_guard<std::mutex>lock(clients_mtx);
         if (clients.count(fd))
         {
             clients.erase(fd);
         }
     }
 }

void ServerContext::broadcast(const std::string& msg, int sender_fd) {
    {
        std::lock_guard<std::mutex>lock(clients_mtx);
        for (const auto&pair:clients)
        {
            int client_fd=pair.first;
            if (client_fd!=-1&&client_fd!=sender_fd)
            {
                send_message_with_length(client_fd,msg);
            }
        }
    }
 }



bool ServerContext::kick_user_by_nickname(const std::string& target_nickname, const std::string& kicker_nickname)
{
    int target_fd=-1;

    {
        std::lock_guard<std::mutex>lock(clients_mtx);


        auto it=clients.begin();
        for (;it!=clients.end();++it)
        {
            if (it->second.nickname==target_nickname)
            {
                target_fd=it->first;
                break;
            }
        }
        if (target_fd==-1)
        {
            return false;
        }

        if (epoll_ctl(epoll_fd,EPOLL_CTL_DEL,target_fd,nullptr)==-1)
        {
            LOG_ERROR("踢人时，从 epoll 移除 fd 失败: " + std::to_string(target_fd));
        }

        if (it!=clients.end())
        {
            clients.erase(it);
        }else
        {
            LOG_ERROR("踢人时，从 clients 容器中移除记录失败，FD:" + std::to_string(target_fd));
        }

        if (close(target_fd)==-1)
        {
            LOG_ERROR("踢人时，关闭文件描述符失败: " + std::to_string(target_fd));
        }

        LOG_INFO("管理员指令：用户 [" + target_nickname + "] (FD: " + std::to_string(target_fd) + ") 已被清理并断开连接。");

    }

    std::string broadcast_msg="系统：用户 [" +target_nickname + "] 已被管理员 [" + kicker_nickname+"] 踢出聊天室。";
    this->broadcast(broadcast_msg,target_fd);

    return true;

}

bool ServerContext::is_user_admin(const std::string& nickname)
{
    std::lock_guard<std::mutex>lock(clients_mtx);
    for (const auto&pair:clients)
    {
        if (pair.second.nickname==nickname)
        {
            return  pair.second.is_admin;
        }
    }
    return false;
}
