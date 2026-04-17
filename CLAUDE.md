# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

e2sar-utils is a C++ utility library for the EJFAT (Experimental Data Flow Architecture Test) project, focused on processing particle physics data from ROOT files and streaming it over the network via E2SAR segmentation.

**Domain Context:** Particle physics data analysis, specifically Dalitz decay analysis with pions and photons (π+π-γγ events).

## Build System: Meson

This project uses Meson (not CMake). All build configuration is in `meson.build` files.

### Common Build Commands

```bash
# Initial setup or reconfigure after meson.build changes
meson setup --reconfigure build/

# Compile
meson compile -C build/

# Run tests
meson test -C build/

# View all build options
meson configure build/
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

### Dependency Discovery Strategy

**ROOT dependency** uses a two-tier approach in `meson.build`:
1. First tries pkg-config: `dependency('ROOT', method: 'pkg-config', components: ['Core', 'RIO', 'Tree'])`
2. Falls back to root-config if pkg-config fails

**E2SAR dependency** is found via pkg-config using the `.pc` file in the install directory.

## Code Organization

```
include/
  event_data.hpp       # EventData abstract base; DalitzEventData, GluexEventData subclasses
  file_processor.hpp   # CommandLineArgs struct; RootFileProcessor, ToyFileProcessor,
                       #   GluexFileProcessor class hierarchy
  meson.build          # install_headers() for both headers

src/
  event_data.cpp       # appendToBuffer(), fromBuffer(), createLorentzVector()
  file_processor.cpp   # global_buffer_id, cout_mutex; RootFileProcessor::process()
                       #   template method; ToyFileProcessor and GluexFileProcessor hooks
  meson.build          # builds libe2sar_utils (shared library)

bin/
  e2sar_root.cpp       # Signal handling, initializeSegmenter(), initializeReassembler(),
                       #   receiveEvents(), parseArgs(), main()
  meson.build          # builds e2sar-root executable, links against libe2sar_utils

tests/
  test_loopback.sh     # End-to-end integration test (sender + receiver over loopback)
  test_e2sar.cpp       # E2SAR library smoke test
  factored_gluex_analysis.C  # Reference implementation (basis for GluexFileProcessor)
  gluex_event_selection.C    # GlueX event selection with kinematic-fit cuts
  compare_histos.C     # Histogram comparison ROOT macro
  read_dalitz_root.py  # Python reference for Dalitz toy-MC ROOT files
  README.md            # Per-file descriptions
```

## Architecture

### Class Hierarchies

#### EventData (`include/event_data.hpp`, `src/event_data.cpp`)

Abstract base `EventData`:
- `virtual void appendToBuffer(std::vector<double>& buf) const = 0`
- `virtual size_t numDoubles() const = 0`
- `size_t size() const` — non-virtual, returns `numDoubles() * sizeof(double)`

Subclasses:
- **`DalitzEventData`** — 16 doubles (4 particles × 4-vector). Fields: `pi_plus`, `pi_minus`, `gamma1`, `gamma2`. Reads spherical-coordinate branches. Static `fromBuffer(const double*)`.
- **`GluexEventData`** — 19 doubles (16 four-vector + 3 kfit scalars). Fields: `pip`, `pim`, `g1`, `g2`, `imass_kfit`, `imassGG_kfit`, `kfit_prob`. Reads `TLorentzVector*` branches. Static `fromBuffer(const double*)`.

#### RootFileProcessor (`include/file_processor.hpp`, `src/file_processor.cpp`)

Abstract base `RootFileProcessor` (Template Method pattern):
- `process(file_path, tree_name)` — owns the algorithm: file open, batch allocation, E2SAR send/retry loop, statistics. Not overridden.
- Pure-virtual hooks implemented by subclasses: `bindBranches(TTree*)`, `appendEntry(vector<double>&)`, `printSample() const`, `eventSize() const`

Subclasses:
- **`ToyFileProcessor`** — dalitz_root_tree schema, spherical-coord branches (mag/theta/phi), produces `DalitzEventData`
- **`GluexFileProcessor`** — myTree schema, `TLorentzVector*` branches + kfit scalars, produces `GluexEventData`

### Supported Event Schemas

| CLI flag | Tree name | Event type | Doubles/event | Bytes/event |
|----------|-----------|------------|---------------|-------------|
| `--toy` | `dalitz_root_tree` | Dalitz toy-MC (π+π-γγ, spherical coords) | 16 | 128 |
| `--gluex` | `myTree` | GlueX kinematic-fit (π+π-γγ + kfit scalars) | 19 | 152 |

`--toy` and `--gluex` are mutually exclusive; exactly one is required for sender/read-only mode. `--recv` mode is exempt.

### Streaming Architecture

Events are processed in configurable batches to bound memory usage:
- Read N MB of events → serialize to `std::vector<double>` → pass to E2SAR → repeat
- Default batch size: 10 MB
- Memory usage: ~20-30 MB regardless of file size

### Wire Serialization Format

Each event is a flat array of doubles in (E, px, py, pz) order per particle:
```
DalitzEventData: [pip4v][pim4v][g14v][g24v]                           = 16 doubles
GluexEventData:  [pip4v][pim4v][g14v][g24v][imass_kfit][imassGG][prob] = 19 doubles
```

### Command-Line Interface

```
Sender:   e2sar-root --toy|--gluex --tree <name> --send --uri <ejfat_uri> [OPTIONS] <file.root> ...
Receiver: e2sar-root --recv --uri <ejfat_uri> --recv-ip <ip> [OPTIONS]
Read-only: e2sar-root --toy|--gluex --tree <name> <file.root> ...
```

Key options: `--bufsize-mb` (batch size, default 10), `--mtu` (default 1500, max 9000), `--rate` (Gbps), `--withcp` (control plane), `--files` (parallel streams).

## Testing

### Test Data
- **Toy MC:** `dalitz_toy_data_0/dalitz_root_file_0.root` — 543 MB, 2,572,650 events, tree `dalitz_root_tree`
- **GlueX:** `gluex/Reduced_PiPiGG_Tree_030735.root` — tree `myTree`

### Common Test Commands

```bash
# E2SAR smoke test
./build/tests/test-e2sar

# Read-only (verify processing, no network)
./build/bin/e2sar-root --toy  --tree dalitz_root_tree dalitz_toy_data_0/dalitz_root_file_0.root
./build/bin/e2sar-root --gluex --tree myTree gluex/Reduced_PiPiGG_Tree_030735.root

# Loopback integration test (REQUIRED after code changes to bin/ or src/)
./tests/test_loopback.sh --toy
./tests/test_loopback.sh --gluex

# With custom options
./tests/test_loopback.sh --toy --timeout 60 --bufsize 2 --files 3
```

### Loopback Test Options

| Option | Default | Description |
|--------|---------|-------------|
| `--toy` / `--gluex` | `--toy` | Event schema and data file selection |
| `--timeout N` | 30 | Receiver wait time in seconds |
| `--bufsize N` | 1 | Batch size in MB |
| `--mtu N` | 9000 | MTU size (jumbo frames) |
| `--files N` | 2 | Number of parallel file streams |

## Adding New Event Types

1. Add a new subclass of `EventData` in `include/event_data.hpp` and `src/event_data.cpp` with `appendToBuffer()`, `fromBuffer()`, and `numDoubles()`.
2. Add a new subclass of `RootFileProcessor` in `include/file_processor.hpp` and `src/file_processor.cpp` implementing `bindBranches()`, `appendEntry()`, `printSample()`, and `eventSize()`.
3. Add a mutually exclusive `--newschema` CLI flag in `bin/e2sar_root.cpp` and a `make_unique<NewSchemaFileProcessor>` branch in `main()`.
4. Update `tests/test_loopback.sh` to handle the new `--newschema` option.

## Performance Characteristics

### Memory Usage
- 10 MB batches (default): ~20-30 MB total
- 1 MB batches: ~5-10 MB
- Memory is independent of file size due to streaming architecture

### Batch Size Calculations
```
Toy (128 bytes/event):  1 MB = 8,192 events;  10 MB = 81,920 events
GlueX (152 bytes/event): 1 MB = 6,898 events; 10 MB = 68,985 events
```

### E2SAR Buffer Packing
```
MTU 1500:  max payload ≈ 1472 bytes → ~11 toy events/frame
MTU 9000:  max payload ≈ 8972 bytes → ~70 toy events/frame
```

## Project Context: EJFAT/e2sar

This utility is part of the EJFAT (Experimental Data Flow Architecture Test) project ecosystem for streaming particle physics event data over high-speed networks.

**Data flow:**
```
ROOT files → RootFileProcessor (branch reading + serialization)
           → std::vector<double> batches
           → E2SAR Segmenter (MTU-sized UDP frames)
           → Load balancer → Data plane endpoints
           → E2SAR Reassembler → event files on disk
```
