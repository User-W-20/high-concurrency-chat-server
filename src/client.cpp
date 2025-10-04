//
// Created by X on 2025/9/15.
//
#include <arpa/inet.h>
#include <csignal>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
constexpr int PORT = 5008;
constexpr int BUF_SIZE = 4096;

void recv_thread_func(int sock)
{
    while (true)
    {
        uint32_t net_len;
        char* net_len_ptr = reinterpret_cast<char*>(&net_len);
        size_t bytes_received_len = 0;

        while (bytes_received_len < sizeof(net_len))
        {
            ssize_t n = recv(sock, net_len_ptr + bytes_received_len,
                             sizeof(net_len) - bytes_received_len, 0);

            if (n <= 0)
            {
                if (n == 0)
                {
                    write(STDOUT_FILENO, "服务器已关闭连接\n", strlen("服务器已关闭连接\n"));
                }
                else
                {
                    write(STDOUT_FILENO, "服务器已断开连接\n", strlen("服务器已断开连接\n"));
                }
                return;
            }
            bytes_received_len += n;
        }

        uint32_t msg_len = ntohl(net_len);

        std::vector<char> buf(msg_len);
        size_t bytes_received = 0;

        while (bytes_received < msg_len)
        {
            ssize_t received_now = recv(sock, buf.data() + bytes_received,
                                        msg_len - bytes_received, 0);

            if (received_now <= 0)
            {
                write(STDOUT_FILENO, "服务器已断开连接或接收出错\n",
                      strlen("服务器已断开连接或接收出错\n"));
                return;
            }
            bytes_received += received_now;
        }
        write(STDOUT_FILENO, buf.data(), msg_len);
        write(STDOUT_FILENO, "\n", 1);
        fflush(stdout);
    }
}

int main()
{
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1)
    {
        perror("socket");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == -1)
    {
        perror("connect");
        close(sock);
        return -1;
    }

    std::thread recv_t(recv_thread_func, sock);

    std::string line;

    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line == "quit")
        {
            uint32_t msg_len = htonl(line.size());
            std::string full_msg(reinterpret_cast<const char*>(&msg_len),
                                 sizeof(msg_len));
            full_msg += line;
            send(sock, full_msg.data(), full_msg.size(), 0);
            break;
        }

        uint32_t msg_len = htonl(line.size());

        std::string full_msg(reinterpret_cast<const char*>(&msg_len),
                             sizeof(msg_len));
        full_msg += line;

        ssize_t s = send(sock, full_msg.data(), full_msg.size(), 0);
        if (s == -1)
        {
            perror("send");
            break;
        }
    }

    close(sock);
    if (recv_t.joinable())
        recv_t.join();

    return 0;
}