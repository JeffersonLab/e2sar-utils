# ERSAP Data Processing Pipeline README

Welcome! This project provides a streaming data processing pipeline built on **ERSAP**, integrating **EJFAT reassembly**, **shared memory transport**, and a **reactive actor-based processing model**. 
It is designed for high-throughput environments such as HPC systems, with deployment handled through a containerized workflow.

This README will guide you through:
- How the pipeline is structured  
- How to configure key actors  
- How to run the system using the `haidis-dp` container  
- Useful HPC commands for working in this environment  

---

## Overview
# ERSAP Data Processing Pipeline

Welcome! This project provides a streaming data processing pipeline built on **ERSAP**, integrating **EJFAT reassembly**, **shared memory transport**, and a **reactive actor-based processing model**.

It is designed for high-throughput environments such as HPC systems, with deployment handled through a containerized workflow.

---

## Overview

The pipeline consists of several coordinated components:

- **EJFAT Reassembly**  
  Reconstructs incoming data streams into complete events.

- **ERSAP Shared Memory Handling**  
  Enables efficient inter-process communication using shared memory buffers.

- **Reactive Actor-Based Processing Pipeline**  
  Data flows through modular actors that process, transform, and forward events.

All components are orchestrated inside the **`haidis-dp` container**, ensuring consistent deployment across systems.

---

## Configuration

ERSAP actors are configured via a YAML file:


/global/cfs/cdirs/amsc016/ersap-data/config/services.yaml

Note that $ERSAP_USER_DATA points to /global/cfs/cdirs/amsc016/ersap-data
There re two env variables set in the env.sh that is sourced by the entrypoint.sh, these are:
$ERSAP_HOME and $ERSAP_USER_DATA. This are hard coded and is part of the docker image.


Below are examples of how to configure the two main actors used in this pipeline.

---

## HaidisGluexActor

This actor connects to an ET system and consumes events.

### Example

```yaml
- name: HaidisGluexActor
  class: org.jlab.ersap.actor.HaidisGluexActor
  properties:
    et_filename: "/tmp/et_sys"
    et_host: "localhost"
    et_port: 11111
    station_name: "ERSAP_STATION"
    verbose: true
**Parameters**
	et_filename — Path to the ET system file
	et_host — Host where the ET system is running
	et_port — Port number for ET connection
	station_name — Name of the ET station
	verbose — Enable detailed logging

#### HaidisGluexLinkActor

This actor writes processed data into shared memory.

Example
- name: HaidisGluexLinkActor
  class: org.jlab.ersap.actor.HaidisGluexLinkActor
  properties:
    verbose: true
    shm_write: true
    data_id: 1
    shm_name: "ersap_shm"
    sem_name: "ersap_sem"
    ack_sem_name: "ersap_ack_sem"
    shm_size: 1048576
**Parameters**
	verbose — Enable detailed logging
	shm_write — Enable shared memory writing
	data_id — Data stream identifier
	shm_name — Shared memory name
	sem_name — Synchronization semaphore
	ack_sem_name — Acknowledgment semaphore
	shm_size — Shared memory size (bytes)

#### Running with haidis-dp Container

The pipeline is deployed using the haidis-dp container via podman-hpc.

### Docker image

#### Building Docker image

```
docker build --target deploy -t haidis-dp:latest -f Dockerfile.cli .
```

#### Running Docker image locally at JLAB
Note: The environment variables EJFA_URI (the reserved load balancer instance)
must be properly set before running the application.

```
docker run -it --network=host --entrypoint
 /bin/bash -v $ERSAP_USER_DATA:/user_data -e EJFAT_URI=$EJFAT_URI -e RECV_IP=$RECV_IP haidis-dp:latest

```

#### Pushing the docker image to docker hub
```
docker login
docker tag haidis-dp:latest gurjyan/haidis-dp:latest
docker push gurjyan/haidis-dp:latest
```
#### Pulling and running docker image on Perlmutter

```
podman-hpc pull docker.io/gurjyan/haidis-dp:latest

podman-hpc run -it --network=host --group-add keep-groups --entrypoint /bin/bash -v /global/cfs/cdirs/amsc016/haidis/ersap-data:/user_data -e EJFAT_URI=$EJFAT_URI  haidis-dp:latest

```
#### Find the project numbers
sacctmgr -p show assoc where user=$USER format=Account,Cluster,QOS

#### Reserve a node
salloc -N 1 -C cpu -q interactive -t 01:00:00 -A m3792

#### common project specific scratch
/global/cfs/cdirs/m3792/haidis/sbatch
or
/global/cfs/cdirs/amsc016/haidis/sbatch


### HPC Environment Tips

**Check Project Allocations**
	sacctmgr show assoc user=$USER format=Account,User

**Allocate Compute Nodes**
	salloc -A <project> -N 1 -t 01:00:00

**Alternative (system dependent):**

	alloc -A <project> -N 1 -t 01:00:00

**Common Directory Paths**

Global storage

	/global/cfs/cdirs/amsc016/

Scratch space

	/pscratch/sd/<first_letter>/<username>/

#### Workflow Summary

	Configure actors in services.yaml
	Allocate HPC resources (if needed)
	Run the container using podman-hpc
	Monitor logs (enable verbose mode if needed)
	Tune parameters (threads, shared memory size, etc.)

#### Notes
	Start with verbose: true during testing
	Ensure shared memory size matches expected data rate
	Use interactive mode for debugging
	Keep configuration files under version control

This pipeline is designed to be flexible and modular, so feel free to extend it with additional actors or optimizations as needed.

