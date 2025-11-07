/*
 * client.cpp
 *
 * Multi-threaded, closed-loop load generator with mixed GET/PUT workloads.
 */

#include "httplib.h"
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <random>

// ------------------------------------------------------------
// 1. GLOBAL STATE AND METRICS
// ------------------------------------------------------------
std::atomic<bool> stop_flag(false);
std::atomic<long long> total_requests_completed(0);
std::atomic<long long> total_response_time_us(0);
std::atomic<long long> total_errors(0);
std::atomic<long long> total_not_found(0);

enum class WorkloadType
{
    GET_POPULAR,
    GET_ALL,
    PUT_ALL,
    MIXED
};

// ------------------------------------------------------------
// 2. CLIENT THREAD FUNCTION
// ------------------------------------------------------------
void client_worker(
    int thread_id,
    const std::string &server_ip,
    int server_port,
    WorkloadType workload,
    int num_put_threads,
    int max_keys_per_put_thread,
    int warmup_sec,
    double get_ratio,
    double put_ratio)
{
    httplib::Client client(server_ip, server_port);
    client.set_keep_alive(true);

    std::mt19937 gen(std::random_device{}() + thread_id);
    std::uniform_real_distribution<> prob(0.0, 1.0);
    std::uniform_int_distribution<> popular_dist(0, 99);
    std::uniform_int_distribution<> put_thread_dist(0, std::max(0, num_put_threads - 1));
    std::uniform_int_distribution<> key_index_dist(0, std::max(0, max_keys_per_put_thread - 1));

    long per_thread_request_counter = 0;
    auto test_start = std::chrono::steady_clock::now();

    while (!stop_flag)
    {
        std::string key;
        std::string val = "default_value_for_put";
        httplib::Result res;

        auto start_time = std::chrono::steady_clock::now();

        try
        {
            bool is_get = false;

            switch (workload)
            {
            case WorkloadType::GET_POPULAR:
            {
                is_get = true;
                int hot_count = std::min(10, max_keys_per_put_thread);
                // If there are fewer than 11 keys total, just pick uniformly from available keys
                if (max_keys_per_put_thread <= hot_count)
                {
                    std::uniform_int_distribution<> all_dist(0, std::max(0, max_keys_per_put_thread - 1));
                    int key_id = all_dist(gen);
                    key = "key_put_0_" + std::to_string(key_id);
                }
                else
                {
                    std::uniform_int_distribution<> hot_dist(0, hot_count - 1);
                    std::uniform_int_distribution<> cold_dist(hot_count, max_keys_per_put_thread - 1);
                    double p_hot = 0.8;
                    double p = prob(gen);
                    int key_id = (p < p_hot) ? hot_dist(gen) : cold_dist(gen);
                    key = "key_put_0_" + std::to_string(key_id);
                }
                res = client.Get("/kv/" + key);
                break;
            }
            case WorkloadType::GET_ALL:
            {
                is_get = true;
                int put_tid = put_thread_dist(gen);
                int key_id = key_index_dist(gen);
                key = "key_put_" + std::to_string(put_tid) + "_" + std::to_string(key_id);
                res = client.Get("/kv/" + key);
                break;
            }

            case WorkloadType::PUT_ALL:
            {
                key = "key_put_" + std::to_string(thread_id) + "_" + std::to_string(per_thread_request_counter++);
                httplib::Params params;
                params.emplace("key", key);
                params.emplace("val", val);
                res = client.Post("/kv", params);
                break;
            }
            case WorkloadType::MIXED:
            {
                double p = prob(gen);
                if (p < get_ratio / (get_ratio + put_ratio))
                {
                    // GET path
                    is_get = true;
                    int put_tid = put_thread_dist(gen);
                    int key_id = key_index_dist(gen);
                    key = "key_put_" + std::to_string(put_tid) + "_" + std::to_string(key_id);
                    res = client.Get("/kv/" + key);
                }
                else
                {
                    // PUT path
                    key = "key_put_" + std::to_string(thread_id) + "_" + std::to_string(per_thread_request_counter++);
                    httplib::Params params;
                    params.emplace("key", key);
                    params.emplace("val", val);
                    res = client.Post("/kv", params);
                }
                break;
            }
            }

            auto end_time = std::chrono::steady_clock::now();

            if (res)
            {
                if (res->status == 200 || res->status == 201)
                {
                    // successful
                    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
                    auto now = std::chrono::steady_clock::now();
                    auto since_start = std::chrono::duration_cast<std::chrono::seconds>(now - test_start).count();

                    if (since_start >= warmup_sec)
                    {
                        total_requests_completed.fetch_add(1, std::memory_order_relaxed);
                        total_response_time_us.fetch_add(elapsed_us, std::memory_order_relaxed);
                    }
                }
                else if (res->status == 404)
                {
                    // not found (count separately)
                    total_not_found.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    // other error status (5xx etc)
                    total_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                total_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
        catch (...)
        {
            total_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ------------------------------------------------------------
// 3. MAIN FUNCTION
// ------------------------------------------------------------
int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 10)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <server_ip> <port> <num_threads> <duration_sec> <workload_type>"
                  << " [num_put_threads] [max_keys_per_put_thread] [get_ratio put_ratio]" << std::endl;
        std::cerr << "Workload Types: GET_POPULAR, GET_ALL, PUT_ALL, MIXED" << std::endl;
        return 1;
    }

    std::string server_ip = argv[1];
    int port = std::stoi(argv[2]);
    int num_threads = std::stoi(argv[3]);
    int duration_sec = std::stoi(argv[4]);
    std::string workload_str = argv[5];

    int num_put_threads = 1;
    int max_keys_per_put_thread = 1000;
    double get_ratio = 1.0;
    double put_ratio = 0.0;

    WorkloadType workload;

    if (workload_str == "GET_POPULAR")
    {
        workload = WorkloadType::GET_POPULAR;
    }
    else if (workload_str == "GET_ALL")
    {
        if (argc < 8)
        {
            std::cerr << "Error: GET_ALL requires [num_put_threads] [max_keys_per_put_thread]" << std::endl;
            return 1;
        }
        workload = WorkloadType::GET_ALL;
        num_put_threads = std::stoi(argv[6]);
        max_keys_per_put_thread = std::stoi(argv[7]);
    }
    else if (workload_str == "PUT_ALL")
    {
        workload = WorkloadType::PUT_ALL;
    }
    else if (workload_str == "MIXED")
    {
        if (argc < 10)
        {
            std::cerr << "Error: MIXED requires [num_put_threads] [max_keys_per_put_thread] [get_ratio put_ratio]" << std::endl;
            return 1;
        }
        workload = WorkloadType::MIXED;
        num_put_threads = std::stoi(argv[6]);
        max_keys_per_put_thread = std::stoi(argv[7]);
        get_ratio = std::stod(argv[8]);
        put_ratio = std::stod(argv[9]);
    }
    else
    {
        std::cerr << "Invalid workload type: " << workload_str << std::endl;
        return 1;
    }

    const int warmup_sec = 5;
    const int total_runtime_sec = duration_sec + warmup_sec;

    if ((workload == WorkloadType::GET_ALL || workload == WorkloadType::MIXED) && num_put_threads <= 0) {
        std::cerr << "Error: num_put_threads must be >= 1" << std::endl;
        return 1;
    }
    if ((workload == WorkloadType::GET_ALL || workload == WorkloadType::MIXED) && max_keys_per_put_thread <= 0) {
        std::cerr << "Error: max_keys_per_put_thread must be >= 1" << std::endl;
        return 1;
    }
    if (workload == WorkloadType::MIXED && (get_ratio + put_ratio) <= 0.0) {
        std::cerr << "Error: get_ratio + put_ratio must be > 0" << std::endl;
        return 1;
    }

    std::cout << "Starting load test..." << std::endl;
    std::cout << "Target: " << server_ip << ":" << port << std::endl;
    std::cout << "Threads: " << num_threads << std::endl;
    std::cout << "Duration: " << duration_sec << " seconds (+ " << warmup_sec << "s warmup)" << std::endl;
    std::cout << "Workload: " << workload_str << std::endl;

    if (workload == WorkloadType::GET_ALL || workload == WorkloadType::MIXED)
    {
        std::cout << "Reading keys from " << num_put_threads << " PUT_ALL threads, "
                  << max_keys_per_put_thread << " keys each." << std::endl;
    }

    if (workload == WorkloadType::MIXED)
    {
        double total = get_ratio + put_ratio;
        std::cout << "Request Mix: " << (100.0 * get_ratio / total) << "% GET, "
                  << (100.0 * put_ratio / total) << "% PUT" << std::endl;
    }

    std::cout << "----------------------------------------" << std::endl;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(client_worker, i, server_ip, port, workload,
                             num_put_threads, max_keys_per_put_thread,
                             warmup_sec, get_ratio, put_ratio);
    }

    std::this_thread::sleep_for(std::chrono::seconds(total_runtime_sec));
    stop_flag = true;

    for (auto &th : threads)
        th.join();

    // ------------------------------------------------------------
    // METRICS REPORT
    // ------------------------------------------------------------
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Load Test Complete" << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    long long total_ops = total_requests_completed.load();
    long long total_time_us = total_response_time_us.load();
    long long errors = total_errors.load();

    double avg_throughput = static_cast<double>(total_ops) / duration_sec;
    double avg_latency_ms = (total_ops > 0)
                                ? (static_cast<double>(total_time_us) / total_ops / 1000.0)
                                : 0.0;
    double error_rate = (total_ops + errors > 0)
                            ? (100.0 * errors / (total_ops + errors))
                            : 0.0;

    std::cout << "Total Successful Requests: " << total_ops << std::endl;
    std::cout << "Total Errors:              " << errors << std::endl;
    std::cout << "Total Not Found (404):     " << total_not_found.load() << std::endl;
    std::cout << "Error Rate:                " << error_rate << " %" << std::endl;
    std::cout << "Average Throughput:        " << avg_throughput << " req/sec" << std::endl;
    std::cout << "Average Response Time:     " << avg_latency_ms << " ms" << std::endl;

    return 0;
}
