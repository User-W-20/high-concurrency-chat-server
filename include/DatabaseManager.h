//
// Created by X on 2025/10/31.
//

#ifndef LITECHAT_DATABASEMANAGER_H
#define LITECHAT_DATABASEMANAGER_H
#include <mysql_connection.h>
#include <mysql_driver.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>

#include <memory>
#include <string>
#include <map>
#include <mutex>

class DatabaseManager
{
private:
    sql::Driver* driver;
    std::unique_ptr<sql::Connection> conn;
    std::mutex mtx;

    DatabaseManager() : driver(nullptr)
    {
    }

public:
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    static DatabaseManager& getInstance();

    bool connect(const std::map<std::string, std::string>& env_vars);

    void disconnect();

    bool register_user(const std::string& username,
                       const std::string& username_lower,
                       const std::string& password_hash);

    bool get_user_data(const std::string& username_lower,
                       std::string& out_username_raw,
                       std::string& out_password_hash,
                       bool& out_is_admin);
};
#endif //LITECHAT_DATABASEMANAGER_H