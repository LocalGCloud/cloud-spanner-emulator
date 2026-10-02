<!-- generated-by: gsd-doc-writer -->
# Getting started

Run this fork of the Cloud Spanner emulator locally with the published Docker image. The image exposes gRPC on port 9010 and REST on port 9020.

## Prerequisites

- Docker with a running engine. No local Bazel installation is needed to run the published image.
- A Cloud Spanner client if you want to make database requests. Set its `SPANNER_EMULATOR_HOST` environment variable to the gRPC address.

To build from source instead, use Bazel 7.6.1 (pinned in [`.bazelversion`](../.bazelversion)) and the platform setup in [Building](building.md).

## Installation steps

1. Clone the repository and enter it:

   ```bash
   git clone https://github.com/LocalGCloud/cloud-spanner-emulator.git
   cd cloud-spanner-emulator
   ```

2. Pull the published image:

   ```bash
   docker pull agentcloud/localcloud-spanner-emulator
   ```

The image can also be run without cloning the repository. Its `latest` tag can lag this checkout; use a published commit tag when you need a specific revision.

## First run

Start the emulator in the foreground. Press Ctrl-C to stop it; without `--data_dir`, its data is in memory and is lost when it stops.

```bash
docker run --rm -p 127.0.0.1:9010:9010 -p 127.0.0.1:9020:9020 \
  agentcloud/localcloud-spanner-emulator
```

In the shell running your client, point it at the gRPC port:

```bash
export SPANNER_EMULATOR_HOST=localhost:9010
```

The REST endpoint is `http://localhost:9020`. See [Configuration](configuration.md) for flags and a persistent-storage Docker command.

## Common setup issues

- **A host port is already in use:** Change the host side of the Docker mapping, for example `-p 127.0.0.1:19010:9010 -p 127.0.0.1:19020:9020`, then set `SPANNER_EMULATOR_HOST=localhost:19010` in the client shell.
- **Docker reports `exec: "--data_dir=/data": no such file or directory`:** The image has no `ENTRYPOINT`. When adding flags, supply the full command after the image name: `./gateway_main --hostname 0.0.0.0 --data_dir=/data`. Mount a directory at `/data` as shown in [Configuration](configuration.md#docker).
- **A client cannot connect:** Check that the container is still running, port 9010 is published, and `SPANNER_EMULATOR_HOST` is set in the client process. The client uses the gRPC port, not the REST port.

## Next steps

- [Configuration](configuration.md) covers ports, flags, and persistence setup.
- [Testing](TESTING.md) covers the repository's test suites.
- [Building](building.md) covers native builds and local Docker image builds.
