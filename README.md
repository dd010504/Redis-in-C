# Redis-in-C

Building my own Redis server from scratch in C.

A minimalist, Redis-compatible key/value store built as a learning project
for low-level systems programming (custom data structures, non-blocking I/O,
epoll-based event loops).

## Status

**Phase 1 - The Brain:** binary-safe chained hash table (DJB2, 1024 fixed
buckets) with `SET` / `GET` / `DEL`. Done.

**Phase 2 - The Skeleton:** placeholder `networking.c` ready to grow into
a single-threaded epoll event loop. In progress.

## Layout

```
Redis-in-C/
├── CMakeLists.txt
├── src/
│   ├── hashtable.h    # binary-safe API + memory-ownership contract
│   ├── hashtable.c    # DJB2 + chained buckets, every malloc/free documented
│   ├── networking.h   # epoll loop placeholder (Phase 2)
│   ├── networking.c
│   └── main.c         # smoke test for SET/GET/DEL, wires networking stubs
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

# Run the Phase 1 smoke test:
./build/redis-in-c
```

`CMAKE_EXPORT_COMPILE_COMMANDS` is forced on, so `build/compile_commands.json`
is generated automatically -- symlink or copy it to the repo root if your
LSP (clangd, ccls) looks for it there.

### Build types

The default build type is `RelWithDebInfo`. To switch:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Leak check (optional)

The smoke test allocates and frees on exit, so once `valgrind` is installed
you can verify the hashtable contract directly:

```bash
valgrind --leak-check=full --error-exitcode=1 ./build/redis-in-c
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
- **Single-threaded.** No locks anywhere. The future epoll loop will own
  the store outright.

## License

See [LICENSE](LICENSE).
