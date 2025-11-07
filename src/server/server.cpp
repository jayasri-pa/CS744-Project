/*
 * server.cpp
 *
 * This file implements the HTTP-based KV server for the CS744 project.
 * It uses:
 * 1. cpp-httplib for the HTTP server
 * 2. A custom thread-safe LRUCache
 * 3. The MySQL C API for database persistence
 */

// 1. INCLUDES

// Now included from ../include/httplib.h
#include "httplib.h"

// Standard C++ libraries
#include <iostream>
#include <string>
#include <list>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <stdexcept>

// MySQL C API
#include <mysql/mysql.h>

// --- Database Configuration ---
// !! IMPORTANT: Change these values to match your MySQL setup !!
#define DB_HOST "127.0.0.1"
#define DB_USER "cs744_user"
#define DB_PASS "123"
#define DB_NAME "cs744_project"
#define CACHE_CAPACITY 100 // Max number of items in the LRU cache

// 2. THREAD-SAFE LRU CACHE IMPLEMENTATION

class LRUCache
{
private:
    size_t capacity;
    std::list<std::pair<std::string, std::string>> items_list;
    std::unordered_map<std::string, std::list<std::pair<std::string, std::string>>::iterator> items_map;
    std::mutex mtx;

public:
    LRUCache(size_t cap) : capacity(cap) {}

    bool get(const std::string &key, std::string &value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = items_map.find(key);
        if (it == items_map.end())
        {
            return false;
        }
        items_list.splice(items_list.begin(), items_list, it->second);
        value = it->second->second;
        return true;
    }

    void put(const std::string &key, const std::string &value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = items_map.find(key);
        if (it != items_map.end())
        {
            items_list.splice(items_list.begin(), items_list, it->second);
            it->second->second = value;
            return;
        }
        if (items_map.size() == capacity)
        {
            std::string lru_key = items_list.back().first;
            items_list.pop_back();
            items_map.erase(lru_key);
        }
        items_list.push_front({key, value});
        items_map[key] = items_list.begin();
    }

    void del(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = items_map.find(key);
        if (it != items_map.end())
        {
            items_list.erase(it->second);
            items_map.erase(it);
        }
    }
};

// 3. THREAD-SAFE DATABASE CONNECTION WRAPPER

class DBConnection
{
private:
    MYSQL *conn;
    std::mutex mtx;

    std::string escape(const std::string &s)
    {
        if (!conn)
            throw std::runtime_error("MySQL connection is null in escape()");
        size_t in_len = s.length();
        size_t out_len = in_len * 2 + 1;
        char *out = new char[out_len];
        unsigned long written = mysql_real_escape_string(conn, out, s.c_str(), in_len);
        std::string escaped_s(out, written); // use actual length
        delete[] out;
        return escaped_s;
    }

    bool ensure_connected()
    {
        if (!conn)
            return false;
        if (mysql_ping(conn) == 0)
        {
            // Connection is alive
            return true;
        }

        std::cerr << "MySQL connection lost, attempting reconnect..." << std::endl;
        mysql_close(conn);
        conn = mysql_init(NULL);
        if (conn == NULL)
        {
            std::cerr << "Re-init failed during reconnect." << std::endl;
            return false;
        }

        if (!mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASS, DB_NAME, 0, NULL, 0))
        {
            std::cerr << "Reconnect failed: " << mysql_error(conn) << std::endl;
            return false;
        }

        std::cout << "Reconnected to MySQL successfully." << std::endl;
        return true;
    }


public : 
    DBConnection()
    {
        conn = mysql_init(NULL);
        if (conn == NULL)
        {
            throw std::runtime_error("MySQL Init failed");
        }

        if (mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASS, DB_NAME, 0, NULL, 0) == NULL)
        {
            std::string error = "MySQL Connect failed: " + std::string(mysql_error(conn));
            mysql_close(conn);
            throw std::runtime_error(error);
        }
        std::cout << "Database connection successful." << std::endl;
    }

    ~DBConnection()
    {
        mysql_close(conn);
    }

    bool db_get(const std::string &key, std::string &value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!ensure_connected())
        {
            std::cerr << "DB connection not available for GET." << std::endl;
            return false;
        }

        std::string query = "SELECT val FROM kv_store WHERE id = '" + escape(key) + "';";

        if (mysql_query(conn, query.c_str()))
        {
            std::cerr << "DB GET query failed: " << mysql_error(conn) << std::endl;
            return false;
        }

        MYSQL_RES *result = mysql_store_result(conn);
        if (result == nullptr)
        {
            unsigned int err = mysql_errno(conn);
            if (err != 0)
            {
                std::cerr << "DB GET store_result failed: " << mysql_error(conn) << std::endl;
                return false;
            }
            // No rows found
            return false;
        }

        bool found = false;
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)))
        {
            if (row[0])
            {
                value = std::string(row[0]);
                found = true;
            }
        }

        mysql_free_result(result);
        return found;
    }

    bool db_put(const std::string &key, const std::string &value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!ensure_connected())
        {
            std::cerr << "DB connection not available for GET." << std::endl;
            return false;
        }
        std::string query = "INSERT INTO kv_store (id, val) VALUES ('" +
                            escape(key) + "', '" + escape(value) +
                            "') ON DUPLICATE KEY UPDATE val = '" + escape(value) + "';";

        if (mysql_query(conn, query.c_str()))
        {
            std::cerr << "DB PUT query failed: " << mysql_error(conn) << std::endl;
            return false;
        }
        return true;
    }

    bool db_del(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!ensure_connected())
        {
            std::cerr << "DB connection not available for GET." << std::endl;
            return false;
        }
        std::string query = "DELETE FROM kv_store WHERE id = '" + escape(key) + "';";

        if (mysql_query(conn, query.c_str()))
        {
            std::cerr << "DB DELETE query failed: " << mysql_error(conn) << std::endl;
            return false;
        }
        return true;
    }
}
;

// 4. MAIN SERVER LOGIC

int main()
{
    using namespace httplib;

    try
    {
        Server svr;
        LRUCache cache(CACHE_CAPACITY);
        DBConnection db;

        svr.Post("/kv", [&](const Request &req, Response &res)
                 {
            if (!req.has_param("key") || !req.has_param("val")) {
                res.status = 400; 
                res.set_content("Missing 'key' or 'val' parameter", "text/plain");
                return;
            }
            std::string key = req.get_param_value("key");
            std::string val = req.get_param_value("val");

            if (!db.db_put(key, val)) {
                res.status = 500; 
                res.set_content("Database write failed", "text/plain");
                return;
            }
            cache.put(key, val);

            res.status = 201; 
            res.set_content("Created", "text/plain"); });

        svr.Get(R"(/kv/([a-zA-Z0-9_-]+))", [&](const Request &req, Response &res)
                {
            std::string key = req.matches[1];
            std::string value;

            if (cache.get(key, value)) {
                res.set_header("X-Cache-Status", "HIT");
                res.set_content(value, "text/plain");
                return;
            }

            if (db.db_get(key, value)) {
                cache.put(key, value);
                res.set_header("X-Cache-Status", "MISS");
                res.set_content(value, "text/plain");
            } else {
                res.status = 404; 
                res.set_content("Key not found", "text/plain");
            } });

        svr.Delete(R"(/kv/([a-zA-Z0-9_-]+))", [&](const Request &req, Response &res)
                   {
            std::string key = req.matches[1];
            if (!db.db_del(key)) {
                res.status = 500;
                res.set_content("Database delete failed", "text/plain");
                return;
            }
            cache.del(key);
            res.set_content("Deleted", "text/plain"); });

        std::cout << "Starting server on http://0.0.0.0:8080..." << std::endl;
        svr.listen("0.0.0.0", 8080);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}