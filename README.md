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

# ROOT stup
source env.sh

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

## Running
Here are the command-line instructions for running the HAIDIS back-end components 
(i.e., the software components downstream of the EJFAT load balancer). 
The first example shows how to run them natively, without using a container.

#### Native

##### Setup the environment
```
source env.sh

```

##### Start ET

```
et_start -f /tmp/et_sys -v -d -n 1000 -s 2097152 -p 23911

```
##### Start ET receiver

```
./ersap-et-receiver -u $EJFAT_URI --withcp -v --recv-ip 129.57.177.8 --recv-port 10000 --recv-threads 8 --et-file /tmp/et_sys

```
##### Start ERSAP

```
$ERSAP_HOME/bin/ersap-shell haidis.ersap

```

### Docker image

#### Building Docker image

```
docker build --target deploy -t haidis-dp:latest -f Dockerfile.cli .
```

#### Running Docker image
Note: The environment variables EJFA_URI (the reserved load balancer instance) and 
RECV_IP (the network interface where incoming data packets are expected) 
must be properly set before running the application.

```
docker run -it --network=host --entrypoint /bin/bash -e EJFAT_URI=$EJFAT_URI -e RECV_IP=$RECV_IP haidis-dp:latest
```

## License

TBD
