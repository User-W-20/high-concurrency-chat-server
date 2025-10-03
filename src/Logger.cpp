//
// Created by X on 2025/10/3.
//
#include "../include/Logger.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

std::ofstream Logger::logFileStream_;
std::mutex Logger::mtx_;
LogLevel Logger::minLogLevel_=LogLevel::INFO;

std::string Logger::getTimeString()
{
    auto now=std::chrono::system_clock::now();
    auto tt=std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss<<std::put_time(std::localtime(&tt),"%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string Logger::getLevelString(LogLevel level)
{
    switch (level)
    {
        case LogLevel::DEBUG : return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

Logger::Logger()
{

}

Logger::~Logger()
{
    if (logFileStream_.is_open())
    {
        logFileStream_.close();
    }
}

void Logger::setLogFile(const std::string& filename)
{
    std::lock_guard<std::mutex>lock(mtx_);
    if (logFileStream_.is_open())
    {
        logFileStream_.close();
    }

    logFileStream_.open(filename,std::ios_base::app);
    if (!logFileStream_.is_open())
    {
        std::cerr<<"FATAL: Unable to open log file: " << filename << std::endl;
    }
}

void Logger::setMinLevel(LogLevel level)
{
    minLogLevel_=level;
}

void Logger::log(LogLevel level, const std::string& msg, const char* file, int line)
{
    if (level<minLogLevel_)
    {
        return;
    }

    std::lock_guard<std::mutex>lock(mtx_);

    std::string levelStr=getLevelString(level);
    std::string timeStr=getTimeString();

    std::stringstream logEntry;

    logEntry<<"["<<timeStr<<"]"
            <<"["<<levelStr<<"]"
    <<"["<<file<<":"<<line<<"]"
    <<" "<<msg;

    if (level>=LogLevel::ERROR)
    {
        std::cerr<<logEntry.str()<<std::endl;
    }else
    {
        std::cout<<logEntry.str()<<std::endl;
    }

    if (logFileStream_.is_open())
    {
        logFileStream_<<logEntry.str()<<"\n";
        logFileStream_.flush();
    }
}

Logger& Logger::getInstance()
{
    static Logger instance;
    return instance;
}
