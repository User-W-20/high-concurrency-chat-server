//
// Created by X on 2025/9/19.
//

#ifndef LITECHAT_LOGGER_H
#define LITECHAT_LOGGER_H

#include <iostream>
#include <string>
#include <mutex>

class Logger {
public:
    static  Logger & getInstance() {
        static Logger instance;
        return instance;
    }

    void log(const std::string&msg) {
        std::lock_guard<std::mutex>lock(mtx_);
        std::cout<<msg<<std::flush;
    }

    Logger(const Logger&)=delete;
    Logger& operator=(const Logger&)=delete;
private:
    Logger(){}
    std::mutex mtx_;
};

#endif //LITECHAT_LOGGER_H