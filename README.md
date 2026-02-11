# e2sar-utils

A C++ utility library for e2sar project.

## Dependencies

### Build Dependencies
- C++17 compatible compiler
- Meson build system (>= 0.55.0)
- Boost (>= 1.89.0)
- pthreads

### Runtime Dependencies
- gRPC (>= 1.74.1)
- Protocol Buffers

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
2. Runs the sender with parallel file processing
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
├── include/       # Public headers
├── src/          # Source files
├── tests/        # Unit tests
├── docs/         # Documentation
└── build/        # Build directory (generated)
```

## License

TBD
