# CS744 Project: HTTP-based Key-Value Store

**Author:** Jayasri PA \
**Roll Number:** 25D0361 \
**Email:** [25d0361@iitb.ac.in](mailto:25d0361@iitb.ac.in)

---

## 1. Project Overview

This project implements a **multi-threaded, HTTP-based Key-Value Store** in C++.
The system supports `GET`, `PUT`, and `DELETE` operations over HTTP, with a **thread-safe LRU cache** for fast access and a **MySQL backend** for persistence.
A separate multi-threaded **client** program acts as a load generator to evaluate system performance under different workloads.

---

## 2. Directory Structure

```
decs-project/
├── src/
│   ├── client/
│   │   └── client.cpp
│   ├── server/
│   │   └── server.cpp
│   └── include/
│       └── httplib.h
│
├── build/
│   ├── (compiled binaries: server, client)
│
├── Makefile
├── .gitignore
└── README.md
```

---

## 3. System Requirements

| Component            | Version / Tool               |
| -------------------- | ---------------------------- |
| **Operating System** | Ubuntu 24.04 LTS             |
| **Compiler**         | g++ 13.3.0 or higher         |
| **MySQL Server**     | 8.0.43                       |
| **Libraries Used**   | `cpp-httplib`, `mysqlclient` |

### MySQL Setup

Run the following commands to configure the database:

```sql
CREATE DATABASE cs744_project;
CREATE TABLE kv_store (id VARCHAR(255) PRIMARY KEY, val TEXT);
CREATE USER 'cs744_user'@'localhost' IDENTIFIED BY '123';
GRANT ALL PRIVILEGES ON cs744_project.* TO 'cs744_user'@'localhost';
FLUSH PRIVILEGES;
EXIT;
```

---

## 4. Build Instructions

1. Clone the repository:

   ```bash
   git clone https://github.com/jayasri-pa/CS744-Project.git
   ```
2. Move into the project directory:

   ```bash
   cd CS744-Project
   ```
3. Build both client and server:

   ```bash
   make
   ```
4. The binaries will be located in the `build/` folder:

   ```
   build/server
   build/client
   ```

---

## 5. System Architecture

![HTTP Client Server FlowChart](html-client-server.png)

> The KV store has a client, a cache, and a database connected through an HTTP server.

**Flow:**

1. The **client** sends HTTP requests to the **server**.
2. The **server** checks the **cache** first for GETs.
3. On cache miss → fetches from **MySQL** and updates the cache.
4. **PUT** and **DELETE** operations modify both cache and database.

---

## 6. Workload Types

| Type            | Description                                   | Typical Bottleneck |
| --------------- | --------------------------------------------- | ------------------ |
| **PUT_ALL**     | Only PUT requests. Used to fill the database. | Disk / Database    |
| **GET_ALL**     | Reads many unique keys (few cache hits).      | Database           |
| **GET_POPULAR** | Repeats requests for a few hot keys.          | Cache / CPU        |
| **MIXED**       | Mix of GET and PUT with configurable ratios.  | Mixed load         |

---

## 7. Client Usage

### Command Format

```bash
./client <server_ip> <port> <num_threads> <duration_sec> <workload_type> [num_put_threads] [max_keys_per_put_thread] [get_ratio put_ratio]
```

### Parameters

| Parameter                 | Description                                                  | Example     |
| ------------------------- | ------------------------------------------------------------ | ----------- |
| `server_ip`               | IP address of the KV server                                  | `127.0.0.1` |
| `port`                    | Port number where the server listens                         | `8080`      |
| `num_threads`             | Number of client threads                                     | `4`         |
| `duration_sec`            | Duration of the test (excluding 5s warm-up)                  | `30`        |
| `workload_type`           | One of: `PUT_ALL`, `GET_ALL`, `GET_POPULAR`, `MIXED`         | `MIXED`     |
| `num_put_threads`         | Number of threads used for PUT_ALL (for GET/MIXED workloads) | `4`         |
| `max_keys_per_put_thread` | Number of keys each PUT thread generates                     | `1000`      |
| `get_ratio`               | Ratio of GET operations in MIXED workload                    | `8`         |
| `put_ratio`               | Ratio of PUT operations in MIXED workload                    | `2`         |

---

## 8. Manual Testing using curl

### Correct way to test `POST` and `GET`

```bash
# Insert a key-value pair
curl -X POST -d "key=key1&val=value1" http://127.0.0.1:8080/kv

# Retrieve the key
curl -X GET http://127.0.0.1:8080/kv/key1

# Delete the key
curl -X DELETE http://127.0.0.1:8080/kv/key1
```

> **Note:** In cpp-httplib, POST parameters are parsed from the request body, not the query string.
> Hence, using `-d "key=key1&val=value1"` ensures the server correctly receives the parameters.

---

### Example Commands
* Start the server in a separate terminal
* Pin it to one physical core (two hyperthreads in the machine considered for test)
```taskset -c 0,1 ./server```

* In another terminal, run the client pinned to a different core
```
#Populate the database with PUT requests
taskset -c 4,5 ./client 127.0.0.1 8080 4 30 PUT_ALL

#Read frequently accessed keys (cache-intensive)
taskset -c 4,5 ./client 127.0.0.1 8080 4 30 GET_POPULAR

#Read all keys uniformly (DB-heavy workload)
taskset -c 4,5 ./client 127.0.0.1 8080 4 30 GET_ALL 4 1000

#Mixed workload: 80% GET, 20% PUT
taskset -c 4,5 ./client 127.0.0.1 8080 4 30 MIXED 4 1000 8 2
```
---

## 8. Example Output

```
Starting load test...
Target: 127.0.0.1:8080
Threads: 4
Duration: 30 seconds (+ 5s warmup)
Workload: MIXED
Reading keys from 4 PUT_ALL threads, 1000 keys each.
Request Mix: 80% GET, 20% PUT
----------------------------------------
Load Test Complete
----------------------------------------
Total Successful Requests: 2283
Total Errors:              0
Total Not Found (404):     4933
Error Rate:                0 %
Average Throughput:        76.1 req/sec
Average Response Time:     26.47 ms
```

---

## 9. Results Summary


| Workload                      | Throughput (req/s) | Avg Response Time (ms) | Successful Requests | Not Found (404) | Notes                 |
| ----------------------------- | -----------------: | ---------------------: | ------------------: | --------------: | --------------------- |
| **PUT_ALL**                   |              49.07 |                  81.74 |                1472 |               0 | Write-heavy, DB-bound |
| **GET_POPULAR**               |              97.07 |                  36.40 |                2912 |             431 | Cache hits dominate   |
| **GET_ALL**                   |              94.47 |                  17.53 |                2834 |            4506 | Cold-cache DB reads   |
| **MIXED (80% GET / 20% PUT)** |              79.20 |                  30.93 |                2376 |            2321 | Balanced workload     |
---

### Observations:

* Throughput improves significantly for cache-friendly workloads (GET_POPULAR).
* PUT-heavy workloads are bottlenecked by MySQL writes.
* Mixed workload performance remains stable under concurrent load.
* 404s mainly arise from GET requests for keys that were not yet inserted.
* No request errors or connection failures were observed across all tests.

---

## 10. References

* cpp-httplib: [https://github.com/yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib)
* MySQL C API: [https://dev.mysql.com/downloads/c-api/](https://dev.mysql.com/downloads/c-api/)
* CS744 Course Project (Fall 2025)
