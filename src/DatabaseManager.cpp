//
// Created by X on 2025/10/31.
//
#include "../include/DatabaseManager.h"
#include "../include/Logger.h"

DatabaseManager& DatabaseManager::getInstance()
{
    static DatabaseManager instance;
    return instance;
}

bool DatabaseManager::connect(
    const std::map<std::string, std::string>& env_vars)
{
    if (env_vars.find("DB_HOST") == env_vars.end() ||
        env_vars.find("DB_USER") == env_vars.end() ||
        env_vars.find("DB_PASSWORD") == env_vars.end() ||
        env_vars.find("DB_NAME") == env_vars.end())
    {
        LOG_ERROR("Connection failed: Missing DB environment variables.");
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> lock(mtx);

        driver = get_driver_instance();

        std::string host_post = env_vars.at("DB_HOST") + ":" + (
                                    env_vars.count("DB_PORT")
                                        ? env_vars.at("DB_PORT")
                                        : std::to_string(3307));

        conn.reset(driver->connect(host_post, env_vars.at("DB_USER"),
                                   env_vars.at("DB_PASSWORD")));

        conn->setSchema(env_vars.at("DB_NAME"));

        LOG_INFO("Successfully connected to MySQL database.");
        return true;
    }
    catch (sql::SQLException& e)
    {
        LOG_ERROR("MySQL Connection Error: " +std::string(e.what()));
        return false;
    }
}

void DatabaseManager::disconnect()
{
    std::lock_guard<std::mutex> lock(mtx);

    if (conn)
    {
        conn.reset();
        LOG_INFO("Disconnected from MySQL database.");
    }
}

bool DatabaseManager::register_user(const std::string& username,
                                    const std::string& username_lower,
                                    const std::string& password_hash)
{
    if (!conn)
    {
        LOG_ERROR("Attempted to register user, but database is not connected.");
        return false;
    }

    std::unique_ptr<sql::PreparedStatement> pstmt;

    std::lock_guard<std::mutex> lock(mtx);

    try
    {
        pstmt.reset(conn->prepareStatement(
            "INSERT INTO users (username, username_lower, password_hash) VALUES (?, ?, ?)"));

        pstmt->setString(1, username);
        pstmt->setString(2, username_lower);
        pstmt->setString(3, password_hash);

        pstmt->executeUpdate();
        LOG_INFO("New user registered: " +username);

        return true;
    }
    catch (sql::SQLException& e)
    {
        if (e.getErrorCode() == 1062)
        {
            LOG_WARNING(
                "User registration failed: Username '"+username+
                "' already exists.");
        }
        else
        {
            LOG_ERROR(
                "Database error during registration: "+std::string(e.what()));
        }
        return false;
    }
}

bool DatabaseManager::get_user_data(const std::string& username_lower,
                                    std::string& out_username_raw,
                                    std::string& out_password_hash,
                                    bool& out_is_admin)
{
    std::lock_guard<std::mutex> lock(mtx);

    if (!conn || conn->isClosed())
    {
        LOG_ERROR("数据库连接无效或已关闭。");
        return false;
    }

    try
    {
        const std::string sql =
            "SELECT username, password_hash, is_admin "
            "FROM users WHERE username_lower = ?";

        std::unique_ptr<sql::PreparedStatement> pstmt(
            conn->prepareStatement(sql));

        pstmt->setString(1, username_lower);

        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

        if (res->next())
        {
            out_username_raw = res->getString("username");
            out_password_hash = res->getString("password_hash");

            out_is_admin = res->getBoolean("is_admin");

            LOG_DEBUG("成功从数据库获取用户数据: "<<out_username_raw);
            return true;
        }
        else
        {
            LOG_DEBUG("数据库中未找到小写昵称为 '" << username_lower<<"' 的用户。");
            return false;
        }
    }
    catch (sql::SQLException& e)
    {
        LOG_ERROR(
            "数据库操作失败 (get_user_data): "<<"#ERR: "<<e.what()<<
            " (MySQL error code: "<<e.getErrorCode()<<", SQLState: " <<e.
            getSQLState()<<")");

        return false;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("意外错误 (get_user_data): "<<e.what());
        return false;
    }
}