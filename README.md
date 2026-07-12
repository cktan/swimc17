# swimc17

[![CI](https://github.com/cktan/swimc17/actions/workflows/nix.yml/badge.svg)](https://github.com/cktan/swimc17/actions/workflows/nix.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A cluster membership library for C, implementing the SWIM
gossip protocol (SWIM+INF+Susp). Nodes discover each other
over UDP, detect failures, and disseminate membership
changes via gossip — no coordinator, no external
dependencies beyond libc.

## Why swimc17?

Most failure-detector implementations either drag in a
runtime, an allocator of their own, or a dependency tree
that doesn't belong in a C project. swimc17 doesn't: it's
plain C17 that links against nothing but libc, pthread,
and libm, and builds down to a single static library.

- **Zero external dependencies** — no libuv, no
  third-party JSON/serialization library, no vendored
  crypto. Packet authentication is done with an inline
  ~50-line SipHash-2-4, not a linked crypto library.
- **Small, bounded footprint** — a 1000-node membership
  list is about 340 KB; see `DESIGN.md` §17 for the full
  resource accounting.
- **Thread-safe by design** — every public call except
  the feed API is safe to call concurrently; each
  `swim_t` instance owns its own mutex, and there's no
  global lock.
- **UDP only, IPv4 + IPv6**, capped at a conservative
  1400-byte MTU so it works unmodified across typical
  networks.
- **Built-in packet authentication** — every packet
  carries a SipHash-2-4 MAC plus a timestamp window, so
  nodes without the shared group secret can't inject
  gossip or forge membership changes.
- **No logging dependency** — telemetry goes out through
  a caller-owned feed queue that you can drain, buffer, or
  pipe into whatever logging you already use.
- **Tested at scale** — unit tests, property tests, and
  64-node stress tests with simulated packet loss, delay,
  and reordering via an in-memory transport.

## Quick example

```c
#include "swim.h"
#include <stdio.h>

int main(void) {
    swim_feed_t *feed = swim_feed_create();

    swim_start_opts_t opts = {0};
    opts.name  = "my_cluster";   // shared secret + group name
    opts.feed  = feed;
    const char *seeds[] = { "10.0.0.1:7771/c1", NULL };
    opts.seeds = seeds;

    swim_t *inst = swim_start("10.0.0.7:7771/c1", &opts);
    if (!inst) {
        fprintf(stderr, "swim_start: %s\n", swim_errmsg());
        return 1;
    }

    char buf[SWIM_FEED_MAX_RECORD_SIZE];
    char *ptr[SWIM_FEED_MAX_ELEMENTS];
    int n;
    while ((n = swim_feed_get(feed, sizeof(buf), buf, 10, ptr)) >= 0) {
        swim_feed_wait(feed, 1000);
        while (n-- > 0) { /* ptr[0..n-1] is one telemetry record */ }
    }

    swim_leave(inst);
    swim_feed_destroy(feed);
}
```

Link with:

```bash
cc my_app.c -lswimc17 -lpthread -lm
```

## Building

```bash
# debug build
export DEBUG=1
make

# release build
unset DEBUG
make
```

## Installing

```bash
make install prefix=/usr/local
```

Installs `swim.h`, `libswimc17.a`, and a pkg-config file,
so downstream projects can build with:

```bash
cc my_app.c $(pkg-config --cflags --libs libswimc17)
```

## Running tests

```bash
export DEBUG=1
make test
```

## Documentation

- [USAGE.md](USAGE.md) — installation, API reference,
  and usage examples.
- [DESIGN.md](DESIGN.md) — protocol details, design
  decisions, and architecture.
- [ALGORITHM.md](ALGORITHM.md) — a step-by-step sketch
  of the SWIM failure detector, gossip, and refutation
  rules.

## Non-goals

swimc17 tells you who is up and who is down — nothing
more. It does not do data replication, leader election,
distributed locking, or reliable message delivery. See
`DESIGN.md` §15 for the full list.

## License

MIT — see [LICENSE](LICENSE).
