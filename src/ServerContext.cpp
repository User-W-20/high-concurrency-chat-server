//
// Created by X on 2025/9/24.
//
#include "../include/ServerContext.h"
#include "../include/group_manager.h"
#include <iostream>
 ServerContext::ServerContext(ThreadPool& p, MessageSender sender)
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
        if (fd<0)return;
         std::lock_guard<std::mutex>lock(clients_mtx);
         if (clients.count(fd))
         {
             clients.at(fd).nickname=username;
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
