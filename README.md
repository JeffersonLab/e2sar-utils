# e2sar-utils

A C++ utility for reading particle physics ROOT files and streaming event data over the network via E2SAR segmentation. Can use simluated Dalitz or GlueX data and extract different types of physics events and send them on the wire. The receive end is for testing only - simply saves every EJFAT event (containing multiple physics events) into a file. 

## Supported Event Schemas

| Flag | Tree name | Event type | Doubles/event |
|------|-----------|------------|---------------|
| `--toy` | `dalitz_root_tree` | Dalitz toy-MC (π+π-γγ, spherical coords) | 16 |
| `--gluex` | `myTree` | GlueX kinematic-fit (π+π-γγ + kfit scalars) | 19 |

**Exactly one of `--toy` or `--gluex` is required** for any sender or read-only invocation.

## Dependencies

### Build Dependencies
- C++17 compatible compiler
- Meson build system (>= 0.55.0)
- Boost (>= 1.89.0)
- pthreads

### Runtime Dependencies
- gRPC (>= 1.74.1)
- Protocol Buffers
- ROOT (particle physics library)
- E2SAR (>= 0.1.5)

## Building

```bash
# Configure the build
meson setup build/

# Compile
meson compile -C build/

# Run tests (if enabled)
meson test -C build/

# Install
meson install -C build/
```

## Usage

```
Sender (files): e2sar-root --toy|--gluex --tree <name> --send --uri <uri> [--parallel N] [OPTIONS] <file1.root> ...
Sender (dir):   e2sar-root --toy|--gluex --tree <name> --send --uri <uri> --dir <dir> [--parallel N] [OPTIONS]
Receiver:       e2sar-root --recv --uri <uri> --recv-ip <ip> [OPTIONS]
Read-only:      e2sar-root --toy|--gluex --tree <name> [--dir <dir>] [<file.root> ...]
```

### Key Options

| Option | Default | Description |
|--------|---------|-------------|
| `--toy` | — | Use Dalitz toy-MC schema |
| `--gluex` | — | Use GlueX kinematic-fit schema |
| `-t, --tree <name>` | — | ROOT tree name to read |
| `-s, --send` | false | Enable E2SAR network sending |
| `-r, --recv` | false | Enable E2SAR network receiving |
| `-u, --uri <uri>` | — | EJFAT URI |
| `--dir <path>` | — | Directory of `*.root` files (alternative to positional files) |
| `--parallel N` | 4 | Max concurrent file streams when sending |
| `--bufsize-mb N` | 10 | Batch size in MB |
| `--mtu N` | 1500 | MTU in bytes (max: 9000) |
| `--rate R` | 1.0 | Send rate in Gbps (negative = no limit) |
| `--numsocks N` | 4 | Number of Segmenter send sockets/threads |
| `--dataid N` | 0 | Data ID passed to E2SAR Segmenter |
| `--recv-ip <ip>` | — | IP address for receiver |
| `--recv-port N` | 19522 | Starting UDP port for receiver |
| `-o, --output-pattern` | `event_{:08d}.dat` | Output filename pattern for received events |
| `--event-timeout N` | 500 | Reassembly timeout in ms |
| `-c, --withcp` | false | Enable control plane interactions |

### Examples

```bash
# Read-only (verify file, no network)
./build/bin/e2sar-root --toy  --tree dalitz_root_tree file.root
./build/bin/e2sar-root --gluex --tree myTree file.root

# Read-only from a directory of ROOT files
./build/bin/e2sar-root --toy --tree dalitz_root_tree --dir data/

# Send a list of files (up to 4 in parallel by default)
./build/bin/e2sar-root --toy -t dalitz_root_tree --send \
  -u "ejfat://token@host:port/lb/1?data=ip:port" \
  --bufsize-mb 5 file0.root file1.root file2.root

# Send all ROOT files from a directory, 8 at a time
./build/bin/e2sar-root --toy -t dalitz_root_tree --send \
  -u "ejfat://token@host:port/lb/1?data=ip:port" \
  --dir data/ --parallel 8 --mtu 9000

# Send GlueX data with jumbo frames, rate-limited
./build/bin/e2sar-root --gluex -t myTree --send \
  -u "ejfat://token@host:port/lb/1?data=ip:port" \
  --mtu 9000 --rate 5.0 file.root

# Receive events
./build/bin/e2sar-root --recv \
  -u "ejfat://token@host:port/lb/1?data=ip:port" \
  --recv-ip 127.0.0.1 -o output_{:06d}.dat
```

### `--dir` and `--parallel`

`--dir <path>` scans the directory non-recursively for `*.root` files (sorted alphabetically) and uses them as the file list. It is mutually exclusive with positional file arguments.

`--parallel N` (default: 4) caps how many files are read and sent concurrently. Files are processed in sequential rounds of N: each round forks N child processes (one per file), sends their data through a shared Segmenter, then reaps all children before starting the next round. After all rounds complete, `stopThreads()` ensures the send queue is fully drained before the process exits.

```bash
# Process 7 files in rounds of 3 (rounds: 3+3+1)
./build/bin/e2sar-root --toy -t dalitz_root_tree --send \
  -u "ejfat://..." --dir data/ --parallel 3 --rate 0.5
```

## Testing

After making code changes, always run the loopback integration test:

```bash
# Default (toy schema, 2 files, parallel=4, rate=unlimited)
./tests/test_loopback.sh --toy

# Rate-limited to avoid UDP loss on loopback (recommended)
./tests/test_loopback.sh --toy --rate 0.5 --timeout 120

# Test directory mode: 7 files processed 3 at a time
./tests/test_loopback.sh --toy --dir-mode --files 7 --parallel 3 \
  --rate 0.5 --timeout 300 --bufsize 2

# GlueX schema
./tests/test_loopback.sh --gluex --rate 0.5 --timeout 120
```

The test script starts a receiver on the loopback interface, runs the sender, waits for all events to arrive, then reports PASS/FAIL.

**Test script options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--toy` / `--gluex` | `--toy` | Event schema |
| `--files N` | 2 | Number of file streams |
| `--parallel N` | 4 | Max concurrent streams (`--parallel` passed to sender) |
| `--dir-mode` | false | Use `--dir` with symlinks instead of positional files |
| `--bufsize N` | 1 | Batch size in MB |
| `--mtu N` | 9000 | MTU size |
| `--rate R` | -1.0 | Send rate in Gbps (-1 = no limit) |
| `--numsocks N` | 4 | Segmenter send sockets |
| `--dataid N` | 0 | Data ID |
| `--timeout N` | 30 | Receiver wait time in seconds |

> **Note on UDP loss:** At uncapped rate (`--rate -1`), the loopback socket buffer can overflow and drop packets, causing reassembly failures. Use `--rate 0.5` for reliable loopback tests.

## Build Options

### Available Options

- `enable_tests`: Build and run tests (default: true)
- `enable_examples`: Build example programs (default: false)
- `enable_docs`: Build documentation (default: false)

### Setting Options

**During initial setup:**
```bash
meson setup build/ -Denable_tests=false -Denable_examples=true
```

**After setup (reconfigure existing build):**
```bash
meson configure build/ -Denable_tests=false -Denable_examples=true
```

**View all options and current values:**
```bash
meson configure build/
```

**Common built-in Meson options:**
```bash
# Change build type (debug, debugoptimized, release)
meson setup build/ -Dbuildtype=release

# Change C++ standard
meson setup build/ -Dcpp_std=c++20

# Combine multiple options
meson setup build/ -Dbuildtype=release -Denable_tests=false -Dcpp_std=c++20
```

After reconfiguring, recompile with:
```bash
meson compile -C build/
```

## Project Structure

```
.
├── include/                  # Public headers (installed under e2sar-utils/)
│   ├── event_data.hpp        # EventData, DalitzEventData, GluexEventData
│   └── file_processor.hpp   # CommandLineArgs, RootFileProcessor hierarchy
├── src/                      # Library sources → libe2sar_utils
│   ├── event_data.cpp        # appendToBuffer / fromBuffer / createLorentzVector
│   └── file_processor.cpp   # RootFileProcessor::process() template method + hooks
├── bin/                      # Executable entry point → e2sar-root
│   └── e2sar_root.cpp        # Signal handling, segmenter/reassembler init, main()
├── tests/                    # Integration tests and ROOT analysis macros
│   └── README.md             # Per-file descriptions
├── docs/                     # Documentation
└── build/                    # Build directory (generated)
```

## License

TBD
