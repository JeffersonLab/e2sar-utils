# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

e2sar-utils is a C++ utility library for the EJFAT (Experimental Data Flow Architecture Test) project, focused on processing particle physics data from ROOT files and streaming it over the network via E2SAR segmentation.

**Domain Context:** Particle physics data analysis, specifically Dalitz decay analysis with pions and photons (π+π-γγ events).

**Current State:** Full streaming implementation with E2SAR network transmission, optimized for minimal memory usage and proper data alignment.

## Build System: Meson

This project uses Meson (not CMake). All build configuration is in `meson.build` files.

### Common Build Commands

```bash
# Initial setup or reconfigure after meson.build changes
meson setup --reconfigure build/

# Compile
meson compile -C build/

# Run tests (when implemented)
meson test -C build/

# View all build options
meson configure build/

# Change build options
meson configure build/ -Denable_tests=false -Dbuildtype=release
```

### Build Options (meson.options)
- `enable_tests`: Build tests (default: true)
- `enable_examples`: Build examples (default: false)
- `enable_docs`: Build documentation (default: false)

## Dependencies

### Required at Build Time
- **Boost** (≥1.89.0): Using multiple modules:
  - `program_options`: CLI parsing
  - `url`, `log`, `thread`, `chrono`: Required by E2SAR
- **ROOT** (particle physics library): Discovered via pkg-config, falls back to root-config
  - Components used: Core, RIO, Tree
  - Classes: TFile, TTree, TLorentzVector, TVector3
- **gRPC** (≥1.74.1) and **Protocol Buffers**: For E2SAR communication
- **E2SAR** (≥0.1.5): EJFAT Event Segmentation And Reassembly
  - Installed location: `/Users/baldin/workspaces/workspace-ejfat/e2sar-install`
  - Project links against libe2sar.a
  - Requires: Boost.URL, Boost.Log, Boost.Thread, Boost.Chrono
  - Test executable: `tests/test_e2sar.cpp` verifies proper E2SAR installation and linking

### Dependency Discovery Strategy

**ROOT dependency** uses a two-tier approach in `meson.build`:
1. First tries pkg-config: `dependency('ROOT', method: 'pkg-config', components: ['Core', 'RIO', 'Tree'])`
2. Falls back to root-config if pkg-config fails
3. This pattern ensures compatibility across different ROOT installations

**E2SAR dependency** uses explicit path configuration:
```python
e2sar_install_root = '/Users/baldin/workspaces/workspace-ejfat/e2sar-install'
e2sar_dep = declare_dependency(
  include_directories: include_directories(e2sar_install_root / 'include'),
  dependencies: [boost_url_dep, boost_log_dep, boost_thread_dep, boost_chrono_dep],
  link_args: ['-L' + e2sar_install_root / 'lib', '-le2sar'],
  version: '0.1.5'
)
```

## Architecture

### Current Implementation: Streaming ROOT → E2SAR Pipeline

**Executable:** `root-reader` (src/root_reader.cpp)

Full-featured streaming tool that:
1. Reads ROOT files with particle physics events
2. Processes events in configurable batches (streaming architecture)
3. Serializes events to binary format with proper alignment
4. Transmits via E2SAR segmentation to network destinations

### Key Features

#### 1. Streaming Architecture (Memory Optimized)

**Problem Solved:** Original approach would buffer all 2.5M events (320 MB) before sending.

**Solution:** Stream events in configurable batches:
- Read batch (N MB) → Serialize → Send → Repeat
- Default batch size: 10 MB (~81,920 events)
- Memory usage: ~20-30 MB (15x reduction from 320 MB)

**Memory Alignment Optimization:**
- Uses `std::vector<double>` for batch storage (ensures 8-byte alignment)
- Each event = 16 doubles (4 particles × 4-vector each = 128 bytes)
- Events appended directly via `push_back()` (no intermediate copying)
- Buffer passed to E2SAR via `data()` method (zero-copy)

#### 2. E2SAR Integration

- Configurable MTU (default: 1500 bytes, supports jumbo frames up to 9000)
- Automatic segmentation into network frames
- Retry logic for queue-full conditions
- Completion tracking and statistics

#### 3. Command-Line Interface

```bash
./root-reader --tree <tree_name> [OPTIONS] <file.root>

Required:
  -t, --tree <name>       Tree name to process (e.g., dalitz_root_tree)
  <file.root>             ROOT file path(s)

E2SAR Sending:
  -s, --send              Enable network sending
  -u, --uri <ejfat_uri>   EJFAT URI (format: ejfat://token@host:port/lb/N?data=ip:port)
  --dataid <N>            Data ID for E2SAR (default: 1)
  --eventsrcid <N>        Event source ID (default: 1)
  --bufsize-mb <N>        Batch size in MB (default: 10)
  --mtu <N>               MTU in bytes (default: 1500, range: 576-9000)
```

### Data Structures

#### DalitzEvent (lines 30-35)
```cpp
struct DalitzEvent {
    TLorentzVector pi_plus;    // π+ (positive pion)
    TLorentzVector pi_minus;   // π- (negative pion)
    TLorentzVector gamma1;     // γ1 (first photon)
    TLorentzVector gamma2;     // γ2 (second photon)
};
// Serialized size: 128 bytes (16 doubles × 8 bytes)
```

#### StreamingStats (lines 38-58)
Tracks progress during streaming:
- Total events processed
- Batches sent
- E2SAR buffers sent
- Total bytes transmitted

### Processing Pipeline

```
ROOT File (TFile)
  ↓
Tree (TTree) - 2.5M entries
  ↓
Read event branches (mag, theta, phi for each particle)
  ↓
Create TLorentzVector for each particle (4 particles)
  ↓
Append 16 doubles to batch vector (aligned memory)
  ↓
When batch full (81,920 events):
  ↓
  Pass vector.data() to E2SAR Segmenter
    ↓
    Segment into MTU-sized frames
      ↓
      Transmit over UDP to load balancer
        ↓
        Delete batch vector (callback)
  ↓
Allocate new batch, continue
```

### Code Organization

```
src/
  root_reader.cpp          # Main executable (561 lines)
    - CommandLineArgs      # CLI configuration struct
    - DalitzEvent          # Event data structure
    - StreamingStats       # Progress tracking
    - createLorentzVector  # Spherical → Cartesian conversion
    - appendEventToVector  # Serialize event to vector<double>
    - freeBuffer           # Callback to delete sent batches
    - initializeSegmenter  # E2SAR setup and initialization
    - parseArgs            # Boost::program_options CLI parsing
    - processRootFile      # Main streaming loop
  meson.build             # Executable build config

include/                  # Public headers (future library APIs)

tests/
  test_e2sar.cpp         # E2SAR integration verification
  read_dalitz_root.py    # Python reference implementation
  meson.build            # Test framework setup
```

## ROOT File Processing

### Tree Structure
ROOT files contain `TTree` objects with event data:
- Tree name: `dalitz_root_tree` (parameterized via --tree flag)
- Branches per event:
  - `mag_plus_rec`, `theta_plus_rec`, `phi_plus_rec` (π+)
  - `mag_neg_rec`, `theta_neg_rec`, `phi_neg_rec` (π-)
  - `mag_neutral1_rec`, `theta_neutral1_rec`, `phi_neutral1_rec` (γ1)
  - `mag_neutral2_rec`, `theta_neutral2_rec`, `phi_neutral2_rec` (γ2)
- Particle masses: π± = 0.139 GeV/c², γ = 0.0 GeV/c²

### Binary Serialization Format

Each event serialized as 16 doubles (128 bytes):
```
[ π+ (E, px, py, pz) ][ π- (E, px, py, pz) ][ γ1 (E, px, py, pz) ][ γ2 (E, px, py, pz) ]
  4 doubles              4 doubles              4 doubles              4 doubles
```

## Testing

### Test Data
Location: `dalitz_toy_data_0/dalitz_root_file_0.root`
- Size: 543 MB
- Events: 2,572,650 entries
- Tree: `dalitz_root_tree`

### Common Test Commands

```bash
# Read and process without sending (verify processing)
./build/src/root-reader --tree dalitz_root_tree ./dalitz_toy_data_0/dalitz_root_file_0.root

# Stream via E2SAR with default settings (10 MB batches, MTU 1500)
./build/src/root-reader -t dalitz_root_tree --send \
  -u "ejfat://token@localhost:12345/lb/1?data=127.0.0.1:19522" \
  ./dalitz_toy_data_0/dalitz_root_file_0.root

# Small batches (1 MB) for testing
./build/src/root-reader -t dalitz_root_tree --send \
  -u "ejfat://token@localhost:12345/lb/1?data=127.0.0.1:19522" \
  --bufsize-mb 1 \
  ./dalitz_toy_data_0/dalitz_root_file_0.root

# Jumbo frames (9000 MTU) with larger batches (20 MB)
./build/src/root-reader -t dalitz_root_tree --send \
  -u "ejfat://token@localhost:12345/lb/1?data=127.0.0.1:19522" \
  --bufsize-mb 20 --mtu 9000 \
  ./dalitz_toy_data_0/dalitz_root_file_0.root

# Test help
./build/src/root-reader --help

# Test error handling
./build/src/root-reader --tree nonexistent_tree ./dalitz_toy_data_0/dalitz_root_file_0.root
```

### E2SAR Integration Test

```bash
# Verify E2SAR library linking and basic functionality
./build/tests/test-e2sar

# Expected output:
# - E2SAR headers included successfully
# - EjfatURI created and parsed
# - E2SAR integration successful
```

### Loopback Integration Test (REQUIRED after code changes)

**Always run this test after modifying e2sar_root.cpp:**

```bash
# Run the full loopback test (sender + receiver)
./tests/test_loopback.sh

# With custom options for more thorough testing
./tests/test_loopback.sh --timeout 60 --bufsize 2 --files 3
```

The test:
1. Starts receiver on loopback interface (127.0.0.1)
2. Runs sender with parallel file processing (2 files by default)
3. Verifies all buffers sent == files received
4. Reports PASS/FAIL

**Options:**
| Option | Default | Description |
|--------|---------|-------------|
| `--timeout N` | 30 | Receiver wait time in seconds |
| `--bufsize N` | 1 | Batch size in MB |
| `--mtu N` | 9000 | MTU size (jumbo frames) |
| `--files N` | 2 | Number of parallel file streams |

**Expected output on success:**
```
[INFO] ========== Test Results ==========
  Buffers sent:     630
  Files received:   630
  Send errors:      0

[INFO] TEST PASSED - All buffers received successfully
```

## Performance Characteristics

### Memory Usage
- **Before streaming refactor:** 320 MB (all events buffered)
- **After streaming (10 MB batches):** ~20-30 MB (15x reduction)
- **With 1 MB batches:** ~5-10 MB
- **With 20 MB batches:** ~30-40 MB

### Batch Size Calculations
```
Batch size in events = (bufsize_mb × 1024 × 1024) / 128
Default (10 MB) = 81,920 events

Examples:
- 1 MB  = 8,192 events
- 10 MB = 81,920 events (default)
- 20 MB = 163,840 events
```

### E2SAR Buffer Packing
```
MTU = 1500 (default)
Max payload ≈ 1472 bytes (from segmenter.getMaxPldLen())
Events per network frame ≈ 11 events

For jumbo frames (MTU 9000):
Max payload ≈ 8972 bytes
Events per network frame ≈ 70 events
```

## Key Implementation Details

### Memory Alignment
- Batch storage uses `std::vector<double>` for 8-byte alignment
- Critical for CPU cache efficiency
- Avoids alignment issues when casting to byte buffers

### Zero-Copy Pattern
- Events appended directly to vector via `appendEventToVector()`
- No intermediate malloc/free cycles
- Buffer pointer obtained via `batch->data()`
- Passed directly to E2SAR with callback for cleanup

### Dynamic Allocation Strategy
- Each batch allocated with `new std::vector<double>()`
- Reserved capacity: `BATCH_SIZE_EVENTS × 16` doubles
- Deleted in callback after E2SAR transmission complete
- Prevents accumulation of batches in memory

### Error Handling
- Queue full: Retry with exponential backoff (max 10,000 retries)
- Send errors: Cleanup and return failure
- Invalid URI: Error reported during segmenter initialization
- File/tree not found: Clear error messages

## Adding New Features

### Adding Boost Modules
```python
# In meson.build
boost_new_dep = dependency('boost', modules: ['new_module'])

# Add to project_deps or specific dependency list
```

### Adding ROOT Components
```python
# In meson.build (lines 26-41)
# Update components list in pkg-config dependency
root_dep = dependency('ROOT',
                      method: 'pkg-config',
                      components: ['Core', 'RIO', 'Tree', 'NewComponent'])
```

### Modifying Event Structure
If changing DalitzEvent:
1. Update struct definition (lines 30-35)
2. Update `appendEventToVector()` to append correct number of doubles
3. Update `EVENT_SIZE` constant if total size changes
4. Update serialization in `serializeEvent()` if backward compatibility needed

## Project Context: EJFAT/e2sar

This utility is part of the EJFAT (Experimental Data Flow Architecture Test) project ecosystem for streaming particle physics event data over networks.

**Current Data Flow:**
```
ROOT files → Event extraction → TLorentzVector creation →
Binary serialization → Batch buffering → E2SAR segmentation →
UDP transmission → Load balancer → Data plane endpoints
```

**E2SAR Role:**
- Segments large data buffers into MTU-sized network frames
- Adds frame headers for reassembly
- Manages send queues and transmission threads
- Provides statistics on transmitted frames

**Future Enhancements:**
- Physics analysis (invariant masses, kinematic checks)
- Multiple file processing patterns
- Compression options
- Receiver/reassembly utilities
