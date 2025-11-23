// db.cpp
#include "db.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <algorithm>

// Internal alias for clock
using Clock = std::chrono::steady_clock;

namespace {
    // safe prepare wrapper
    inline bool mysql_stmt_safe_prepare(MYSQL * /*conn*/, MYSQL_STMT *stmt, const char *q, unsigned long qlen) {
        if (mysql_stmt_prepare(stmt, q, qlen) != 0) return false;
        return true;
    }
}

MySQLPool::MySQLPool(const char *host,
                     const char *user,
                     const char *pass,
                     const char *db,
                     int pool_size)
    : pool_size_(pool_size)
{
    if (pool_size_ <= 0) pool_size_ = 1;
    pool_.reserve(pool_size_);
    available_.assign(pool_size_, false);

    for (int i = 0; i < pool_size_; ++i) {
        MYSQL *c = mysql_init(nullptr);
        if (!c) throw std::runtime_error("mysql_init failed");
        if (!mysql_real_connect(c, host, user, pass, db, 0, NULL, 0)) {
            std::string err = mysql_error(c);
            mysql_close(c);
            throw std::runtime_error("MySQL connect failed: " + err);
        }
        pool_.push_back(c);
        available_[i] = true;
    }
}

MySQLPool::~MySQLPool() {
    for (auto c : pool_) if (c) mysql_close(c);
}

std::pair<int, MYSQL*> MySQLPool::acquire() {
    auto t0 = Clock::now();
    std::unique_lock<std::mutex> lk(mtx_);
    wait_cv_.wait(lk, [this] {
        return std::any_of(available_.begin(), available_.end(), [](bool v){ return v; });
    });
    int idx = -1;
    for (int i = 0; i < pool_size_; ++i) {
        if (available_[i]) { available_[i] = false; idx = i; break; }
    }
    auto t1 = Clock::now();
    long long wait_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    db_pool_acquire_count++;
    db_pool_acquire_wait_us.fetch_add(wait_us);
    long long prev_max = db_pool_acquire_wait_max_us.load();
    while (wait_us > prev_max && !db_pool_acquire_wait_max_us.compare_exchange_weak(prev_max, wait_us)) {
        // keep trying to update max
    }
    return {idx, pool_[idx]};
}

void MySQLPool::release(int idx) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (idx >= 0 && idx < pool_size_) available_[idx] = true;
    }
    wait_cv_.notify_one();
}

int MySQLPool::in_use() {
    std::lock_guard<std::mutex> lk(mtx_);
    int used = 0;
    for (int i = 0; i < pool_size_; ++i) if (!available_[i]) ++used;
    return used;
}


// -------- Low-level DB ops (prepared statement on the fly) ----------
bool db_get_raw(MYSQL *conn, const std::string &key, std::string &out_val) {
    const char *q = "SELECT val FROM kv_store WHERE id = ?";
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    if (!stmt) return false;
    if (!mysql_stmt_safe_prepare(conn, stmt, q, strlen(q))) { mysql_stmt_close(stmt); return false; }

    MYSQL_BIND param{};
    memset(&param, 0, sizeof(param));
    param.buffer_type = MYSQL_TYPE_STRING;
    param.buffer = (void*)key.data();
    param.buffer_length = key.size();
    if (mysql_stmt_bind_param(stmt, &param) != 0) { mysql_stmt_close(stmt); return false; }

    // result bind - 8KB buffer (increase if you need larger values)
    char buf[8192];
    unsigned long length = 0;
    MYSQL_BIND result{};
    memset(&result, 0, sizeof(result));
    result.buffer_type = MYSQL_TYPE_STRING;
    result.buffer = buf;
    result.buffer_length = sizeof(buf);
    result.length = &length;

    if (mysql_stmt_bind_result(stmt, &result) != 0) { mysql_stmt_close(stmt); return false; }

    if (mysql_stmt_execute(stmt) != 0) { mysql_stmt_close(stmt); return false; }

    int rc = mysql_stmt_fetch(stmt);
    if (rc == 0 || rc == MYSQL_DATA_TRUNCATED) {
        out_val.assign(buf, length);
        mysql_stmt_close(stmt);
        return true;
    } else {
        mysql_stmt_close(stmt);
        return false;
    }
}

bool db_put_raw(MYSQL *conn, const std::string &key, const std::string &val) {
    const char *q = "INSERT INTO kv_store (id,val) VALUES (?,?) ON DUPLICATE KEY UPDATE val=VALUES(val)";
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    if (!stmt) return false;
    if (!mysql_stmt_safe_prepare(conn, stmt, q, strlen(q))) { mysql_stmt_close(stmt); return false; }

    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)key.data();
    bind[0].buffer_length = key.size();
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void*)val.data();
    bind[1].buffer_length = val.size();

    if (mysql_stmt_bind_param(stmt, bind) != 0) { mysql_stmt_close(stmt); return false; }
    bool ok = (mysql_stmt_execute(stmt) == 0);
    mysql_stmt_close(stmt);
    return ok;
}

bool db_del_raw(MYSQL *conn, const std::string &key) {
    const char *q = "DELETE FROM kv_store WHERE id = ?";
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    if (!stmt) return false;
    if (!mysql_stmt_safe_prepare(conn, stmt, q, strlen(q))) { mysql_stmt_close(stmt); return false; }
    MYSQL_BIND param{};
    memset(&param, 0, sizeof(param));
    param.buffer_type = MYSQL_TYPE_STRING;
    param.buffer = (void*)key.data();
    param.buffer_length = key.size();
    if (mysql_stmt_bind_param(stmt, &param) != 0) { mysql_stmt_close(stmt); return false; }
    bool ok = (mysql_stmt_execute(stmt) == 0);
    mysql_stmt_close(stmt);
    return ok;
}
