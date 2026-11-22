# nvcodec-container-fix

English | [简体中文](README.zh-CN.md)

An `LD_PRELOAD` library that makes NVDEC/NVENC work in containers that are given only some of the host's GPUs.

## Problem

Since NVIDIA driver 570 (still present in 595), when a container has only some of the host's `/dev/nvidiaN` devices,
the driver's GPU list queries still return all of the host's GPUs. On initialization, `libnvcuvid` (NVDEC) and
`libnvidia-encode` (NVENC) open the "primary" GPU, the one with the lowest UUID in that list. If that GPU is not in the
container, they fail. CUDA compute is not affected.

### Symptoms

```
cuvidGetDecoderCaps(&caps) failed -> CUDA_ERROR_NO_DEVICE: no CUDA-capable device is detected
OpenEncodeSessionEx failed: unsupported device (2): (no details)
```

See [nvidia-container-toolkit#1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249) and
[k8s-device-plugin#1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282), and
[Status upstream](#status-upstream) below.

## How it works

The library intercepts `ioctl`. After the driver returns a GPU list on `/dev/nvidiactl` (RM controls
`GET_ATTACHED_IDS` 0x201, `GET_PROBED_IDS` 0x214 and `GET_ACTIVE_DEVICE_IDS` 0x288, sent directly or wrapped in
`NV_ESC_IOCTL_XFER_CMD`), it removes the GPUs that are certainly not in the container. Once the list holds only the
container's GPUs, the primary GPU is always one the container can open. Every GPU that is in the container is kept.

To decide which GPUs are in the container, the library asks the driver for each GPU's device minor with
`NV_ESC_CARD_INFO` on the same file descriptor. The driver reports each GPU under the same id that it puts in the
lists, so no /proc entry is needed. This matters because in legacy mode libnvidia-container hides the other GPUs'
`/proc/driver/nvidia/gpus` entries. A GPU is in the container if `/dev/nvidia{minor}` is the matching character
device (195:minor), and certainly not in it if that node is missing or is something else. For an id that CARD_INFO
does not list, or if CARD_INFO fails, the library falls back to the PCI address encoded in the id and the Device Minor
in `/proc/driver/nvidia/gpus/<PCI>/information`.

GPUs whose membership cannot be determined are kept. A list passes through unchanged unless at least one of the host's
GPUs is known to be in the container and at least one is known not to be (so bare metal, whole-machine containers and
CPU-only machines are untouched), and when filtering would leave it empty. So the library can be preloaded globally.
The hook uses only raw system calls and stack buffers, preserves `errno` and depends only on GLIBC_2.17 symbols. See
the header of `nvcodec_container_fix.c` for details.

## Build

Needs x86-64 Linux, gcc, make, binutils (`objdump`) and glibc 2.35 or newer headers (for `struct dl_find_object`). The
built library needs only GLIBC_2.17, so it also loads into processes of older images.

```bash
make        # builds libnvcodec_container_fix.so and checks that it needs no glibc symbol newer than GLIBC_2.17
make test   # unit tests with a mocked driver, /proc and /dev; no GPU needed
```

## Usage

```bash
LD_PRELOAD=/path/to/libnvcodec_container_fix.so <command>
```

### Global preload

To load it in every login shell, add this to `~/.bash_profile`:

```bash
export LD_PRELOAD=/path/to/libnvcodec_container_fix.so
```

`ssh host 'command'` and `tmux new-session 'command'` do not read `~/.bash_profile`; set `LD_PRELOAD` in the command
explicitly. An image that sets its own `LD_PRELOAD` overrides the one from `~/.bash_profile`. To cover every process,
write the library's absolute path to `/etc/ld.so.preload` instead (write it again after the container restarts):

```bash
echo /path/to/libnvcodec_container_fix.so >> /etc/ld.so.preload
```

### Environment variables

- `NVCODEC_CONTAINER_FIX_DEBUG=1`: print every rewrite to stderr
- `NVCODEC_CONTAINER_FIX_DISABLE=1`: pass everything through unchanged

## Testing on a GPU machine

```bash
bash tests/run_tests.sh
FFMPEG=/opt/ffmpeg/bin/ffmpeg PYTHON=/usr/bin/python3 bash tests/run_tests.sh   # other binaries
```

The script runs the same checks without the library as a baseline, then with it preloaded, and prints the exit code and
last output lines of each: CUDA (`tests/test_cuda.py`), `nvidia-smi`, and for each GPU ffmpeg NVDEC (`hevc_cuvid`,
`-hwaccel cuda`) and NVENC (`h264_nvenc`, `hevc_nvenc`). Finally it prints the library's debug log for one decode and
one encode. The baseline is informational; the script exits non-zero if any check fails with the library preloaded. It
needs:

- `nvidia-smi`
- `PYTHON` (default `python3`): Python 3; `tests/test_cuda.py` uses only the standard library (`ctypes`) and the
  driver's `libcuda.so.1`
- `FFMPEG` (default `ffmpeg`): an ffmpeg build with NVDEC/NVENC support (`hevc_cuvid`, `-hwaccel cuda`, `h264_nvenc`,
  `hevc_nvenc`). The HEVC test clip is generated from ffmpeg's `testsrc` and encoded with `libx265` if that ffmpeg has
  it, otherwise with `hevc_nvenc` with the library preloaded.

## Verified environment

This version, with `tests/run_tests.sh`, on hosts with 8 RTX 4090s, driver 595.71.05, in containers whose
`/proc/driver/nvidia/gpus` lists all of the host's GPUs (as in CDI mode):

- **The bug, fixed:** a container holding 2 of the 8 GPUs, with the device node of the host's primary GPU (the one
  with the lowest UUID) made unavailable inside the container, leaving one GPU that is not the primary. Without the
  library, `hevc_cuvid`, `h264_nvenc` and `hevc_nvenc` failed and `-hwaccel cuda` reported `CUDA_ERROR_NO_DEVICE` and
  fell back to software decoding; with it, all of them worked. CUDA and `nvidia-smi` worked either way.
- **No regression:** containers holding 1 and 2 of the 8 GPUs, including the primary GPU (where the bug does not
  show): every check passed with and without the library; in the 2-GPU container, both GPUs decoded and encoded with
  the library preloaded.
- In every run the debug log showed the lists from libcuda (0x201, 0x214, 0x288) and libnvcuvid (0x201) filtered down
  to the container's GPUs, with membership from CARD_INFO.

An earlier version, which found minors through /proc only, has been used for long real-world decoding runs on 8 or
more single-GPU containers of the same kind.

- **By design, not yet run on hardware:** legacy mode (`docker --gpus`, the default nvidia-container-runtime, the GPU
  Operator's envvar strategy), where libnvidia-container
  ([`mount_procfs`, `mount_procfs_gpu`](https://github.com/NVIDIA/libnvidia-container/blob/90cde8ad3e17aa93e6e065115026c2c9db3ae41a/src/nvc_mount.c#L349))
  mounts a tmpfs over `/proc/driver/nvidia` and binds back only the container's GPUs. Membership then comes from
  CARD_INFO.
- **Untested:** containers holding several GPUs none of which is the primary; drivers other than 595.71.05.

## Limitations

- A 32-bit process that loads this 64-bit library prints a `wrong ELF class` warning (it keeps running).
- x86-64 only: the source pins `dlsym` to `GLIBC_2.2.5`, the x86-64 base symbol version.
- The library depends on the layout of the driver structs listed in the source header (the same in 570.86.15,
  580.65.06, 595.71.05 and 620.73.04). After a major driver upgrade, run `make test` and `tests/run_tests.sh` first.
- Membership is decided by the `/dev/nvidia{minor}` node. A device cgroup that denies an existing node is not seen.
- Preloading does not reach statically linked programs, and the `LD_PRELOAD` variable is ignored for setuid programs
  (`/etc/ld.so.preload` is not).

## Comparison with similar projects

| Project | Layer / delivery | Commands filtered | XFER | Membership decided by | Unknown ids | License |
|---|---|---|---|---|---|---|
| this project | `LD_PRELOAD` (environment or `/etc/ld.so.preload`) | 0x201, 0x214 with gpuFlags, 0x288 | yes | CARD_INFO minor, /proc fallback; 195:minor character device | kept | MIT |
| [flexgrip/nvidia-gpu-enumeration](https://github.com/flexgrip/nvidia-gpu-enumeration) | `LD_PRELOAD` set in the image | 0x201 | no | `GET_ID_INFO`, then /proc | dropped | GPLv3 |
| [xerktech/fenrir nri-nvenc-fix](https://github.com/xerktech/fenrir/tree/main/cmd/nri-nvenc-fix) ([#45](https://github.com/xerktech/fenrir/pull/45), [#47](https://github.com/xerktech/fenrir/pull/47)) | NRI plugin, read-only `/etc/ld.so.preload`, i686 stub | 0x201 | no | `GET_ID_INFO`, then /proc (full PCI address) | dropped | GPLv3 (shim) |
| [tinkerborg/nvcodec-shim](https://github.com/tinkerborg/nvcodec-shim) | `LD_PRELOAD` injected by an admission policy | 0x201, 0x214 | no | id encoding, /proc, stat of the node | kept | none stated |
| [Avi130805 nvenc-gpu-filter](https://github.com/Avi130805/webgl2video/tree/main/nvenc-gpu-filter) | wraps ffmpeg | 0x201 | yes | `GET_PCI_INFO`, then /proc | list left as is | MIT |
| [Bl4ut0 tdarr-node-runpod #3](https://github.com/Bl4ut0/tdarr-node-runpod/pull/3) | `LD_PRELOAD` from the entrypoint | 0x201 | no | `GET_PCI_INFO`, then /proc, per query | dropped | none stated |
| [selkies pixelflux nvgpufilter.rs](https://github.com/selkies-project/pixelflux/blob/main/src/nvgpufilter.rs) | GOT patch of `ioctl` in the NVIDIA libraries of its own process | 0x201, 0x214 | no | /proc, then opening the node | filters only to a strict subset | MPL-2.0 |
| [WetFoxx vgnv.c](https://github.com/WetFoxx/Vast.ai-Cloud-Gaming-/blob/main/docker/vgpad/vgnv.c) | `LD_PRELOAD` for Sunshine | 0x201, 0x214 | no | `GET_PCI_INFO` against addresses from `nvidia-smi` | kept | MIT |
| [anlamm open-gpu-kernel-modules fork](https://github.com/anlamm/open-gpu-kernel-modules/tree/fix/nvenc-container-visibility) | kernel RM patch (570.86.16 open modules) | 0x201, 0x214 | n/a | `kern_path()` of the node in the caller's mount namespace | hidden | MIT/GPLv2 |

Where this project differs:

- **Coverage:** 0x201, 0x214 (with `gpuFlags` compacted) and 0x288 (MIG entries kept), sent directly or wrapped in
  `NV_ESC_IOCTL_XFER_CMD`. Queries from every library in the process are handled, so CUDA and NVML see the same GPUs.
- **Membership from the driver:** `NV_ESC_CARD_INFO` gives each GPU's minor under the id RM uses in the lists, so
  membership neither depends on /proc listing the hidden GPUs nor on decoding the id. It is checked against a
  195:minor character device.
- **Conservative:** only GPUs that are certainly absent are removed; ids that cannot be placed are kept, and lists pass
  through when all or none of the host's GPUs are in the container.
- **Low footprint:** `errno` is preserved, only GLIBC_2.17 symbols are needed, the hook does no allocation and no
  stdio, and no files or system settings are changed.

Where others are ahead:

- fenrir injects automatically into Kubernetes GPU containers through NRI. It uses a read-only `/etc/ld.so.preload`,
  which an image's own `LD_PRELOAD` cannot override, and an i686 stub for 32-bit processes.
- The anlamm kernel fork works inside the driver, so it also covers statically linked programs and processes started
  without the preload.

## Status upstream

No official fix is confirmed. In [k8s-device-plugin#1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282)
(2026-06-08), an NVIDIA maintainer wrote that "610.x versions seem to have the fix" but had not tested it. Users report
NVENC still failing on 610.43.03 and 610.57.04
([forum posts #11](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361/11)
and [#13](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361/13)).
[nvidia-container-toolkit#1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249) was closed because
"a fix for this does not belong in NVIDIA Container Toolkit", and
[#1209](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1209) was closed as its duplicate.
[open-gpu-kernel-modules#826](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/826) is open.

## License

MIT, see [LICENSE](LICENSE). Struct layouts and constants come from NVIDIA's
[open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules/tree/595.71.05) 595.71.05 (dual MIT/GPL).
They are interoperability facts; no code is copied.

## References

- [nvidia-container-toolkit#1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249),
  [#1209](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1209),
  [k8s-device-plugin#1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282),
  [open-gpu-kernel-modules#826](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/826),
  [NVIDIA forum thread](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361)
- NVIDIA open-gpu-kernel-modules 595.71.05:
  [`nv_ioctl_card_info_t`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/kernel-open/common/inc/nv-ioctl.h#L56-L68),
  [`nvidia_read_card_info`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/kernel-open/nvidia/nv.c#L2359-L2405),
  [`gpu_id` assignment](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/src/nvidia/arch/nvalloc/unix/src/osinit.c#L1622-L1639),
  [ids in the RM lists](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/src/nvidia/src/kernel/gpu_mgr/gpu_mgr.c#L2258),
  `nvos.h` (`NVOS54_PARAMETERS`), `gpu.c` (`gpuGenerate32BitId`)
- [libnvidia-container `nvc_mount.c`](https://github.com/NVIDIA/libnvidia-container/blob/90cde8ad3e17aa93e6e065115026c2c9db3ae41a/src/nvc_mount.c#L349-L458)
