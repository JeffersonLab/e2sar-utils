# Running HAIDIS

## Preparations

First obtain the EJFAT LB if needed using lbadm tool (admin token required, Ilya or Vardan can do that). If you have an instance EJFAT_URI, you can test if it is valid 
as follows:
```bash
baldin@login01:~/haidis> podman-hpc run --rm --network=host docker.io/ibaldin/e2sar:0.3.1 lbadm -u 'ejfats://secrettoken@ejfat-lb.es.net:18008/lb/334?sync=192.188.29.6:19010&data=192.188.29.10&data=[2001:400:a300::10]' --status
E2SAR Version: 0.3.1
Getting LB Status 
   Contacting: ejfats://ejfat-lb.es.net:18008/lb/334?sync=192.188.29.6:19010&data=192.188.29.10&data=[2001:400:a300::10] using address: ejfat-lb.es.net:18008
   LB ID: 334
LB details: expiresat=2027-02-24T18:13:25.240Z, currentepoch=780122, predictedeventnum=1771962496393817
Registered senders: 129.57.177.8 
Registered workers: 
```

It is also recommended that you pull the following images (or the version of the images you are planning to use), in order to cache them locally:
```bash
$ podman-hpc pull docker.io/ibaldin/e2sar:0.3.1
$ podman-hpc pull docker.io/gurjyan/haidis-dp:latest
```

## Running the code

```bash
EJFAT_URI='ejfats://secrettoken@ejfat-lb.es.net:18008/lb/334?sync=192.188.29.6:19010&data=192.188.29.10&data=[2001:400:a300::10]' sbatch haidis/sbatch/haidis_slurm.sh 
```

The output will be in haidis/runs/slurm-<job id>.out and haidis/runs/slurm-<job id>.err, and the containers will both write to a log file under haidis/runs/slurm_job_<job id>/haidis_containers.log 
(same directory will also contain the container startup script for inspection).

## Sending the data

From e.g. ejfat-1 at JLab (when using ejfat[123] use `docker`, when using ejfat[456] use `podman`, note that with podman you have to prepend the image name with
`docker.io/`):

```bash
$ docker run --rm --network=host -v /nvme/haidis/toy_data:/nvme/haidis/toy_data ibaldin/e2sar-utils:0.1.2 e2sar-root -s -u 'ejfats://secrettoken@ejfat-lb.es.net:18008/lb/334?sync=192.188.29.6:19010&data=192.188.29.10&data=[2001:400:a300::10]' --withcp --files /nvme/haidis/toy_data/dalitz_toy_data_0/dalitz_root_file_0.root --tree dalitz_root_tree --bufsize-mb 1 --mtu 9000
```

IMPORTANT: The EJFAT_URI above when starting the sender and when you start the slurm job must be the same! 

If you get SSL issues with EJFAT LB (certificate expired, for example), add `-v` option to e2sar-root. 

## Testing containers interactively

You can get an interactive GPU node as follows:
```bash
salloc -N 1 --account=m3792 --qos=debug -t 00:30:00 --constraint=gpu --gpus=4  
```
The command will log you into the node where you can use podman to test the startup.