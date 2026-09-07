# Building GhostESP in Docker (CLI only)

GhostESP is a plain ESP-IDF project, **not** a PlatformIO one — there is no
`platformio.ini`, board support lives in `configs/sdkconfig.*` +
`main/Kconfig.projbuild`, and the build needs ESP-IDF **v6.1**. PlatformIO's
`espressif32` platform pins its own (older) ESP-IDF and cannot supply that, so
build with `idf.py` in the Espressif image instead.

## Quick start

```sh
tools/docker/build.sh m5core2_aws esp32      # M5Stack Core2 for AWS
tools/docker/build.sh cardputer   esp32s3    # any configs/sdkconfig.<name>
```

The first argument is the suffix of a file in `configs/`; the second is the
chip. The pairing must match the matrix in
[`.github/workflows/compile_all.yml`](../../.github/workflows/compile_all.yml)
— building a config against the wrong chip fails in confusing ways.

The script builds `ghostesp-idf:v6.1` on first use (a few minutes), then runs
`idf.py` in it against a bind mount of the repo. Artifacts land in `build/` on
the host, owned by you. Extra `idf.py` arguments pass straight through:

```sh
tools/docker/build.sh m5core2_aws esp32 size-components
tools/docker/build.sh m5core2_aws esp32 menuconfig     # needs -it; see below
```

## Why a derived image

Espressif's stock `espressif/idf:v6.1` cannot compile this project: `esp_gdbstub`
fails with `command_name_matches`, `portNUM_PROCESSORS` and `StaticTask_t`
errors. CI patches ESP-IDF in place before every build; the `Dockerfile` here
bakes the same patch (`patch_idf.py`) in once, so builds can run as your own
uid without write access to `$IDF_PATH`. Keep `patch_idf.py` in sync with the
workflow's "Patch ESP-IDF gdbstub compatibility" step.

## Flashing

The container has no access to your USB device, so flash from the host with
esptool (the exact command, with the right offsets, is printed at the end of
every build):

```sh
python -m esptool --chip esp32 -b 460800 write-flash \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/ota_data_initial.bin \
  0x20000 build/Ghost_ESP_IDF.bin
```

Note the app offset is **not** always `0x10000` — the OTA tables put `otadata`
there and the first app slot at `0x20000`. Use the offsets the build printed.

Alternatively pass the device through and flash from inside the container:

```sh
GHOSTESP_DOCKER_ARGS="--device=/dev/ttyUSB0" \
  tools/docker/build.sh m5core2_aws esp32 -p /dev/ttyUSB0 flash monitor
```

## Behind a proxy

The first build fetches the managed components listed in
`main/idf_component.yml` from the Espressif registry. If that needs a proxy:

```sh
GHOSTESP_DOCKER_ARGS="--network host -e HTTPS_PROXY=$HTTPS_PROXY" \
  tools/docker/build.sh m5core2_aws esp32
```

## Interactive use

`build.sh` runs one command and exits. For a shell (menuconfig, poking at the
build), start the container yourself:

```sh
docker run --rm -it -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -e IDF_TARGET=esp32 -v "$PWD:/project" -w /project ghostesp-idf:v6.1 bash
```

Inside, apply a board config the way CI does before building — copying only one
of the two files silently builds the wrong configuration:

```sh
cp configs/sdkconfig.m5core2_aws sdkconfig
cp configs/sdkconfig.m5core2_aws sdkconfig.defaults
idf.py menuconfig
idf.py build
```

Run `idf.py fullclean` whenever you switch board or chip.
