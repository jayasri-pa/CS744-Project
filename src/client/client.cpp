// client.cpp
// Multi-threaded closed-loop load generator with percentiles, prepopulate, CSV export, and JSON summary.
// Compile: g++ -std=c++17 client.cpp -o client -lpthread
//
// Usage examples:
// 1) Prepopulate keys:
//    ./client 127.0.0.1 8080 prepopulate 4 1000    # num_put_threads=4, max_keys_per_put_thread=1000
//
// 2) Run GET_ALL for 60s with 32 threads:
//    ./client 127.0.0.1 8080 run 32 60 GET_ALL 4 1000 --csv latencies.csv --seed 42
//
// 3) Run MIXED with get:put = 3:1 for 120s:
//    ./client 127.0.0.1 8080 run 64 120 MIXED 4 1000 3 1 --csv lat.csv --rate 500
//
// Modes:
//  - prepopulate <num_put_threads> <max_keys_per_put_thread>
//  - run <num_threads> <duration_sec> <workload> [...workload params...]
//
// Workloads:
//  GET_POPULAR
//  GET_ALL <num_put_threads> <max_keys_per_put_thread>
//  PUT_ALL
//  MIXED <num_put_threads> <max_keys_per_put_thread> <get_ratio> <put_ratio>
//
// Notes:
//  * The client uses cpp-httplib (https://github.com/yhirose/cpp-httplib). Make sure the include is available.
//  * Latency vectors are stored per-thread and aggregated at the end. For extremely long runs + very high RPS this may use a lot of memory.
//  * JSON summary printed to stdout; optional CSV file for raw per-request latencies.

#include "httplib.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

enum class WorkloadType
{
    GET_POPULAR,
    GET_ALL,
    PUT_ALL,
    MIXED
};

struct Config
{
    std::string server_ip;
    int server_port = 8080;
    std::string mode;        // "prepopulate" or "run"
    int num_threads = 1;     // for run
    int duration_sec = 10;   // for run (measured duration, excluding warmup)
    WorkloadType workload;   // for run
    int num_put_threads = 4; // used for GET_ALL / MIXED
    int max_keys_per_put_thread = 10000;
    double get_ratio = 1.0; // for MIXED
    double put_ratio = 0.0; // for MIXED
    int warmup_sec = 5;
    bool do_prepopulate = false;
    std::string csv_path = "";
    std::string json_path = "";
    unsigned int seed = 0;
    bool use_seed = false;
    int max_retries = 3;
    int base_backoff_ms = 20;
    double target_rate = 0.0; // global RPS limit (0.0 = unlimited)
};

static std::mutex print_mtx;

// ---------- Global aggregated counters (atomic) ----------
static std::atomic<long long> agg_total_requests_completed(0);
static std::atomic<long long> agg_total_errors(0);
static std::atomic<long long> agg_total_not_found(0);
static std::atomic<long long> agg_cache_hits(0);
static std::atomic<long long> agg_cache_misses(0);

// ---------- Utility functions ----------
static inline long long now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void print_usage(const char *prog)
{
    std::lock_guard<std::mutex> lk(print_mtx);
    std::cerr << "Usage:\n";
    std::cerr << "  Prepopulate mode:\n";
    std::cerr << "    " << prog << " <server_ip> <port> prepopulate <num_put_threads> <max_keys_per_put_thread>\n";
    std::cerr << "  Run mode:\n";
    std::cerr << "    " << prog << " <server_ip> <port> run <num_threads> <duration_sec> <workload> "
                                   "[workload params...]\n";
    std::cerr << "  Workload types:\n";
    std::cerr << "    GET_POPULAR\n";
    std::cerr << "    GET_ALL <num_put_threads> <max_keys_per_put_thread>\n";
    std::cerr << "    PUT_ALL\n";
    std::cerr << "    MIXED <num_put_threads> <max_keys_per_put_thread> <get_ratio> <put_ratio>\n";
    std::cerr << "  Optional flags (append at end):\n";
    std::cerr << "    --csv <path>    : write raw latencies to CSV\n";
    std::cerr << "    --seed <N>      : deterministic RNG seed\n";
    std::cerr << "    --retries <N>   : retry attempts for transient errors (default 3)\n";
    std::cerr << "    --rate <RPS>    : target aggregate request rate (requests/sec)\n";
    std::cerr << std::endl;
}

// compute percentile (0-100) from vector of microsecond latencies. vector must be non-empty.
static double percentile_from_sorted_vec(const std::vector<long long> &v_sorted, double pct)
{
    if (v_sorted.empty())
        return 0.0;
    double pos = pct * (v_sorted.size() - 1) / 100.0;
    size_t idx = static_cast<size_t>(std::floor(pos));
    double frac = pos - idx;
    if (idx + 1 < v_sorted.size())
    {
        return v_sorted[idx] * (1.0 - frac) + v_sorted[idx + 1] * frac;
    }
    else
    {
        return static_cast<double>(v_sorted[idx]);
    }
}

// aggregate percentiles and simple stats
struct Summary
{
    long long total_success = 0;
    long long total_errors = 0;
    long long total_not_found = 0;
    long long cache_hits = 0;
    long long cache_misses = 0;
    double throughput = 0.0; // req/sec
    double avg_latency_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
};

// ---------- Worker thread for PUT (used for prepopulate and PUT_ALL path) ----------
void do_put(httplib::Client &cli, const std::string &key, const std::string &val,
            int max_retries, int base_backoff_ms,
            long long &local_success, long long &local_errors)
{
    httplib::Params params;
    params.emplace("key", key);
    params.emplace("val", val);

    int attempt = 0;
    while (attempt <= max_retries)
    {
        auto res = cli.Post("/kv", params);
        if (res && (res->status == 200 || res->status == 201))
        {
            local_success++;
            return;
        }
        else
        {
            attempt++;
            if (attempt > max_retries)
            {
                local_errors++;
                return;
            }
            // exponential backoff
            int backoff = base_backoff_ms * (1 << (attempt - 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
        }
    }
}

// ---------- Client worker (main load generating thread) ----------
void client_worker_func(int thread_id, const Config &cfg,
                        std::atomic<bool> &stop_flag,
                        std::vector<long long> &latencies_us_out,
                        long long &local_success, long long &local_errors,
                        long long &local_not_found, long long &local_cache_hits,
                        long long &local_cache_misses,
                        unsigned int rng_seed_override = 0)
{
    httplib::Client client(cfg.server_ip.c_str(), cfg.server_port);
    client.set_keep_alive(true);
    // small timeout to avoid blocking too long on network (tunable)
    client.set_connection_timeout(5); // seconds

    std::mt19937 gen;
    if (cfg.use_seed)
    {
        gen.seed(cfg.seed + thread_id + rng_seed_override);
    }
    else
    {
        gen.seed(std::random_device{}() + thread_id + rng_seed_override);
    }

    std::uniform_real_distribution<> prob(0.0, 1.0);
    std::uniform_int_distribution<> popular_dist(0, 99);
    std::uniform_int_distribution<> put_thread_dist(0, std::max(1, cfg.num_put_threads) - 1);
    std::uniform_int_distribution<> key_index_dist(0, std::max(1, cfg.max_keys_per_put_thread) - 1);

    long per_thread_put_counter = 0;

    // Rate limiter: shared tokens per second across threads. We'll implement a simple sleep-based approach:
    const double target_rps = cfg.target_rate; // if 0.0 then unlimited
    const double per_thread_rps = (target_rps > 0.0) ? (target_rps / cfg.num_threads) : 0.0;
    const std::chrono::microseconds per_request_delay_us = (per_thread_rps > 0.0)
                                                               ? std::chrono::microseconds(static_cast<long long>(1e6 / per_thread_rps))
                                                               : std::chrono::microseconds(0);

    while (!stop_flag.load(std::memory_order_acquire))
    {
        bool is_get = false;
        std::string key;
        std::string val = "value_from_client";
        httplib::Result res;

        long long start_us = now_us();

        try
        {
            if (cfg.workload == WorkloadType::GET_POPULAR)
            {
                is_get = true;
                int hot_count = std::min(10, cfg.max_keys_per_put_thread);
                if (cfg.max_keys_per_put_thread <= hot_count)
                {
                    std::uniform_int_distribution<> all_dist(0, std::max(1, cfg.max_keys_per_put_thread) - 1);
                    int key_id = all_dist(gen);
                    key = "key_put_0_" + std::to_string(key_id);
                }
                else
                {
                    std::uniform_int_distribution<> hot_dist(0, hot_count - 1);
                    std::uniform_int_distribution<> cold_dist(hot_count, cfg.max_keys_per_put_thread - 1);
                    double p_hot = 0.8;
                    double p = prob(gen);
                    int key_id = (p < p_hot) ? hot_dist(gen) : cold_dist(gen);
                    key = "key_put_0_" + std::to_string(key_id);
                }
                res = client.Get("/kv/" + key);
            }
            else if (cfg.workload == WorkloadType::GET_ALL)
            {
                is_get = true;
                int put_tid = put_thread_dist(gen);
                int key_id = key_index_dist(gen);
                key = "key_put_" + std::to_string(put_tid) + "_" + std::to_string(key_id);
                res = client.Get("/kv/" + key);
            }
            else if (cfg.workload == WorkloadType::PUT_ALL)
            {
                key = "key_put_" + std::to_string(thread_id) + "_" + std::to_string(per_thread_put_counter++);
                httplib::Params params;
                params.emplace("key", key);
                params.emplace("val", val);
                // implement retries for PUTs (transient errors)
                int attempt = 0;
                while (attempt <= cfg.max_retries)
                {
                    res = client.Post("/kv", params);
                    if (res && (res->status == 200 || res->status == 201))
                        break;
                    attempt++;
                    if (attempt > cfg.max_retries)
                        break;
                    int backoff = cfg.base_backoff_ms * (1 << (attempt - 1));
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                }
            }
            else if (cfg.workload == WorkloadType::MIXED)
            {
                double p = prob(gen);
                double denom = (cfg.get_ratio + cfg.put_ratio);
                double threshold = (denom > 0.0) ? (cfg.get_ratio / denom) : 1.0;
                if (p < threshold)
                {
                    is_get = true;
                    int put_tid = put_thread_dist(gen);
                    int key_id = key_index_dist(gen);
                    key = "key_put_" + std::to_string(put_tid) + "_" + std::to_string(key_id);
                    res = client.Get("/kv/" + key);
                }
                else
                {
                    key = "key_put_" + std::to_string(thread_id) + "_" + std::to_string(per_thread_put_counter++);
                    httplib::Params params;
                    params.emplace("key", key);
                    params.emplace("val", val);
                    int attempt = 0;
                    while (attempt <= cfg.max_retries)
                    {
                        res = client.Post("/kv", params);
                        if (res && (res->status == 200 || res->status == 201))
                            break;
                        attempt++;
                        if (attempt > cfg.max_retries)
                            break;
                        int backoff = cfg.base_backoff_ms * (1 << (attempt - 1));
                        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                    }
                }
            }

            long long end_us = now_us();
            long long elapsed_us = end_us - start_us;

            // result handling
            if (res)
            {
                if (res->status == 200 || res->status == 201)
                {
                    // parse X-Cache-Status header if present
                    std::string cache_status = res->get_header_value("X-Cache-Status");
                    if (!cache_status.empty())
                    {
                        if (cache_status == "HIT")
                        {
                            local_cache_hits++;
                        }
                        else if (cache_status == "MISS")
                        {
                            local_cache_misses++;
                        }
                    }
                    local_success++;
                    latencies_us_out.push_back(elapsed_us);
                }
                else if (res->status == 404)
                {
                    local_not_found++;
                }
                else
                {
                    local_errors++;
                }
            }
            else
            {
                local_errors++;
            }
        }
        catch (const std::exception &ex)
        {
            // log but keep running
            {
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cerr << "[thread " << thread_id << "] exception: " << ex.what() << std::endl;
            }
            local_errors++;
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lk(print_mtx);
            std::cerr << "[thread " << thread_id << "] unknown exception\n";
            local_errors++;
        }

        // optional rate limiting per thread (simple sleep)
        if (per_request_delay_us.count() > 0)
        {
            std::this_thread::sleep_for(per_request_delay_us);
        }
    } // end while
}

// ---------- Prepopulate function (multi-threaded puts) ----------
void prepopulate_keys(const Config &cfg)
{
    std::cout << "Prepopulating keys: " << cfg.num_put_threads << " put-threads, " << cfg.max_keys_per_put_thread << " keys each\n";
    std::vector<std::thread> threads;
    std::atomic<int> active_threads(0);
    std::atomic<long long> total_success(0);
    std::atomic<long long> total_errors(0);

    auto worker = [&](int tid)
    {
        httplib::Client client(cfg.server_ip.c_str(), cfg.server_port);
        client.set_keep_alive(true);
        long long local_success = 0;
        long long local_errors = 0;
        for (int i = 0; i < cfg.max_keys_per_put_thread; ++i)
        {
            std::string key = "key_put_" + std::to_string(tid) + "_" + std::to_string(i);
            do_put(client, key, "initial_value", cfg.max_retries, cfg.base_backoff_ms, local_success, local_errors);
        }
        total_success += local_success;
        total_errors += local_errors;
    };

    for (int t = 0; t < cfg.num_put_threads; ++t)
    {
        threads.emplace_back(worker, t);
    }
    for (auto &th : threads)
        th.join();
    std::cout << "Prepopulate done. Success: " << total_success.load() << ", Errors: " << total_errors.load() << "\n";
}

// ---------- Main run orchestration ----------
int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        print_usage(argv[0]);
        return 1;
    }

    Config cfg;
    cfg.server_ip = argv[1];
    cfg.server_port = std::stoi(argv[2]);
    cfg.mode = argv[3]; // "prepopulate" or "run"

    // parse optional flags at end
    auto parse_optional_flags = [&](int start_idx)
    {
        for (int i = start_idx; i < argc; ++i)
        {
            std::string token = argv[i];
            if (token == "--csv" && i + 1 < argc)
            {
                cfg.csv_path = argv[++i];
            }
            else if (token == "--seed" && i + 1 < argc)
            {
                cfg.seed = static_cast<unsigned int>(std::stoul(argv[++i]));
                cfg.use_seed = true;
            }
            else if (token == "--retries" && i + 1 < argc)
            {
                cfg.max_retries = std::stoi(argv[++i]);
            }
            else if (token == "--rate" && i + 1 < argc)
            {
                cfg.target_rate = std::stod(argv[++i]);
            }
            else if (token == "--json" && i + 1 < argc)
            {
                cfg.json_path = argv[++i];
            }
            else
            {
                std::cerr << "Unknown or malformed option: " << token << "\n";
                print_usage(argv[0]);
                exit(1);
            }
        }
    };

    if (cfg.mode == "prepopulate")
    {
        if (argc < 6)
        {
            std::cerr << "prepopulate requires: num_put_threads max_keys_per_put_thread\n";
            return 1;
        }
        cfg.num_put_threads = std::stoi(argv[4]);
        cfg.max_keys_per_put_thread = std::stoi(argv[5]);
        // optional flags after that
        if (argc > 6)
            parse_optional_flags(6);
        prepopulate_keys(cfg);
        return 0;
    }
    else if (cfg.mode == "run")
    {
        // minimal required args for run:
        // argv[4] = num_threads, argv[5] = duration_sec, argv[6] = workload
        if (argc < 7)
        {
            print_usage(argv[0]);
            return 1;
        }
        cfg.num_threads = std::stoi(argv[4]);
        cfg.duration_sec = std::stoi(argv[5]);
        std::string workload_str = argv[6];

        int next_arg = 7;
        if (workload_str == "GET_POPULAR")
        {
            cfg.workload = WorkloadType::GET_POPULAR;
        }
        else if (workload_str == "GET_ALL")
        {
            if (argc < next_arg + 2)
            {
                std::cerr << "GET_ALL requires [num_put_threads] [max_keys_per_put_thread]\n";
                return 1;
            }
            cfg.workload = WorkloadType::GET_ALL;
            cfg.num_put_threads = std::stoi(argv[next_arg++]);
            cfg.max_keys_per_put_thread = std::stoi(argv[next_arg++]);
        }
        else if (workload_str == "PUT_ALL")
        {
            cfg.workload = WorkloadType::PUT_ALL;
        }
        else if (workload_str == "MIXED")
        {
            if (argc < next_arg + 4)
            {
                std::cerr << "MIXED requires [num_put_threads] [max_keys_per_put_thread] [get_ratio] [put_ratio]\n";
                return 1;
            }
            cfg.workload = WorkloadType::MIXED;
            cfg.num_put_threads = std::stoi(argv[next_arg++]);
            cfg.max_keys_per_put_thread = std::stoi(argv[next_arg++]);
            cfg.get_ratio = std::stod(argv[next_arg++]);
            cfg.put_ratio = std::stod(argv[next_arg++]);
            if (cfg.get_ratio + cfg.put_ratio <= 0.0)
            {
                std::cerr << "get_ratio + put_ratio must be > 0\n";
                return 1;
            }
        }
        else
        {
            std::cerr << "Unknown workload: " << workload_str << "\n";
            print_usage(argv[0]);
            return 1;
        }

        // parse optional flags after required args
        if (next_arg < argc)
            parse_optional_flags(next_arg);

        // basic validation
        if ((cfg.workload == WorkloadType::GET_ALL || cfg.workload == WorkloadType::MIXED) && cfg.num_put_threads <= 0)
        {
            std::cerr << "num_put_threads must be >= 1\n";
            return 1;
        }
        if ((cfg.workload == WorkloadType::GET_ALL || cfg.workload == WorkloadType::MIXED) && cfg.max_keys_per_put_thread <= 0)
        {
            std::cerr << "max_keys_per_put_thread must be >= 1\n";
            return 1;
        }

        // Summary of run
        {
            std::lock_guard<std::mutex> lk(print_mtx);
            std::cout << "Server: " << cfg.server_ip << ":" << cfg.server_port << "\n";
            std::cout << "Threads: " << cfg.num_threads << ", Duration: " << cfg.duration_sec << "s (+ warmup " << cfg.warmup_sec << "s)\n";
            std::cout << "Workload: " << workload_str << "\n";
            if (cfg.workload == WorkloadType::GET_ALL || cfg.workload == WorkloadType::MIXED)
            {
                std::cout << "  Reading keys from " << cfg.num_put_threads << " put-threads with " << cfg.max_keys_per_put_thread << " keys each\n";
            }
            if (cfg.workload == WorkloadType::MIXED)
            {
                double tot = cfg.get_ratio + cfg.put_ratio;
                std::cout << "  Mix: " << (100.0 * cfg.get_ratio / tot) << "% GET, " << (100.0 * cfg.put_ratio / tot) << "% PUT\n";
            }
            if (!cfg.csv_path.empty())
                std::cout << "CSV output: " << cfg.csv_path << "\n";
            if (cfg.use_seed)
                std::cout << "Using deterministic seed: " << cfg.seed << "\n";
            if (cfg.target_rate > 0.0)
                std::cout << "Target aggregate RPS: " << cfg.target_rate << "\n";
            std::cout << "----------------------------------------\n";
        }

        // Warmup consideration:
        // If workload is GET_ALL or MIXED and keys are not prepopulated, it's recommended to run prepopulate mode first.
        // This client does not automatically prepopulate during 'run' to avoid unintended writes.
        // TODO: you can add an option to auto-prepopulate here if desired.

        // prepare per-thread structures
        std::vector<std::thread> threads;
        std::vector<std::vector<long long>> thread_latencies(cfg.num_threads);
        std::vector<long long> thread_success(cfg.num_threads, 0);
        std::vector<long long> thread_errors(cfg.num_threads, 0);
        std::vector<long long> thread_not_found(cfg.num_threads, 0);
        std::vector<long long> thread_cache_hits(cfg.num_threads, 0);
        std::vector<long long> thread_cache_misses(cfg.num_threads, 0);

        std::atomic<bool> stop_flag(false);

        // Start threads
        for (int t = 0; t < cfg.num_threads; ++t)
        {
            threads.emplace_back(client_worker_func, t, std::cref(cfg), std::ref(stop_flag),
                                 std::ref(thread_latencies[t]),
                                 std::ref(thread_success[t]),
                                 std::ref(thread_errors[t]),
                                 std::ref(thread_not_found[t]),
                                 std::ref(thread_cache_hits[t]),
                                 std::ref(thread_cache_misses[t]),
                                 /*rng_seed_override=*/0);
        }

        // run: sleep for warmup + measured duration
        std::this_thread::sleep_for(std::chrono::seconds(cfg.warmup_sec + cfg.duration_sec));
        stop_flag.store(true, std::memory_order_release);

        for (auto &th : threads)
            th.join();

        // Aggregate results
        std::vector<long long> all_latencies;
        long long total_success = 0, total_errors = 0, total_not_found = 0, total_cache_hits = 0, total_cache_misses = 0;
        for (int t = 0; t < cfg.num_threads; ++t)
        {
            total_success += thread_success[t];
            total_errors += thread_errors[t];
            total_not_found += thread_not_found[t];
            total_cache_hits += thread_cache_hits[t];
            total_cache_misses += thread_cache_misses[t];
            // move latencies
            auto &v = thread_latencies[t];
            all_latencies.insert(all_latencies.end(), v.begin(), v.end());
        }

        // compute stats
        double measured_duration = static_cast<double>(cfg.duration_sec); // exclude warmup
        double throughput = (measured_duration > 0.0) ? (static_cast<double>(total_success) / measured_duration) : 0.0;
        double avg_latency_ms = 0.0;
        if (!all_latencies.empty())
        {
            long long sum_us = 0;
            for (auto x : all_latencies)
                sum_us += x;
            avg_latency_ms = static_cast<double>(sum_us) / all_latencies.size() / 1000.0;
        }

        // percentiles
        std::sort(all_latencies.begin(), all_latencies.end());
        double p50_ms = percentile_from_sorted_vec(all_latencies, 50.0) / 1000.0;
        double p95_ms = percentile_from_sorted_vec(all_latencies, 95.0) / 1000.0;
        double p99_ms = percentile_from_sorted_vec(all_latencies, 99.0) / 1000.0;

        // optional CSV dump (one latency per line in microseconds)
        if (!cfg.csv_path.empty())
        {
            std::ofstream csv(cfg.csv_path);
            if (csv)
            {
                csv << "latency_us\n";
                for (auto v : all_latencies)
                    csv << v << "\n";
                csv.close();
            }
            else
            {
                std::cerr << "Failed to open CSV file: " << cfg.csv_path << "\n";
            }
        }

        // JSON output (print to stdout)
        std::ostringstream json;
        json << std::fixed << std::setprecision(3);
        json << "{\n";
        json << "  \"server\": \"" << cfg.server_ip << ":" << cfg.server_port << "\",\n";
        json << "  \"threads\": " << cfg.num_threads << ",\n";
        json << "  \"duration_sec\": " << cfg.duration_sec << ",\n";
        json << "  \"warmup_sec\": " << cfg.warmup_sec << ",\n";
        json << "  \"workload\": \"" << workload_str << "\",\n";
        json << "  \"num_put_threads\": " << cfg.num_put_threads << ",\n";
        json << "  \"max_keys_per_put_thread\": " << cfg.max_keys_per_put_thread << ",\n";
        json << "  \"total_success\": " << total_success << ",\n";
        json << "  \"total_errors\": " << total_errors << ",\n";
        json << "  \"total_not_found\": " << total_not_found << ",\n";
        json << "  \"cache_hits\": " << total_cache_hits << ",\n";
        json << "  \"cache_misses\": " << total_cache_misses << ",\n";
        json << "  \"throughput_req_per_sec\": " << throughput << ",\n";
        json << "  \"avg_latency_ms\": " << avg_latency_ms << ",\n";
        json << "  \"p50_ms\": " << p50_ms << ",\n";
        json << "  \"p95_ms\": " << p95_ms << ",\n";
        json << "  \"p99_ms\": " << p99_ms << ",\n";
        json << "  \"csv_path\": \"" << cfg.csv_path << "\"\n";
        json << "}\n";

        if (!cfg.json_path.empty())
        {
            std::ofstream jf(cfg.json_path);
            if (jf)
            {
                jf << json.str();
                jf.close();
            }
            else
            {
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cerr << "Failed to open JSON output file: " << cfg.json_path << "\n";
                std::cout << json.str() << std::endl;
            }
        }
        else
        {
            std::cout << "----------------------------------------\n";
            std::cout << "Load Test Complete\n";
            std::cout << "----------------------------------------\n";
            std::cout << json.str() << std::endl;
        }
        return 0;
    }
    else
    {
        std::cerr << "Unknown mode: " << cfg.mode << "\n";
        print_usage(argv[0]);
        return 1;
    }
}
