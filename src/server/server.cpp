// server.cpp
// Option A - Thread-pool HTTP KV Server with Sharded LRU and MySQL pool
// Build: g++ -std=c++17 server.cpp db.cpp -o server -lmysqlclient -lpthread

#include "httplib.h"
#include "db.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// --------------------------- Configuration -----------------------------------
static int PORT = std::getenv("CS744_PORT") ? std::stoi(std::getenv("CS744_PORT")) : 8080;
static int SHARD_COUNT = std::getenv("CS744_CACHE_SHARDS") ? std::stoi(std::getenv("CS744_CACHE_SHARDS")) : 16;
static int CACHE_CAPACITY = std::getenv("CS744_CACHE") ? std::stoi(std::getenv("CS744_CACHE")) : 2000; // total items
static int DB_POOL_SIZE = std::getenv("CS744_DBPOOL") ? std::stoi(std::getenv("CS744_DBPOOL")) : 8;
static int WORKER_THREADS = std::getenv("CS744_WORKERS") ? std::stoi(std::getenv("CS744_WORKERS")) : std::thread::hardware_concurrency();
static const char *DB_HOST = std::getenv("CS744_DB_HOST") ? std::getenv("CS744_DB_HOST") : "127.0.0.1";
static const char *DB_USER = std::getenv("CS744_DB_USER") ? std::getenv("CS744_DB_USER") : "cs744_user";
static const char *DB_PASS = std::getenv("CS744_DB_PASS") ? std::getenv("CS744_DB_PASS") : "123";
static const char *DB_NAME = std::getenv("CS744_DB_NAME") ? std::getenv("CS744_DB_NAME") : "cs744_project";

// --------------------------- Utilities --------------------------------------
using Clock = std::chrono::steady_clock;
static inline long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

// --------------------------- Thread Pool ------------------------------------
class ThreadPool {
public:
    ThreadPool(size_t n) : stop_flag(false) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv.wait(lk, [this] { return stop_flag || !tasks.empty(); });
                        if (stop_flag && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    try { task(); } catch (const std::exception &e) {
                        std::cerr << "Worker task exception: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "Worker task unknown exception\n";
                    }
                }
            });
        }
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lk(mtx);
            stop_flag = true;
        }
        cv.notify_all();
        for (auto &t : workers) if (t.joinable()) t.join();
    }
    template<typename F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using R = decltype(f());
        auto task_ptr = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task_ptr->get_future();
        {
            std::unique_lock<std::mutex> lk(mtx);
            tasks.emplace([task_ptr](){ (*task_ptr)(); });
        }
        cv.notify_one();
        return fut;
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop_flag;
};

// --------------------------- Sharded LRU Cache -------------------------------
class LRUShard {
public:
    using kv_t = std::pair<std::string, std::string>;
    LRUShard(size_t cap) : capacity(cap) {}

    bool get(const std::string &key, std::string &val) {
        std::shared_lock<std::shared_mutex> rlock(mux);
        auto it = map.find(key);
        if (it == map.end()) return false;
        rlock.unlock();
        std::unique_lock<std::shared_mutex> wlock(mux);
        items.splice(items.begin(), items, it->second);
        val = it->second->second;
        return true;
    }

    void put(const std::string &key, const std::string &val) {
        std::unique_lock<std::shared_mutex> wlock(mux);
        auto it = map.find(key);
        if (it != map.end()) {
            items.splice(items.begin(), items, it->second);
            it->second->second = val;
            return;
        }
        if (map.size() >= capacity) {
            auto &back = items.back();
            map.erase(back.first);
            items.pop_back();
        }
        items.emplace_front(key, val);
        map[key] = items.begin();
    }

    void del(const std::string &key) {
        std::unique_lock<std::shared_mutex> wlock(mux);
        auto it = map.find(key);
        if (it != map.end()) {
            items.erase(it->second);
            map.erase(it);
        }
    }

private:
    size_t capacity;
    std::list<kv_t> items;
    std::unordered_map<std::string, std::list<kv_t>::iterator> map;
    mutable std::shared_mutex mux;
};

class ShardedLRU {
public:
    ShardedLRU(size_t total_capacity, int shards)
        : shards_count(shards)
    {
        if (shards_count <= 0) shards_count = 1;
        size_t per = std::max<size_t>(1, total_capacity / shards_count);
        for (int i = 0; i < shards_count; ++i) {
            parts.emplace_back(std::make_unique<LRUShard>(per));
        }
    }

    bool get(const std::string &key, std::string &val) {
        auto &sh = *parts[shard_of(key)];
        return sh.get(key, val);
    }

    void put(const std::string &key, const std::string &val) {
        auto &sh = *parts[shard_of(key)];
        sh.put(key, val);
    }

    void del(const std::string &key) {
        auto &sh = *parts[shard_of(key)];
        sh.del(key);
    }

    int shards() const { return shards_count; }

private:
    size_t shard_of(const std::string &key) const {
        return std::hash<std::string>{}(key) % shards_count;
    }
    int shards_count;
    std::vector<std::unique_ptr<LRUShard>> parts;
};

// --------------------------- Global metrics ----------------------------------
static std::atomic<long long> g_total_requests(0);
static std::atomic<long long> g_total_success(0);
static std::atomic<long long> g_total_errors(0);
static std::atomic<long long> g_total_not_found(0);
static std::atomic<long long> g_cache_hits(0);
static std::atomic<long long> g_cache_misses(0);
static std::atomic<long long> g_db_reads(0);
static std::atomic<long long> g_db_writes(0);
static std::atomic<long long> g_db_deletes(0);

// --------------------------- Main --------------------------------------------
std::atomic<bool> keep_running{true};

void signal_handler(int) {
    keep_running.store(false);
}

int main(int argc, char **argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // print config
    std::cout << "Starting server on port " << PORT
              << " | workers=" << WORKER_THREADS
              << " | shards=" << SHARD_COUNT
              << " | cache=" << CACHE_CAPACITY
              << " | dbpool=" << DB_POOL_SIZE << std::endl;

    // create pieces
    ThreadPool pool(std::max<int>(1, WORKER_THREADS));
    ShardedLRU cache(CACHE_CAPACITY, SHARD_COUNT);

    // construct DB pool using env vars (db.hpp provides class)
    MySQLPool dbpool(DB_HOST, DB_USER, DB_PASS, DB_NAME, DB_POOL_SIZE);

    using namespace httplib;
    Server svr;

    // Helper: run a blocking job in the thread pool and return future
    auto enqueue_job = [&](auto job)->auto {
        return pool.submit(job);
    };

    // POST /kv : create or update
    svr.Post("/kv", [&](const Request &req, Response &res) {
        g_total_requests++;
        if (!req.has_param("key") || !req.has_param("val")) {
            res.status = 400;
            res.set_content("Missing 'key' or 'val'", "text/plain");
            return;
        }
        std::string key = req.get_param_value("key");
        std::string val = req.get_param_value("val");

        // enqueue a job that does db_put + cache put
        auto fut = enqueue_job([&cache, &dbpool, key, val]() -> bool {
            auto [idx, conn] = dbpool.acquire();
            bool ok = db_put_raw(conn, key, val);
            dbpool.release(idx);
            if (ok) {
                cache.put(key, val);
                g_db_writes++;
            }
            return ok;
        });

        bool ok = false;
        try { ok = fut.get(); }
        catch (...) { ok = false; }

        if (!ok) {
            g_total_errors++;
            res.status = 500;
            res.set_content("Database write failed", "text/plain");
            return;
        }
        g_total_success++;
        res.status = 201;
        res.set_content("Created", "text/plain");
    });

    // GET /kv/<key>
    svr.Get(R"(/kv/([a-zA-Z0-9_\-]+))", [&](const Request &req, Response &res) {
        g_total_requests++;
        std::string key = req.matches[1];
        std::string val;

        // Fast-path: try cache in-line (this only takes shared locks per shard)
        if (cache.get(key, val)) {
            g_cache_hits++;
            g_total_success++;
            res.set_header("X-Cache-Status", "HIT");
            res.set_content(val, "text/plain");
            return;
        }

        // Cache miss: enqueue DB read + cache insert
        auto fut = enqueue_job([&cache, &dbpool, key]() -> std::tuple<bool,std::string> {
            auto [idx, conn] = dbpool.acquire();
            std::string v;
            bool ok = db_get_raw(conn, key, v);
            dbpool.release(idx);
            if (ok) {
                cache.put(key, v);
            }
            return std::make_tuple(ok, v);
        });

        bool ok = false;
        std::string v;
        try {
            auto tup = fut.get();
            ok = std::get<0>(tup);
            v = std::get<1>(tup);
        } catch (...) { ok = false; }

        if (ok) {
            g_cache_misses++;
            g_db_reads++;
            g_total_success++;
            res.set_header("X-Cache-Status", "MISS");
            res.set_content(v, "text/plain");
        } else {
            g_total_not_found++;
            res.status = 404;
            res.set_content("Key not found", "text/plain");
        }
    });

    // DELETE /kv/<key>
    svr.Delete(R"(/kv/([a-zA-Z0-9_\-]+))", [&](const Request &req, Response &res) {
        g_total_requests++;
        std::string key = req.matches[1];
        auto fut = enqueue_job([&cache, &dbpool, key]() -> bool {
            auto [idx, conn] = dbpool.acquire();
            bool ok = db_del_raw(conn, key);
            dbpool.release(idx);
            if (ok) cache.del(key);
            return ok;
        });

        bool ok = false;
        try { ok = fut.get(); } catch (...) { ok = false; }

        if (!ok) {
            g_total_errors++;
            res.status = 500;
            res.set_content("DB delete failed", "text/plain");
            return;
        }
        g_db_deletes++;
        g_total_success++;
        res.status = 200;
        res.set_content("Deleted", "text/plain");
    });

    // /metrics endpoint (JSON)
    svr.Get("/metrics", [&](const Request&, Response& res) {
        std::ostringstream out;
        out << "{\n";
        out << "  \"requests\": " << g_total_requests.load() << ",\n";
        out << "  \"success\": " << g_total_success.load() << ",\n";
        out << "  \"errors\": " << g_total_errors.load() << ",\n";
        out << "  \"not_found\": " << g_total_not_found.load() << ",\n";
        out << "  \"cache_hits\": " << g_cache_hits.load() << ",\n";
        out << "  \"cache_misses\": " << g_cache_misses.load() << ",\n";
        out << "  \"db_reads\": " << g_db_reads.load() << ",\n";
        out << "  \"db_writes\": " << g_db_writes.load() << ",\n";
        out << "  \"db_deletes\": " << g_db_deletes.load() << ",\n";
        out << "  \"db_pool_in_use\": " << dbpool.in_use() << ",\n";
        out << "  \"db_pool_acquire_count\": " << dbpool.db_pool_acquire_count.load() << ",\n";
        out << "  \"db_pool_acquire_wait_us_sum\": " << dbpool.db_pool_acquire_wait_us.load() << ",\n";
        out << "  \"db_pool_acquire_wait_us_max\": " << dbpool.db_pool_acquire_wait_max_us.load() << ",\n";
        out << "  \"cache_shards\": " << cache.shards() << "\n";
        out << "}\n";
        res.set_content(out.str(), "application/json");
    });

    // Start server (listen)
    std::thread server_thread([&] {
        svr.listen("0.0.0.0", PORT);
    });

    // Wait for signal
    while (keep_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cerr << "Shutdown requested; stopping server..." << std::endl;
    svr.stop();
    if (server_thread.joinable()) server_thread.join();

    std::cout << "Server stopped gracefully." << std::endl;
    return 0;
}
