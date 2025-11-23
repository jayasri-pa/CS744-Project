// db.hpp
#ifndef CS744_DB_HPP
#define CS744_DB_HPP

#include <mysql/mysql.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class MySQLPool {
public:
    // Construct: host, user, pass, dbname, pool_size
    MySQLPool(const char *host,
              const char *user,
              const char *pass,
              const char *db,
              int pool_size);
    ~MySQLPool();

    // Acquire a connection: blocks until available. Returns (index, MYSQL*)
    std::pair<int, MYSQL*> acquire();

    // Release previously acquired index
    void release(int idx);

    // Number of connections currently in use
    int in_use();

    // Instrumentation counters (public for /metrics access)
    std::atomic<long long> db_pool_acquire_wait_us{0};
    std::atomic<long long> db_pool_acquire_count{0};
    std::atomic<long long> db_pool_acquire_wait_max_us{0};

private:
    int pool_size_;
    std::vector<MYSQL*> pool_;
    std::vector<bool> available_;
    std::mutex mtx_;
    std::condition_variable wait_cv_;
};

// Low-level DB ops (prepared statements). Return true on success.
// For db_get_raw, out_val will be filled when return==true.
bool db_get_raw(MYSQL *conn, const std::string &key, std::string &out_val);
bool db_put_raw(MYSQL *conn, const std::string &key, const std::string &val);
bool db_del_raw(MYSQL *conn, const std::string &key);

#endif // CS744_DB_HPP
