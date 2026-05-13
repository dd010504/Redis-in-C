# Redis-in-C

Building my own Redis server from scratch in C.

A minimalist, Redis-compatible key/value store built as a learning project
for low-level systems programming (custom data structures, non-blocking I/O,
epoll-based event loops).

## Status

**Phase 1 - The Brain:** binary-safe chained hash table (DJB2, 1024 fixed
buckets) with `SET` / `GET` / `DEL`. Done.

**Phase 2 - The Skeleton:** single-threaded, level-triggered `epoll(7)`
event loop accepting TCP connections on `127.0.0.1:6379` and echoing bytes
back to the client. RESP framing and hashtable dispatch are the next steps.

## Layout

```
Redis-in-C/
├── CMakeLists.txt
├── src/
│   ├── hashtable.h    # binary-safe API + memory-ownership contract
│   ├── hashtable.c    # DJB2 + chained buckets, every malloc/free documented
│   ├── networking.h   # epoll loop public API
│   ├── networking.c   # level-triggered epoll echo loop (Phase 2 step 1)
│   └── main.c         # smoke test for SET/GET/DEL, then enters the loop
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

# Run: prints the Phase 1 smoke test, then blocks on epoll_wait()
# until you Ctrl-C.
./build/redis-in-c
```

`CMAKE_EXPORT_COMPILE_COMMANDS` is forced on, so `build/compile_commands.json`
is generated automatically -- symlink or copy it to the repo root if your
LSP (clangd, ccls) looks for it there.

## Phase 2 (in progress) - epoll echo loop

Once the smoke test prints, the server starts listening on
`127.0.0.1:6379`. In another terminal:

```bash
# Echo test with netcat:
nc 127.0.0.1 6379
hello
hello        # <-- echoed back by the server
^D           # close the connection

# Or test multiple simultaneous clients:
( nc 127.0.0.1 6379 ) &
( nc 127.0.0.1 6379 ) &
```

In the server terminal you should see `[networking] accepted fd=...` and
`[networking] closed fd=...` log lines. `Ctrl-C` (SIGINT) triggers a clean
shutdown: every live connection is closed, every `conn_t` is freed, and
the epoll/listening fds are released before `main` returns.

The bind address is currently hardcoded to loopback for safety. To change
the port today, edit `LISTEN_PORT` at the top of [`src/networking.c`](src/networking.c)
and rebuild. A proper CLI flag lands alongside the RESP parser.

### Build types

The default build type is `RelWithDebInfo`. To switch:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Leak check (optional)

Both the hashtable and the networking layer free every allocation on exit,
so a `valgrind` run that includes connecting, disconnecting, and finally
SIGINT-ing the server should report no leaks:

```bash
valgrind --leak-check=full --error-exitcode=1 ./build/redis-in-c
# In another terminal:
#   nc 127.0.0.1 6379  -> type stuff, ^D
# Then SIGINT (Ctrl-C) the server in the valgrind terminal.
# Expected: "All heap blocks were freed -- no leaks are possible"
```

## Design notes

- **Binary-safe keys/values.** Every public hashtable function takes
  `(const void *, size_t)` pairs, so embedded NULs round-trip cleanly and
  the Phase 2 RESP parser can slice straight into `ht_set`.
- **Caller-owned struct.** `hashtable_t` lives wherever the caller puts it
  (BSS in `main.c` today). `ht_init` does not allocate; `ht_destroy` frees
  every internal entry but never the struct itself.
- **Three malloc()s per insert, all accounted for.** See the allocation
  table at the top of [`src/hashtable.c`](src/hashtable.c).
- **Single-threaded.** No locks anywhere. The epoll loop in
  [`src/networking.c`](src/networking.c) owns the store outright; once RESP
  dispatch lands, command handling will happen inline with I/O.
- **Level-triggered epoll, half-duplex echo.** Each connection holds a
  fixed 4 KiB buffer and toggles between `EPOLLIN` and `EPOLLOUT` interest
  so no growable per-connection queue is needed yet. See the allocation
  table at the top of [`src/networking.c`](src/networking.c).

## License

See [LICENSE](LICENSE).
