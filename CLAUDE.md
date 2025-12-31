# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

e2sar-utils is a C++ utility library for the EJFAT (Experimental Data Flow Architecture Test) project, focused on processing particle physics data from ROOT files for streaming and distributed analysis.

**Domain Context:** Particle physics data analysis, specifically Dalitz decay analysis with pions and photons (π+π-γγ events).

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
- **Boost** (≥1.89.0): Currently using `program_options` module for CLI parsing
- **ROOT** (particle physics library): Discovered via pkg-config, falls back to root-config
  - Components used: Core, RIO, Tree
  - For future physics analysis: Physics (TLorentzVector, TVector3), MathCore
- **gRPC** (≥1.74.1) and **Protocol Buffers**: For future network streaming

### Dependency Discovery Strategy

**ROOT dependency** uses a two-tier approach in `meson.build`:
1. First tries pkg-config: `dependency('ROOT', method: 'pkg-config', components: ['Core', 'RIO', 'Tree'])`
2. Falls back to root-config if pkg-config fails
3. This pattern ensures compatibility across different ROOT installations

## Architecture

### Current Implementation (Step 1)

**Executable:** `root-reader` (src/root_reader.cpp)
- Command-line tool to read ROOT files and extract named trees
- Verifies tree extraction without processing events yet
- Uses Boost::program_options for robust CLI argument handling

### Multi-Step Development Plan

This project follows a phased approach:

1. **Step 1 (Complete):** ROOT file reading and tree extraction verification
2. **Step 2 (Planned):** Event iteration and branch reading from trees
3. **Step 3 (Planned):** Physics analysis (particle kinematics, invariant mass calculations)
4. **Step 4 (Planned):** Memory buffer management for event data
5. **Step 5 (Planned):** Network transmission via e2sar/gRPC

**Reference Implementation:** `tests/read_dalitz_root.py` demonstrates the target Python workflow that the C++ utilities should replicate.

### Code Organization Pattern

```
src/
  root_reader.cpp        # Executables (user-facing tools)
  meson.build           # Executable build configs

include/                 # Public headers (currently empty, for future library APIs)

tests/
  read_dalitz_root.py   # Python reference implementation
  meson.build           # Test framework setup (GTest template ready)
```

**Library vs Executable Strategy:**
- The project template supports building a shared library (`e2sar_utils_lib`) when source files are added
- Currently focused on executables
- Future physics processing functions should be extracted to library components

## ROOT File Processing

### Tree Structure
ROOT files contain `TTree` objects with event data. The reference implementation processes:
- Tree name: `dalitz_root_tree` (but parameterized via --tree flag)
- Branches: Particle momentum vectors (magnitude, theta, phi) for π+, π-, γ1, γ2
- Event count example: ~2.5 million entries in test data

### Processing Pattern (from Python reference)
```python
with ROOT.TFile(file_path, "READ") as f:
    t = f['tree_name']
    for event in t:
        # Read branches: mag_plus_rec, theta_plus_rec, phi_plus_rec, etc.
        # Create TLorentzVector from (mag, theta, phi, mass)
        # Compute invariant masses and kinematic checks
```

**C++ Equivalent (for future steps):**
- Open: `TFile::Open(path, "READ")`
- Get tree: `file->Get<TTree>(tree_name)`
- Set branch addresses: `tree->SetBranchAddress(branch_name, &variable)`
- Iterate: `for (Long64_t i = 0; i < tree->GetEntries(); i++)`

## Adding New Dependencies

When adding Boost modules:
```python
# In src/meson.build
boost_module_dep = dependency('boost', modules: ['module_name'])
# Add to executable dependencies
```

When adding new ROOT components:
```python
# In meson.build (line 22-37)
# Update the components list in the pkg-config dependency call
```

## Testing ROOT Utilities

Test data location: `dalitz_toy_data_0/dalitz_root_file_0.root` (543 MB, ~2.5M entries)

```bash
# Test tree extraction
./build/src/root-reader --tree dalitz_root_tree ./dalitz_toy_data_0/dalitz_root_file_0.root

# Test help
./build/src/root-reader --help

# Test error handling
./build/src/root-reader --tree nonexistent_tree ./dalitz_toy_data_0/dalitz_root_file_0.root
```

## Project Context: EJFAT/e2sar

This utility library is part of the EJFAT (Experimental Data Flow Architecture Test) project ecosystem. The end goal is streaming particle physics event data over networks using specialized protocols (likely via gRPC, hence the dependency).

**Data Flow (Planned):**
ROOT files → Event extraction → Physics analysis → Memory buffers → Network streaming (e2sar)
