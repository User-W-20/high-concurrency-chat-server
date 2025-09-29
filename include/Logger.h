//
// Created by X on 2025/9/19.
//

#ifndef LITECHAT_LOGGER_H
#define LITECHAT_LOGGER_H

#include <iostream>
#include <mutex>
#include <string>
#include <sstream>

enum class LogLevel
{
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

class Logger
{
public:
    static Logger& getInstance()
    {
        static Logger instance;
        return instance;
    }

    void log(LogLevel level, const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        std::string levelStr = getLevelString(level);

        std::cout << "[" << levelStr << "]" << msg << std::endl;
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger()
    {
    }

    std::mutex mtx_;

    static std::string getLevelString(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::DEBUG:
                return "DEBUG";
            case LogLevel::INFO:
                return "INFO";
            case LogLevel::ERROR:
                return "ERROR";
            case LogLevel::WARNING:
                return "WARNING";
            case LogLevel::FATAL:
                return "FATAL";
            default:
                return "UNKNOWN";
        }
    }
};

#define  LOG_MESSAGE(level,...)\
    do{\
        std::stringstream ss;\
        ss<<__VA_ARGS__;\
        Logger::getInstance().log(level,ss.str());\
    }while (0)

#define  LOG_DEBUG(...) LOG_MESSAGE(LogLevel::DEBUG,__VA_ARGS__)
#define LOG_INFO(...) LOG_MESSAGE(LogLevel::INFO, __VA_ARGS__)
#define LOG_WARNING(...) LOG_MESSAGE(LogLevel::WARNING, __VA_ARGS__)
#define LOG_ERROR(...) LOG_MESSAGE(LogLevel::ERROR, __VA_ARGS__)
#define LOG_FATAL(...) LOG_MESSAGE(LogLevel::FATAL, __VA_ARGS__)

#endif  // LITECHAT_LOGGER_H