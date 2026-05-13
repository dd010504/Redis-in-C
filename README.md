# Redis-in-C

Building my own Redis server from scratch in C.

A minimalist, Redis-compatible key/value store built as a learning project
for low-level systems programming (custom data structures, non-blocking I/O,
epoll-based event loops).

## Status

**Phase 1 - The Brain:** binary-safe chained hash table (DJB2, 1024 fixed
buckets) with `SET` / `GET` / `DEL`. Done.

**Phase 2 - The Skeleton:** single-threaded, level-triggered `epoll(7)`
event loop on `127.0.0.1:6379` with full RESP2 wire protocol. Commands
implemented: `PING`, `GET`, `SET`, `DEL`, `EXISTS`, `COMMAND`. Talks to
`redis-cli` end-to-end. Done.

## Layout

```
Redis-in-C/
├── CMakeLists.txt
├── src/
│   ├── hashtable.h    # binary-safe API + memory-ownership contract
│   ├── hashtable.c    # DJB2 + chained buckets, every malloc/free documented
│   ├── networking.h   # epoll loop public API
│   ├── networking.c   # level-triggered epoll loop with command dispatch
│   ├── resp.h         # RESP2 parser + reply serializers
│   ├── resp.c
│   ├── bytebuf.h      # growable byte buffer (per-connection read/write)
│   ├── bytebuf.c
│   ├── command.h      # command dispatcher
│   ├── command.c      # PING / GET / SET / DEL / EXISTS / COMMAND
│   └── main.c         # smoke tests, then enters the epoll loop
└── README.md
```

## Build & run (Linux / WSL2)

Prerequisites: `cmake >= 3.16`, `ninja-build`, and a C11-capable compiler
(`gcc` or `clang`).

```bash
# Install toolchain on Debian/Ubuntu (including WSL2):
sudo apt-get install -y build-essential cmake ninja-build

# Configure (creates build/ and build/compile_commands.json):
cmake -S . -B build -G Ninja

# Build:
cmake --build build

# Run: prints the three startup smoke tests (hashtable, RESP parser,
# command dispatcher), then blocks on epoll_wait() until you Ctrl-C.
./build/redis-in-c
```

`CMAKE_EXPORT_COMPILE_COMMANDS` is forced on, so `build/compile_commands.json`
is generated automatically -- symlink or copy it to the repo root if your
LSP (clangd, ccls) looks for it there.

## Phase 2 - talking to `redis-cli`

Once the three smoke tests print all-OK, the server listens on
`127.0.0.1:6379`. In another terminal:

```bash
redis-cli -p 6379
127.0.0.1:6379> PING
PONG
127.0.0.1:6379> SET foo bar
OK
127.0.0.1:6379> GET foo
"bar"
127.0.0.1:6379> EXISTS foo nope
(integer) 1
127.0.0.1:6379> DEL foo
(integer) 1
127.0.0.1:6379> GET foo
(nil)
127.0.0.1:6379> XYZ
(error) ERR unknown command 'XYZ'
```

`PING` also accepts an argument and echoes it as a bulk string, matching
real Redis:

```bash
127.0.0.1:6379> PING "hello world"
"hello world"
```

The server prints `[networking] accepted fd=...` / `closed fd=...` log
lines as clients come and go. `Ctrl-C` (SIGINT) triggers a clean
shutdown: every live connection is closed, every per-connection buffer
is freed, and the epoll/listening fds are released before `main`
returns.

### Pipelining sanity check

Newline-separated commands on a single connection get pipelined and all
replies come back in order:

```bash
redis-cli -p 6379 -r 5 PING
# 5 PONGs over one connection

printf '*1\r\n$4\r\nPING\r\n*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$1\r\n1\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n' \
  | nc -q1 127.0.0.1 6379
# +PONG, +OK, $1 1 -- three replies back-to-back
```

### Optional: redis-benchmark

If you have `redis-tools` installed (`sudo apt install redis-tools` on
Debian/Ubuntu), you can hammer the server with the real Redis
benchmark:

```bash
redis-benchmark -p 6379 -t set,get -n 100000 -c 50 -q
```

Numbers will be modest -- we're single-threaded with no fancy
optimizations -- but it's a fun sanity check.

### Changing the bind address

Loopback-only on port 6379 is hardcoded for now. Edit `LISTEN_HOST` /
`LISTEN_PORT` at the top of [`src/networking.c`](src/networking.c) and
rebuild. A proper CLI flag lands in Phase 6 (ops).

### Build types

The default build type is `RelWithDebInfo`. To switch:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Leak check (optional)

The hashtable, the per-connection bytebufs, and the conn_t structs all
free on exit, so a `valgrind` run that includes connecting, issuing a
few commands, disconnecting, and finally SIGINT-ing the server should
report no leaks:

```bash
valgrind --leak-check=full --error-exitcode=1 ./build/redis-in-c
# In another terminal:
#   redis-cli -p 6379
#   > SET foo bar
#   > GET foo
#   > quit
# Then SIGINT (Ctrl-C) the server in the valgrind terminal.
# Expected: "All heap blocks were freed -- no leaks are possible"
```

## Design notes

- **Binary-safe keys/values end-to-end.** The hashtable, the RESP
  parser, and the dispatcher all carry `(const void *, size_t)` pairs,
  so embedded NULs round-trip from the socket to the store and back.
- **Caller-owned hashtable struct.** `hashtable_t g_store` lives in
  BSS in [`src/main.c`](src/main.c). `ht_init` does not allocate;
  `ht_destroy` frees every internal entry but never the struct itself.
- **Single-threaded.** No locks anywhere. The epoll loop in
  [`src/networking.c`](src/networking.c) owns the store outright;
  command dispatch happens inline with I/O.
- **Level-triggered epoll.** `EPOLLIN` is armed at all times during
  normal operation; `EPOLLOUT` is armed iff a connection has unsent
  reply bytes. No half-duplex throttling -- pipelined requests fire
  back-to-back replies in a single event loop turn.
- **Growable per-connection buffers.** Each connection holds two
  `bytebuf_t` (in/out). Doubling growth up to 16 MiB; lazy head
  compaction once the consumed prefix is large. See
  [`src/bytebuf.c`](src/bytebuf.c) for the allocation table.
- **Every malloc has a documented free.** Allocation tables at the
  top of [`src/hashtable.c`](src/hashtable.c),
  [`src/bytebuf.c`](src/bytebuf.c), and
  [`src/networking.c`](src/networking.c) name every alloc site and
  its matching free.

## License

See [LICENSE](LICENSE).
