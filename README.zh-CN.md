# nvcodec-container-fix

[English](README.md) | 简体中文

让只分到宿主机部分 GPU 的容器里 NVDEC/NVENC 能用的 `LD_PRELOAD` 库。

## 问题

NVIDIA 驱动 570 起（595 仍在）：容器只挂了宿主机部分 `/dev/nvidiaN` 时，驱动的 GPU 列表查询仍返回宿主机全部 GPU。`libnvcuvid`（NVDEC）、`libnvidia-encode`（NVENC）初始化时要打开列表里 UUID 最小的那张"主卡"，它不在容器里就失败。CUDA 计算不受影响。

### 症状

```
cuvidGetDecoderCaps(&caps) failed -> CUDA_ERROR_NO_DEVICE: no CUDA-capable device is detected
OpenEncodeSessionEx failed: unsupported device (2): (no details)
```

见 [nvidia-container-toolkit#1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249)、[k8s-device-plugin#1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282)，以及下文的[上游现状](#上游现状)。

## 做法

拦截 `ioctl`，驱动在 `/dev/nvidiactl` 上返回 GPU 列表（RM 控制 `GET_ATTACHED_IDS` 0x201、`GET_PROBED_IDS` 0x214、`GET_ACTIVE_DEVICE_IDS` 0x288，直接发的或套在 `NV_ESC_IOCTL_XFER_CMD` 里的）后，去掉确定不在容器里的 GPU。列表里只剩容器里的卡后，主卡总是容器能打开的那张。容器里的卡都会保留。

判断哪些卡在容器里时，在同一个文件描述符上用 `NV_ESC_CARD_INFO` 向驱动要每张卡的设备次号。驱动给每张卡的 id 和它放进列表里的 id 相同，所以不需要 /proc。这一点要紧，因为 legacy 模式下 libnvidia-container 会把其他卡在 `/proc/driver/nvidia/gpus` 里的条目藏起来。`/dev/nvidia{minor}` 是对应的字符设备（195:minor）才算在容器里，这个节点不存在或是别的东西才算确定不在。CARD_INFO 里没有的 id，或 CARD_INFO 失败时，退回到按 id 里编码的 PCI 地址去对 `/proc/driver/nvidia/gpus/<PCI>/information` 的 Device Minor。

判断不了的卡保留。宿主机里至少一张卡确定在容器里、至少一张确定不在时才改写，否则原样放行（裸机、整机容器、CPU 机器都不动）；过滤后为空时也原样放行。所以可以全局加载。钩子里只用裸系统调用和栈上缓冲区，保留 `errno`，只依赖 GLIBC_2.17 的符号。细节见 `nvcodec_container_fix.c` 文件头。

## 编译

需要 x86-64 Linux、gcc、make、binutils（`objdump`）和 glibc 2.35 及以上的头文件（用到 `struct dl_find_object`）。编出的库只依赖 GLIBC_2.17，preload 进旧镜像的进程也能加载。

```bash
make        # 编出 libnvcodec_container_fix.so，并检查依赖的 glibc 符号版本不超过 GLIBC_2.17
make test   # 模拟驱动、/proc 和 /dev 的单元测试，不需要 GPU
```

## 用法

```bash
LD_PRELOAD=/path/to/libnvcodec_container_fix.so <命令>
```

### 全局加载

每个登录 shell 都加载：在 `~/.bash_profile` 里加

```bash
export LD_PRELOAD=/path/to/libnvcodec_container_fix.so
```

`ssh 主机 '命令'`、`tmux new-session '命令'` 不读 `~/.bash_profile`，要在命令里显式带上 `LD_PRELOAD`。镜像自己设 `LD_PRELOAD` 会把 `~/.bash_profile` 里的覆盖；要对所有进程生效，改把库的绝对路径写进 `/etc/ld.so.preload`（容器重启后要重写）：

```bash
echo /path/to/libnvcodec_container_fix.so >> /etc/ld.so.preload
```

### 环境变量

- `NVCODEC_CONTAINER_FIX_DEBUG=1`：每次改写打到 stderr
- `NVCODEC_CONTAINER_FIX_DISABLE=1`：原样放行

## 在 GPU 机器上测试

```bash
bash tests/run_tests.sh
FFMPEG=/opt/ffmpeg/bin/ffmpeg PYTHON=/usr/bin/python3 bash tests/run_tests.sh   # 换用别的程序
```

同一组测试先不加载库跑一遍作对照，再加载跑一遍，每项打印退出码和输出的最后几行：CUDA（`tests/test_cuda.py`）、`nvidia-smi`，以及每张卡上的 ffmpeg NVDEC（`hevc_cuvid`、`-hwaccel cuda`）和 NVENC（`h264_nvenc`、`hevc_nvenc`）。最后打印一次解码、一次编码时库的调试日志。对照组只作参考；加载库后有任何一项失败，脚本以非零退出。需要：

- `nvidia-smi`
- `PYTHON`（默认 `python3`）：Python 3；`tests/test_cuda.py` 只用标准库（`ctypes`）和驱动的 `libcuda.so.1`
- `FFMPEG`（默认 `ffmpeg`）：支持 NVDEC/NVENC 的 ffmpeg（`hevc_cuvid`、`-hwaccel cuda`、`h264_nvenc`、`hevc_nvenc`）。HEVC 测试视频由 ffmpeg 的 `testsrc` 生成，这个 ffmpeg 有 `libx265` 时用它编码，否则加载本库用 `hevc_nvenc` 编码。

## 已验证

当前版本用 `tests/run_tests.sh` 测过。环境：宿主机有 8 张 RTX 4090，驱动 595.71.05，容器里的 `/proc/driver/nvidia/gpus` 列出宿主机全部 GPU（和 CDI 模式一样）。

- **bug 复现并修好：** 容器分到 8 张里的 2 张，在容器里让宿主机主卡（UUID 最小的那张）的设备节点不可用，只剩一张不是主卡的卡。不加载时 `hevc_cuvid`、`h264_nvenc`、`hevc_nvenc` 失败，`-hwaccel cuda` 报 `CUDA_ERROR_NO_DEVICE` 并退回软件解码；加载后全部正常。CUDA 和 `nvidia-smi` 加不加载都正常。
- **没有副作用：** 分到 8 张里 1 张和 2 张、且包含主卡的容器（这时 bug 不出现），加载和不加载都全部通过；2 卡容器里加载后两张卡都能解码、编码。
- 每次运行的调试日志都显示，libcuda（0x201、0x214、0x288）和 libnvcuvid（0x201）拿到的列表被过滤成只剩容器里的卡，卡在不在容器里由 CARD_INFO 判断。

只用 /proc 找次号的早先版本，在 8 个以上同样的单卡容器里长时间跑过实际的解码任务。

- **按设计能处理，还没在硬件上跑过：** legacy 模式（`docker --gpus`、默认的 nvidia-container-runtime、GPU Operator 的 envvar 策略）。这时 libnvidia-container（[`mount_procfs`、`mount_procfs_gpu`](https://github.com/NVIDIA/libnvidia-container/blob/90cde8ad3e17aa93e6e065115026c2c9db3ae41a/src/nvc_mount.c#L349)）在 `/proc/driver/nvidia` 上挂一层 tmpfs，只把容器自己的卡绑回去，容器里的卡由 CARD_INFO 判断。
- **没测过：** 分到多张卡、但都不是主卡的容器；595.71.05 以外的驱动。

## 局限

- 32 位进程加载这个 64 位库会打 `wrong ELF class` 警告（不影响运行）。
- 只支持 x86-64：源码把 `dlsym` 固定到 x86-64 的基础符号版本 `GLIBC_2.2.5`。
- 依赖源码文件头列出的驱动结构体布局（570.86.15、580.65.06、595.71.05、620.73.04 相同），换大版本驱动后先跑 `make test` 和 `tests/run_tests.sh`。
- 是否在容器里由 `/dev/nvidia{minor}` 节点判断，设备 cgroup 拒绝访问一个存在的节点时看不出来。
- preload 管不到静态链接的程序；setuid 程序会忽略 `LD_PRELOAD` 环境变量（`/etc/ld.so.preload` 不受影响）。

## 与同类项目的比较

| 项目 | 层次 / 加载方式 | 过滤的命令 | XFER | 怎么判断在不在容器里 | 判断不了的 id | 许可证 |
|---|---|---|---|---|---|---|
| 本项目 | `LD_PRELOAD`（环境变量或 `/etc/ld.so.preload`） | 0x201、0x214（含 gpuFlags）、0x288 | 是 | CARD_INFO 次号，/proc 兜底；195:minor 字符设备 | 保留 | MIT |
| [flexgrip/nvidia-gpu-enumeration](https://github.com/flexgrip/nvidia-gpu-enumeration) | 镜像里设 `LD_PRELOAD` | 0x201 | 否 | `GET_ID_INFO`，再 /proc | 去掉 | GPLv3 |
| [xerktech/fenrir nri-nvenc-fix](https://github.com/xerktech/fenrir/tree/main/cmd/nri-nvenc-fix)（[#45](https://github.com/xerktech/fenrir/pull/45)、[#47](https://github.com/xerktech/fenrir/pull/47)） | NRI 插件，只读 `/etc/ld.so.preload`，i686 桩 | 0x201 | 否 | `GET_ID_INFO`，再 /proc（完整 PCI 地址） | 去掉 | GPLv3（shim） |
| [tinkerborg/nvcodec-shim](https://github.com/tinkerborg/nvcodec-shim) | 准入策略注入 `LD_PRELOAD` | 0x201、0x214 | 否 | id 编码、/proc、stat 节点 | 保留 | 未声明 |
| [Avi130805 nvenc-gpu-filter](https://github.com/Avi130805/webgl2video/tree/main/nvenc-gpu-filter) | 包一层 ffmpeg | 0x201 | 是 | `GET_PCI_INFO`，再 /proc | 列表原样 | MIT |
| [Bl4ut0 tdarr-node-runpod #3](https://github.com/Bl4ut0/tdarr-node-runpod/pull/3) | 入口脚本设 `LD_PRELOAD` | 0x201 | 否 | `GET_PCI_INFO`，再 /proc，每次查询重读 | 去掉 | 未声明 |
| [selkies pixelflux nvgpufilter.rs](https://github.com/selkies-project/pixelflux/blob/main/src/nvgpufilter.rs) | 在自身进程里改 NVIDIA 库 GOT 中的 `ioctl` | 0x201、0x214 | 否 | /proc，再打开节点 | 只在结果是真子集时过滤 | MPL-2.0 |
| [WetFoxx vgnv.c](https://github.com/WetFoxx/Vast.ai-Cloud-Gaming-/blob/main/docker/vgpad/vgnv.c) | 给 Sunshine 用的 `LD_PRELOAD` | 0x201、0x214 | 否 | `GET_PCI_INFO` 对 `nvidia-smi` 给的地址 | 保留 | MIT |
| [anlamm open-gpu-kernel-modules 分支](https://github.com/anlamm/open-gpu-kernel-modules/tree/fix/nvenc-container-visibility) | 内核 RM 补丁（570.86.16 开源模块） | 0x201、0x214 | 不适用 | 在调用方的挂载命名空间里 `kern_path()` 节点 | 隐藏 | MIT/GPLv2 |

本项目的不同之处：

- **覆盖面：** 0x201、0x214（`gpuFlags` 跟着压缩）和 0x288（MIG 条目保留），直接发的和套在 `NV_ESC_IOCTL_XFER_CMD` 里的都处理；进程里所有库发的都处理，CUDA、NVML 看到的卡也一致。
- **由驱动判断：** `NV_ESC_CARD_INFO` 按 RM 列表里用的 id 给出每张卡的次号，既不依赖 /proc 列出被藏起来的卡，也不依赖解码 id；再核对 195:minor 字符设备。
- **保守：** 只去掉确定不在的卡，对不上的 id 保留；宿主机的卡全在或全不在容器里时原样放行。
- **轻量：** 保留 `errno`，只依赖 GLIBC_2.17 符号，钩子里不分配内存、不用 stdio，不改任何文件和系统配置。

别人做得更好的地方：

- fenrir 通过 NRI 自动注入 Kubernetes 的 GPU 容器，用只读的 `/etc/ld.so.preload`（镜像自己的 `LD_PRELOAD` 盖不掉），并为 32 位进程准备了 i686 桩。
- anlamm 的内核分支在驱动里生效，静态链接的程序和没带 preload 启动的进程也管得到。

## 上游现状

没有确认的官方修复。NVIDIA 维护者在 [k8s-device-plugin#1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282)（2026-06-08）里说 "610.x versions seem to have the fix"，但没测过。有用户报告 610.43.03 和 610.57.04 上 NVENC 仍然失败（论坛 [#11](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361/11)、[#13](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361/13)）。[nvidia-container-toolkit#1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249) 以 "a fix for this does not belong in NVIDIA Container Toolkit" 关闭，[#1209](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1209) 作为它的重复关闭。[open-gpu-kernel-modules#826](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/826) 仍开着。

## 许可证

MIT，见 [LICENSE](LICENSE)。结构体布局和常量取自 NVIDIA [open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules/tree/595.71.05) 595.71.05（MIT/GPL 双许可），属于互操作所需的事实，没有复制其代码。

## 参考

- [nvidia-container-toolkit#1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249)、[#1209](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1209)、[k8s-device-plugin#1282](https://github.com/NVIDIA/k8s-device-plugin/issues/1282)、[open-gpu-kernel-modules#826](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/826)、[NVIDIA 论坛帖](https://forums.developer.nvidia.com/t/nvenc-and-nvdec-work-on-only-one-gpu-with-multi-gpu-setups-with-nvidia-container-toolkit-in-driver-565/347361)
- NVIDIA open-gpu-kernel-modules 595.71.05：[`nv_ioctl_card_info_t`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/kernel-open/common/inc/nv-ioctl.h#L56-L68)、[`nvidia_read_card_info`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/kernel-open/nvidia/nv.c#L2359-L2405)、[`gpu_id` 的来源](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/src/nvidia/arch/nvalloc/unix/src/osinit.c#L1622-L1639)、[RM 列表里的 id](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/595.71.05/src/nvidia/src/kernel/gpu_mgr/gpu_mgr.c#L2258)、`nvos.h`（`NVOS54_PARAMETERS`）、`gpu.c`（`gpuGenerate32BitId`）
- [libnvidia-container `nvc_mount.c`](https://github.com/NVIDIA/libnvidia-container/blob/90cde8ad3e17aa93e6e065115026c2c9db3ae41a/src/nvc_mount.c#L349-L458)
