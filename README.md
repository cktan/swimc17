# swimc17

A C17 library implementing the SWIM gossip protocol.

## Documentation

For detailed information on design, algorithms, and usage,
see the following documents:

- [USAGE.md](USAGE.md):
  Installation instructions, public API reference, and
  usage examples.
- [DESIGN.md](DESIGN.md):
  Protocol details, design decisions, and architecture.
- [ALGORITHM.md](ALGORITHM.md):
  A step-by-step sketch of the SWIM failure detector,
  gossip, and refutation rules.


## Building

For debug build:
```bash
export DEBUG=1
make
```

For release build:
```bash
unset DEBUG
make
```

## Running tests

The following command invokes the tests:

```bash
export DEBUG=1
make test
```
