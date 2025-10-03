//
// Created by X on 2025/9/19.
//

#ifndef LITECHAT_LOGGER_H
#define LITECHAT_LOGGER_H

#include <iostream>
#include <mutex>
#include <string>
#include <sstream>
#include <fstream>

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
    static Logger& getInstance();

    static void setLogFile(const std::string& filename);
    static void setMinLevel(LogLevel level);
    void log(LogLevel level, const std::string& msg, const char* file,
             int line);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger();
    ~Logger();

    static std::ofstream logFileStream_;
    static std::mutex mtx_;
    static LogLevel minLogLevel_;

    static std::string getTimeString();
    static std::string getLevelString(LogLevel level);
};

#define  LOG_MESSAGE(level,...)\
    do{\
        std::stringstream ss;\
        ss<<__VA_ARGS__;\
        Logger::getInstance().log(level,ss.str(),__FILE__,__LINE__);\
    }while (0)

#define  LOG_DEBUG(...) LOG_MESSAGE(LogLevel::DEBUG,__VA_ARGS__)
#define LOG_INFO(...) LOG_MESSAGE(LogLevel::INFO, __VA_ARGS__)
#define LOG_WARNING(...) LOG_MESSAGE(LogLevel::WARNING, __VA_ARGS__)
#define LOG_ERROR(...) LOG_MESSAGE(LogLevel::ERROR, __VA_ARGS__)
#define LOG_FATAL(...) LOG_MESSAGE(LogLevel::FATAL, __VA_ARGS__)

#endif  // LITECHAT_LOGGER_H