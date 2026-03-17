# Distributed Rate Limiter

A C++17 distributed API rate limiter built around the original architecture:

Client  
-> Crow HTTP gateway (`rate_limiter`)  
-> consistent hash ring  
-> gRPC node (`grpc_server`)  
-> token bucket

The upgraded system keeps that flow intact and adds:

- Redis-backed distributed token bucket state when `hiredis` is available
- in-memory fallback when Redis is unavailable
- gateway-side node health monitoring and dynamic hash-ring rebuilds
- richer atomic metrics
- structured request logs
- a `wrk`-based load-test script

## Architecture

```text
                         +----------------------+
                         |  HTTP Gateway        |
Client ---> GET /request |  Crow on :8000       |
                         |  - request_id        |
                         |  - health monitor    |
                         |  - dynamic hash ring |
                         +----------+-----------+
                                    |
                     consistent hash|client_id
                                    v
         +------------------+  +------------------+  +------------------+
         | gRPC node1       |  | gRPC node2       |  | gRPC node3       |
         | :50051           |  | :50052           |  | :50053           |
         | token bucket     |  | token bucket     |  | token bucket     |
         | Redis preferred  |  | Redis preferred  |  | Redis preferred  |
         | memory fallback  |  | memory fallback  |  | memory fallback  |
         +--------+---------+  +--------+---------+  +--------+---------+
                  \                    |                     /
                   \                   |                    /
                    +-------------------------------------+
                    | Redis bucket state (optional)       |
                    | key: rate_limit:{client_id}         |
                    | fields: tokens,last_refill_timestamp|
                    +-------------------------------------+
```

## Distributed Token Bucket

Each request is routed by `client_id`. The selected node evaluates a token bucket with:

- `capacity`
- `refill_rate`
- `tokens`
- `last_refill_timestamp`

If `hiredis` is available at build time, the node uses Redis hashes:

- key format: `rate_limit:{client_id}`
- fields:
  - `tokens`
  - `last_refill_timestamp`

Bucket updates are performed atomically with a Redis `EVAL` script so refill and token consumption happen in one operation.

If Redis is not compiled in or the Redis server is unavailable, the node falls back to the existing in-memory bucket behavior. The gateway tracks those Redis failures in `/metrics`.

## Failure Handling

The HTTP gateway now runs a background health checker that periodically calls a gRPC `HealthCheck` RPC on each node.

Behavior:

- healthy nodes remain in the active hash ring
- unhealthy nodes are removed from the active hash ring
- when a removed node comes back, it is re-added automatically
- the gateway logs:
  - `Node removed from cluster`
  - `Node rejoined cluster`

Request routing behavior:

- normal case: route to the hash owner in the active ring
- node down: the active ring reassigns ownership away from the failed node
- direct forwarding failure: the gateway still tries other active peers before returning `503`

## Build

```bash
cmake -S . -B build
cmake --build build
```

Expected binaries:

- `build/rate_limiter`
- `build/grpc_server`
- `build/grpc_client`

If `libhiredis-dev` is installed, CMake enables Redis support automatically.

## Running Redis

Start Redis locally:

```bash
redis-server --port 6379
```

Optional environment variables for the gRPC nodes:

```bash
export REDIS_HOST=127.0.0.1
export REDIS_PORT=6379
```

If Redis is not running, the nodes still work and fall back to in-memory buckets.

## Demo Cluster

Open four terminals in the repository root.

Terminal 1:

```bash
./build/grpc_server --node_id node1 --port 50051 --capacity 5 --refill_rate 1
```

Terminal 2:

```bash
./build/grpc_server --node_id node2 --port 50052 --capacity 5 --refill_rate 1
```

Terminal 3:

```bash
./build/grpc_server --node_id node3 --port 50053 --capacity 5 --refill_rate 1
```

Terminal 4:

```bash
./build/rate_limiter
```

Send a request:

```bash
curl -s "http://localhost:8000/request?client_id=user1"
```

Check metrics:

```bash
curl -s http://localhost:8000/metrics
```

Direct gRPC probe:

```bash
./build/grpc_client
```

## Metrics

`GET /metrics` now exposes:

- `total_requests`
- `forwarded_to_owner`
- `fallback_requests`
- `failed_requests`
- `allowed_requests`
- `rejected_requests`
- `redis_requests`
- `redis_failures`
- `node_failures`
- `hash_reroutes`

It also reports:

- configured peer list
- currently active nodes in the health-checked ring

## Structured Logging

Gateway and node logs now include the main fields needed for a systems demo:

- `request_id`
- `client_id`
- `target_node`
- `tokens_remaining`
- `latency_ms`

The gateway generates the `request_id` and propagates it over gRPC.

## Load Testing

The repository includes:

- `scripts/load_test.sh`
- `scripts/wrk_rate_limiter.lua`

Default scenario:

- 100 concurrent users
- 1000 requests target
- multiple randomized `client_id` values

Run it with:

```bash
scripts/load_test.sh
```

Requirements:

- `wrk` installed locally
- gateway running on `http://localhost:8000`
- cluster nodes already running

Useful overrides:

```bash
THREADS=8 CONNECTIONS=100 REQUESTS=1000 CLIENT_POOL_SIZE=250 scripts/load_test.sh
```

The script prints:

- throughput
- average latency
- p95 latency
- count of `200` responses
- count of `429` responses
- a simple rate-limit accounting ratio

## Load Test Results

`wrk` is not installed in this workspace, so no benchmark numbers are checked into this README yet. After running `scripts/load_test.sh`, record the output in this section.

Suggested template:

```text
Environment:
- 3 gRPC nodes
- 1 Crow gateway
- Redis enabled/disabled

Run:
- connections: 100
- requests: 1000
- client pool: 250

Results:
- throughput_req_per_sec=...
- avg_latency_ms=...
- p95_latency_ms=...
- allowed_requests=...
- rejected_requests=...
- rate_limit_accuracy=...
```

## Testing Instructions

1. Build the project with `cmake -S . -B build && cmake --build build`.
2. Start Redis if you want distributed bucket state.
3. Start the three gRPC nodes.
4. Start the HTTP gateway.
5. Send repeated requests to `/request?client_id=<id>`.
6. Inspect `/metrics`.
7. Stop one node and confirm:
   - `active_nodes` shrinks
   - `node_failures` increases
   - requests still succeed through rerouting
   - `hash_reroutes` increases for remapped clients
8. Run `scripts/load_test.sh` if `wrk` is installed.

## Notes

- The HTTP API remains `GET /request?client_id=<id>`.
- The project remains C++17, Crow-based for HTTP, and gRPC-based between nodes.
- Redis support is optional and activated only when `hiredis` is found at build time.
