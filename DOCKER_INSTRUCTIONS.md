# Docker Build and Run Instructions for E2SAR EJFAT Tools

## Overview

This Docker image packages the complete E2SAR EJFAT tools suite along with all local dependencies (CODA, ERSAP) into a single containerized environment. The container automatically initializes ET and ERSAP systems on startup.

## Key Modifications

### Dockerfile.cli Changes

1. **Complete Directory Packaging**: The entire build context (including `coda/`, `ersap/`, `env.sh`, source files, etc.) is copied to `/work` inside the container.

2. **Environment Configuration**:
   - `MACHINE=Linux-x86_64` - Set for CODA binaries
   - `WORKDIR=/work` - All tools and dependencies reside here
   - `PATH` updated to include E2SAR, CODA, and ERSAP binaries
   - ET port 23911 is exposed

3. **Entrypoint Script**: Custom `/usr/local/bin/entrypoint.sh` handles the startup sequence.

### Entrypoint Behavior

The entrypoint script (`entrypoint.sh`) executes these steps in order:

1. **Validate `EJFAT_URI`**: Checks that the environment variable is set (passed at runtime). Fails with clear error if missing.

2. **Source ROOT Environment**: Sources `/rootlib/root/bin/thisroot.sh` to set up ROOT paths and libraries.

3. **Source Custom Environment**: Sets CODA and ERSAP environment variables (CODA, CODA_BIN, CODA_LIB, ERSAP_HOME, ERSAP_USER_DATA) and updates PATH.

4. **Start ET System**: Launches ET in background with:
   ```
   et_start -f /tmp/et_sys -v -d -n 1000 -s 2097152 -p 23911
   ```

5. **Start ERSAP**: Runs ERSAP in foreground (as PID 1) with:
   ```
   exec ersap-shell haidis.ersap
   ```

The script uses `set -euo pipefail` for safety and prints diagnostic information during startup.

## Build Commands

### Build Production Image

```bash
docker build --target deploy -t e2sar-utils:latest -f Dockerfile.cli .
```

Or with explicit tag:

```bash
docker build --target deploy -t e2sar-utils:slim -f Dockerfile.cli .
```

### Build Debug Image (with network tools)

```bash
docker build --target deploy-debug -t e2sar-utils:debug -f Dockerfile.cli .
```

The debug image includes: netcat, tcpdump, iproute2, ping, nano, and scapy.

### Build Compile Stage (for development)

```bash
docker build --target compile -t e2sar-utils:build -f Dockerfile.cli .
```

## Run Commands

### Normal Runtime (ENTRYPOINT sequence)

Run with required `EJFAT_URI` environment variable:

```bash
docker run -e EJFAT_URI="ejfat://example.com:12345/lb/1" \
           -p 23911:23911 \
           e2sar-utils:latest
```

**Port Mapping**:
- `-p 23911:23911` - Maps ET port to host (if you need external access)
- Can be omitted if running within Docker network

**Additional Environment Variables** (optional):

```bash
docker run -e EJFAT_URI="ejfat://example.com:12345/lb/1" \
           -e OTHER_VAR="value" \
           -p 23911:23911 \
           e2sar-utils:latest
```

### Interactive Shell (Override ENTRYPOINT)

To explore the container or debug issues:

```bash
docker run -it --entrypoint /bin/bash \
           -e EJFAT_URI="ejfat://example.com:12345/lb/1" \
           e2sar-utils:latest
```

Inside the container, you can manually run components:

```bash
# Source ROOT environment
source /rootlib/root/bin/thisroot.sh

# Set up environment variables
export MACHINE=Linux-x86_64
export CODA=/work/coda
export ERSAP_HOME=/work/ersap
export PATH=/work/coda/${MACHINE}/bin:/work/ersap/bin:$PATH

# Start ET manually
et_start -f /tmp/et_sys -v -d -n 1000 -s 2097152 -p 23911 &

# Start ERSAP manually
ersap-shell haidis.ersap
```

### Run with Volume Mount (for development)

If you want to mount local files (e.g., for testing changes without rebuilding):

```bash
docker run -e EJFAT_URI="ejfat://example.com:12345/lb/1" \
           -v $(pwd)/ersap-data:/work/ersap-data \
           -p 23911:23911 \
           e2sar-utils:latest
```

### Run in Background (Detached)

```bash
docker run -d \
           -e EJFAT_URI="ejfat://example.com:12345/lb/1" \
           -p 23911:23911 \
           --name e2sar-container \
           e2sar-utils:latest
```

View logs:

```bash
docker logs -f e2sar-container
```

Stop container:

```bash
docker stop e2sar-container
docker rm e2sar-container
```

## Troubleshooting

### EJFAT_URI Not Set Error

If you see:
```
ERROR: EJFAT_URI environment variable is not set!
```

Make sure to pass it at runtime:
```bash
docker run -e EJFAT_URI="your-value-here" ...
```

### ET Start Fails

Check logs for:
```
ERROR: ET failed to start or exited immediately
```

Common causes:
- Port 23911 already in use
- Insufficient shared memory (increase with `--shm-size=2g`)

### Missing Binaries

If `et_start` or `ersap-shell` are not found, verify:
```bash
docker run -it --entrypoint /bin/bash e2sar-utils:latest
echo $PATH
ls -la /work/coda/Linux-x86_64/bin/
ls -la /work/ersap/bin/
```

## File Locations Inside Container

| Component | Location |
|-----------|----------|
| Working directory | `/work` |
| CODA binaries | `/work/coda/Linux-x86_64/bin/` |
| ERSAP binaries | `/work/ersap/bin/` |
| ERSAP data | `/work/ersap-data/` |
| ROOT installation | `/rootlib/root/` |
| E2SAR installation | `/e2sar-install/` |
| Environment script | `/work/env.sh` |
| Entrypoint script | `/usr/local/bin/entrypoint.sh` |

## Advanced Usage

### Using Docker Compose

Create `docker-compose.yml`:

```yaml
version: '3.8'

services:
  e2sar-tools:
    image: e2sar-utils:latest
    environment:
      - EJFAT_URI=ejfat://example.com:12345/lb/1
    ports:
      - "23911:23911"
    volumes:
      - ./ersap-data:/work/ersap-data
    restart: unless-stopped
```

Run with:
```bash
docker-compose up
```

### Inspecting the Image

```bash
# View image layers
docker history e2sar-utils:latest

# Inspect container filesystem
docker run --rm -it --entrypoint /bin/bash e2sar-utils:latest
```

## Notes

- The container preserves the exact directory structure from the build context.
- All local dependencies in `coda/` and `ersap/` are included as-is.
- The multi-stage build optimizations from the original Dockerfile.cli are preserved.
- ET runs in the background; ERSAP runs as PID 1 for proper signal handling.
- The entrypoint script provides clear diagnostic output during initialization.
