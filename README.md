# Orbit

![C23](https://img.shields.io/badge/C-23-blue.svg?style=flat-square)
![Linux](https://img.shields.io/badge/Platform-Linux-green.svg?style=flat-square)
![io_uring](https://img.shields.io/badge/I/O-io__uring-orange.svg?style=flat-square)
![Zero Allocation](https://img.shields.io/badge/Memory-Zero_Allocation-red.svg?style=flat-square)
![License](https://img.shields.io/badge/License-AGPLv3-red.svg)

`orbit` is an ultra-low latency, zero-allocation, lock-free WebSocket-to-SIP bridge written in modern C23. It seamlessly bridges bidirectional WebSocket payloads (like OPUS/PCMU) strictly into raw RTP packets while orchestrating SIP endpoints.

Designed for raw speed, it uses a multi-ring Linux `io_uring` kernel event loop and data-oriented pre-allocated memory slabs to guarantee deterministic, jitter-free performance under heavy VoIP load.

## Features

- **Zero Allocation Hot Paths:** All `malloc` and `free` operations are statically forbidden after the initial boot phase. Memory is driven by `O(1)` memory-aligned slabs.
- **`io_uring` Backend:** Fully async, kernel-level I/O processing for both TCP (WebSockets) and UDP (SIP/RTP) processing.
- **Scatter-Gather I/O:** Packets are processed with zero-copy logic via `iovec`.
- **Stateless SIP Routing:** Real-time SIP `INVITE` mapping, `SDP` parsing, and `OPTIONS` capability advertisement.
- **WebSocket Bridge:** Natively bridges WebSocket binary audio payloads into valid RTP datagrams (and vice versa) utilizing `wslay`.
- **Asynchronous Eventing:** Publishes real-time call lifecycle events (e.g., `call_answered`) asynchronously to protect the hot path. Supports three backends:
  - **UDP:** Lightweight raw JSON event emission over UDP sockets.
  - **Redis:** Publishes events directly to a configurable Redis Pub/Sub channel.
  - **Kafka:** Produces events to a Kafka topic via a high-performance, asynchronous `librdkafka` producer.

## Architecture

At its core, `orbit` employs a **Multi-Ring Share-Nothing Architecture** designed to completely eliminate cross-core cache invalidation and kernel scheduling overhead.

1. **Thread Pinning:** Upon startup, the supervisor process dynamically spawns exactly one worker thread per physical CPU core (using `pthread_setaffinity_np`).
2. **Pre-Allocation:** Each thread boots by pre-allocating an isolated, fixed-size memory slab for all concurrent state objects (RTP contexts, SIP sessions, WebSocket buffers). `malloc` and `free` are entirely forbidden in the hot path.
3. **`thread_local` Isolation:** Because all state is strictly `thread_local`, the system completely avoids mutex locks, atomics, or synchronization primitives during packet bridging.
4. **Kernel Load Balancing (`SO_REUSEPORT`):** Every worker binds to the exact same SIP and WebSocket ports. The Linux network stack natively hashes incoming 4-tuples, ensuring that all packets belonging to a given call are routed exclusively to the same isolated CPU core.
5. **RTP Port Sharding:** Dynamic RTP ports are sharded sequentially across the worker threads, guaranteeing that bidirectional media streams naturally bind to the exact same core that originated the SIP dialogue.

This architecture was selected because it guarantees strict `O(1)` bounds checking, prevents memory fragmentation, and reduces CPU cache misses to near zero, resulting in perfectly linear, deterministic vertical scaling.

## Performance

Built from the ground up for massive concurrency, the bridge uses `SO_REUSEPORT` kernel load balancing and RTP port sharding to ensure complete 1:1 hardware core isolation (Share-Nothing Architecture).

### Load Test Metrics

- **Concurrent Calls:** `4,000` (Bidirectional bridging, 8,000 streams)
- **Packet Throughput:** `400,000` packets per second
- **Instruction Efficiency:** `~46` CPU instructions per routed packet
- **Hardware:** `16 Cores`
- **CPU Utilization:** `< 0.1%` global CPU saturation (cores spend 99.9% of their time asleep)
- **Memory Footprint:** Zero allocations in the hot path. Static memory footprint with `0` major page faults during operation.
- **Cache Performance:** 100% cache-line aligned hot paths completely eliminate cross-core false sharing.

## Requirements

- **OS:** Linux kernel 5.1+ (requires `io_uring` support)
- **Compiler:** GCC 14.2+ or Clang (Must support `-std=gnu23`)
- **Dependencies:** `liburing`, `pkg-config`

## Build Instructions

This project uses CMake. The build process automatically fetches and embeds dependencies like `wslay` out-of-the-box.

```bash
# Configure the build directory
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build the project
cmake --build build

# Run the test suite
ctest --test-dir build
```

## Running the Server

Start the bridge by running the built executable. `orbit` relies heavily on environment variables for its configuration.

```bash
./scripts/run.sh
```

### Environment Variables

| Variable | Default Value | Description |
|---|---|---|
| `SIP_LISTEN_ADDR` | `0.0.0.0` | The local IP address the SIP listener should bind to. |
| `SIP_LISTEN_PORT` | `5060` | The UDP port the SIP listener should bind to. |
| `SIP_EXTERNAL_ADDR` | `127.0.0.1` | The public IP address advertised in SIP headers (`Contact`, `Via`). |
| `RTP_LISTEN_ADDR` | `0.0.0.0` | The local IP address the RTP media ports should bind to. |
| `RTP_EXTERNAL_ADDR` | `127.0.0.1` | The public IP address advertised in the SIP `SDP` bodies. |
| `RTP_MIN_PORT` | `16000` | The inclusive start range of the dynamic UDP RTP ports. |
| `RTP_MAX_PORT` | `32000` | The inclusive end range of the dynamic UDP RTP ports. |
| `WS_LISTEN_ADDR` | `0.0.0.0` | The local IP address the WebSocket bridge should bind to. |
| `WS_EXTERNAL_ADDR` | `127.0.0.1` | The public IP address the WebSocket server advertises (if applicable). |
| `WS_LISTEN_PORT` | `8080` | The TCP port the WebSocket bridge binds and listens on. |
| `WS_EXTERNAL_PORT` | `8080` | The public TCP port for the WebSocket server advertised in events. |
| `EVENT_PROVIDER` | `udp` | The event provider backend to use (`udp`, `redis`, `kafka`, `mock`, `disabled`). |
| `EVENT_QUEUE_CAPACITY` | `16384` | Maximum event queue capacity per worker thread. Must be between `MAX_CALLS` and `MAX_CALLS * 16`. |
| `EVENT_UDP_LISTEN_ADDR` | None | Optional. If provided, UDP bridge events will be sent to this IP. |
| `EVENT_UDP_LISTEN_PORT` | `0` | Optional. If provided, UDP bridge events will be sent to this port. |
| `EVENT_REDIS_CHANNEL` | `orbit:events` | Target Redis Pub/Sub channel. |
| `EVENT_REDIS_DATABASE` | `0` | Target Redis database number. |
| `EVENT_REDIS_HOST` | `127.0.0.1` | Target Redis server IP address or hostname. |
| `EVENT_REDIS_PASSWORD` | None | Optional. Redis password for authentication. |
| `EVENT_REDIS_PORT` | `6379` | Target Redis server port. |
| `EVENT_REDIS_USERNAME` | None | Optional. Redis username for ACL-based authentication. |
| `EVENT_KAFKA_BROKERS` | `127.0.0.1:9092` | Comma-separated list of Kafka bootstrap brokers. |
| `EVENT_KAFKA_TOPIC` | `orbit-events` | Target Kafka topic for publishing events. |
| `MAX_CALLS` | `1024` | Optional. Sets the maximum number of calls this server will handle. |

## WebSocket Protocol

The bridge exposes a WebSocket endpoint to interact with SIP media streams directly from web browsers or backend applications.

### 1. Handshake and Authentication

Connect to the bridge via `ws://<WS_EXTERNAL_ADDR>:<WS_EXTERNAL_PORT>/?id=<INTERNAL_ID>`.
The `INTERNAL_ID` is a unique 36-character UUID generated by the bridge when it receives an incoming `INVITE`. This ID is broadcasted via an external eventing system (if configured) so your application knows where to connect.

### 2. JSON Metadata Frame (Server -> Client)

Immediately upon a successful handshake, the server sends a **Text Frame** containing JSON metadata about the stream's negotiated codec.

```json
{
  "type": "metadata",
  "sample_rate": 8000,
  "codec": "PCMU",
  "channels": 1,
  "ptime": 20,
  "endianness": "big",
  "call_id": "call-12345-abcde"
}
```

### 3. JSON DTMF Frame (Client -> Server)

To send a DTMF tone (telephone event) through the active RTP session, the client can send a **Text Frame** to the server:

```json
{
  "type": "dtmf",
  "digit": "5"
}
```

*(Valid digits: `"0"`-`"9"`, `*`, `#`, `"A"`-`"D"`)*

### 4. Binary Audio Frames (Bidirectional)

All media payloads are exchanged as **Binary Frames**.

- **Client to Server**: Send raw audio payload bytes matching the negotiated codec (e.g., PCMU, OPUS). The server will automatically prepend the correct 12-byte RTP header and monotonically increasing sequence/timestamp data before dispatching it to the SIP endpoint via UDP.
- **Server to Client**: The server intercepts incoming RTP packets, strips the 12-byte RTP header (and any extensions), and forwards purely the audio payload bytes to the client as a binary WebSocket frame.

## Event System & Schemas

The bridge broadcasts real-time call lifecycle events as raw JSON payloads. Depending on the configured `EVENT_PROVIDER`, these events are delivered via:

- **UDP:** Broadcasted as raw UDP datagrams to `EVENT_UDP_LISTEN_ADDR` / `EVENT_UDP_LISTEN_PORT`.
- **Redis:** Published to the Redis Pub/Sub channel specified by `EVENT_REDIS_CHANNEL`.
- **Kafka:** Produced asynchronously to the Kafka topic specified by `EVENT_KAFKA_TOPIC`.

This enables external application servers (or the load test client) to dynamically trace call lifecycles and trigger WebSocket connections.

### `call_answered`

Fired when the bridge successfully processes a new inbound SIP `INVITE` and allocates a session. It provides the exact WebSocket URL (including the generated Internal ID) needed to connect to the audio stream.

```json
{
  "event": "call_answered",
  "call_id": "b39a8c12-7f...",
  "from": "alice_xyz",
  "to": "bob_123",
  "x_headers": {
    "X-Tenant-ID": "corp-55",
    "X-Route-Rule": "sales"
  },
  "ws_url": "ws://127.0.0.1:8080/media?id=c7924c52-0fba-4b..."
}
```

## Development and Debugging

To compile with debug symbols and `LOGDBG` verbosity, build the application without the release flag, and CMake will automatically expose the traces:

```bash
cmake -B build
cmake --build build
```

The repository includes a rigid formatting script targeting `clang-format` to enforce the codebase's strict East-Const C23 guidelines:

```bash
./scripts/format.sh
```

## License

Orbit is released under the **GNU Affero General Public License v3 (AGPL-3.0)**.

This ensures that the software remains free and open source, even when used as a network service. Any modifications to this codebase must be made publicly available under the same license. For more details, see the [LICENSE](./LICENSE) file.
