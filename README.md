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
Sender:   e2sar-root --toy|--gluex --tree <tree_name> --send --uri <ejfat_uri> [OPTIONS] <file1.root> ...
Receiver: e2sar-root --recv --uri <ejfat_uri> --recv-ip <ip> [OPTIONS]
```

### Key Options

| Option | Description |
|--------|-------------|
| `--toy` | Use Dalitz toy-MC schema |
| `--gluex` | Use GlueX kinematic-fit schema |
| `-t, --tree <name>` | ROOT tree name to read |
| `-s, --send` | Enable E2SAR network sending |
| `-r, --recv` | Enable E2SAR network receiving |
| `-u, --uri <uri>` | EJFAT URI |
| `--bufsize-mb N` | Batch size in MB (default: 10) |
| `--mtu N` | MTU in bytes (default: 1500, max: 9000) |
| `--recv-ip <ip>` | IP address for receiver |
| `-o, --output-pattern` | Output filename pattern (default: `event_{:08d}.dat`) |

### Examples

```bash
# Read-only (verify file, no network)
./build/bin/e2sar-root --toy  --tree dalitz_root_tree file.root
./build/bin/e2sar-root --gluex --tree myTree file.root

# Send toy-MC data
./build/bin/e2sar-root --toy -t dalitz_root_tree --send \
  -u "ejfat://token@host:port/lb/1?data=ip:port" --bufsize-mb 5 file.root

# Send GlueX data with jumbo frames
./build/bin/e2sar-root --gluex -t myTree --send \
  -u "ejfat://token@host:port/lb/1?data=ip:port" --mtu 9000 file.root

# Receive events
./build/bin/e2sar-root --recv \
  -u "ejfat://token@host:port/lb/1?data=ip:port" \
  --recv-ip 127.0.0.1 -o output_{:06d}.dat
```

## Testing

After making code changes, always run the loopback integration test:

```bash
# Run the loopback test (sender + receiver over localhost)
./tests/test_loopback.sh

# With custom options
./tests/test_loopback.sh --timeout 60 --bufsize 2 --files 3
```

The test script:
1. Starts a receiver on the loopback interface
2. Runs the sender (`--toy` schema) with parallel file processing
3. Verifies all buffers were received without errors
4. Reports PASS/FAIL status

**Options:**
- `--timeout N` - Receiver timeout in seconds (default: 30)
- `--bufsize N` - Batch size in MB (default: 1)
- `--mtu N` - MTU size (default: 9000)
- `--files N` - Number of parallel file streams (default: 2)

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
