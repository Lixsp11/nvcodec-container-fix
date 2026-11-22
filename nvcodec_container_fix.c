// nvcodec_container_fix: make NVDEC/NVENC work in containers that see only some of the host's GPUs
//
// Copyright (c) 2026 Lixsp11
// SPDX-License-Identifier: MIT
//
// Problem (NVIDIA driver 570 and later, still present in 595; nvidia-container-toolkit#1249, k8s-device-plugin#1282):
// the driver's GPU list queries return all of the host's GPUs. On initialization, libnvcuvid (NVDEC) and
// libnvidia-encode (NVENC) open the "primary" GPU, the one with the lowest UUID in that list; if it is not in the
// container, they fail with CUDA_ERROR_NO_DEVICE / "unsupported device". CUDA compute is not affected.
//
// Approach: LD_PRELOAD this library. It intercepts ioctl and, after the driver returns a GPU list on /dev/nvidiactl,
// rewrites the result to remove only the GPUs that are certainly not in the container. Once the list holds only the
// container's GPUs, the primary GPU is always one the container can open. Every GPU that is in the container is kept.
// No files are modified and no system configuration is touched:
//   - Handles the RM control commands GET_ATTACHED_IDS (0x201), GET_PROBED_IDS (0x214) and GET_ACTIVE_DEVICE_IDS
//     (0x288), both sent directly (NV_ESC_RM_CONTROL) and wrapped in NV_ESC_IOCTL_XFER_CMD, on file descriptors of
//     /dev/nvidiactl; the descriptor is checked before the request is decoded. Queries from every library are handled,
//     so the GPUs that CUDA and NVML see also match the container
//   - Each GPU's device minor comes from the driver: NV_ESC_CARD_INFO, sent on the same descriptor as a raw system
//     call, lists every non-excluded GPU with its gpu_id (the id RM puts in these lists) and minor number. This works
//     when the container runtime hides other GPUs' /proc/driver/nvidia/gpus entries (legacy libnvidia-container mode)
//   - For an id that CARD_INFO does not list, or if CARD_INFO fails, the id is converted to a PCI address following
//     RM's encoding (gpuGenerate32BitId) and matched to the Device Minor in /proc/driver/nvidia/gpus/<PCI>/information.
//     If there is no exact match but exactly one GPU matches on bus+device, that GPU is used (some driver branches
//     leave the PCI domain out of the gpuId); an id with domain 0 whose bus+device match several GPUs is ambiguous
//   - A GPU is in the container if /dev/nvidia{minor} is a character device with major number 195 and the matching
//     minor number, and certainly not in it if that node does not exist or is something else. Ids whose membership
//     cannot be determined (no minor number, unreadable driver information, other stat errors, ambiguous ids) are kept
//   - A list is left unchanged if filtering would empty it, and unless at least one of the host's GPUs is known to be
//     in the container and at least one is known not to be; bare metal, whole-machine containers and CPU-only machines
//     pass through, so the library can be preloaded globally
//   - Membership is recomputed on every query, with raw system calls and stack buffers only: no allocation, no stdio,
//     no locks, no cancellation points. errno is left unchanged when the ioctl succeeds
//   - Depends only on GLIBC_2.17 symbols (newer glibc binds dlsym@GLIBC_2.34 and stat@GLIBC_2.33 by default), so it
//     also loads into processes of older images
//
// Usage:
//   make                             build libnvcodec_container_fix.so
//   make test                        unit tests that need no GPU (mocked driver, /proc and /dev)
//   LD_PRELOAD=$PWD/libnvcodec_container_fix.so <command>
//   NVCODEC_CONTAINER_FIX_DEBUG=1    print every rewrite to stderr
//   NVCODEC_CONTAINER_FIX_DISABLE=1  pass everything through unchanged, rewrite nothing
//
// Struct layouts and constants come from NVIDIA's open-gpu-kernel-modules 595.71.05 (dual MIT/GPL). They are
// interoperability facts; no code is copied:
//   NVOS54_PARAMETERS (nvos.h, 32 bytes): hClient@0 hObject@4 cmd@8 flags@12 params@16 paramsSize@24 status@28; the
//   kernel requires paramsSize to equal the size of the params struct exactly
//   nv_ioctl_xfer_t (nv-ioctl.h, 16 bytes): cmd@0 size@4 ptr@8
//   nv_ioctl_card_info_t (nv-ioctl.h, 72 bytes): valid@0 pci_info@4 gpu_id@16 minor_number@56. NV_ESC_CARD_INFO
//   (NV_IOCTL_BASE(200) + 0, nvidiactl only) takes an array whose size is a nonzero multiple of 72 bytes, fails with
//   EINVAL if it has fewer slots than the driver has GPUs, and zeroes the slots it does not fill (nvidia_read_card_info
//   in nv.c). gpu_id is the id registered for the GPU (osinit.c) that the RM lists report (gpu_mgr.c). The same in
//   570.86.15, 580.65.06 and 620.73.04
//   gpuId = (domain & 0xffff) << 16 | bus << 8 | device (gpuGenerate32BitId in gpu.c)
//   0x201 is gpuIds[32] (128 bytes); 0x214 is gpuIds[32] + excludedGpuIds[32] + gpuFlags[32] (384 bytes, gpuFlags
//   parallel to gpuIds); gpuIds are contiguous and terminated by 0xffffffff; 0x288 is numDevices +
//   devices[256]{gpuId, gpuInstanceId, computeInstanceId}
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");

#define NV_IOCTL_MAGIC 'F'
#define NV_ESC_RM_CONTROL 0x2A
#define NV_ESC_CARD_INFO 0xC8       // NV_IOCTL_BASE(200) + 0
#define NV_ESC_IOCTL_XFER_CMD 0xD3  // NV_IOCTL_BASE(200) + 11
#define NV_MAJOR 195
#define NV_CTL_MINOR 255
#define CMD_GET_ATTACHED_IDS 0x201
#define CMD_GET_PROBED_IDS 0x214
#define CMD_GET_ACTIVE_DEVICE_IDS 0x288
#define MAX_IDS 32  // NV_MAX_DEVICES, the most GPUs the driver handles: also enough CARD_INFO slots
#define MAX_ACTIVE_DEVICES 256
#define MAX_HOST_GPUS 64
#define INVALID_ID 0xffffffffu

// Membership of a GPU in the container
enum { ABSENT = 0, PRESENT = 1, UNKNOWN = -1 };

typedef struct {
    uint32_t hClient, hObject, cmd, flags;
    uint64_t params;
    uint32_t paramsSize, status;
} rm_control_t;
typedef struct {
    uint32_t cmd, size;
    uint64_t ptr;
} xfer_t;
typedef struct {
    uint8_t valid;
    struct {
        uint32_t domain;
        uint8_t bus, slot, function;
        uint16_t vendor_id, device_id;
    } pci_info;
    uint32_t gpu_id;
    uint16_t interrupt_line;
    uint64_t reg_address, reg_size, fb_address, fb_size;
    uint32_t minor_number;
    uint8_t dev_name[10];
} card_info_t;
_Static_assert(sizeof(rm_control_t) == 32, "NVOS54_PARAMETERS layout");
_Static_assert(sizeof(xfer_t) == 16, "nv_ioctl_xfer_t layout");
_Static_assert(sizeof(card_info_t) == 72 && offsetof(card_info_t, pci_info) == 4 &&
                   offsetof(card_info_t, gpu_id) == 16 && offsetof(card_info_t, minor_number) == 56,
               "nv_ioctl_card_info_t layout");

// A host GPU as CARD_INFO (gpu_id) or /proc (PCI address) reports it, and its membership
typedef struct {
    uint32_t gpu_id, domain, bus, device;
    int membership;
} host_gpu_t;
typedef struct {
    int num_cards, num_procs;  // -1: CARD_INFO failed / /proc not read yet
    host_gpu_t cards[MAX_IDS], procs[MAX_HOST_GPUS];
} host_t;

// getdents64 record
typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
} dirent64_t;

// Whether st is the NVIDIA character device 195:minor_id (a GPU node) or 195:255 (nvidiactl)
static int is_gpu_node(const struct stat *st, uint32_t minor_id) {
    return S_ISCHR(st->st_mode) && major(st->st_rdev) == NV_MAJOR && minor(st->st_rdev) == minor_id;
}
static int is_nvidiactl(const struct stat *st) { return is_gpu_node(st, NV_CTL_MINOR); }

// The unit tests (tests/test_unit.c) replace these with a mocked driver, /proc, /dev and device checks
#ifndef PROC_GPUS_DIR
#define PROC_GPUS_DIR "/proc/driver/nvidia/gpus"
#endif
#ifndef DEV_DIR
#define DEV_DIR "/dev"
#endif
#ifndef NODE_IS_GPU
#define NODE_IS_GPU(st, minor_id) is_gpu_node(&(st), minor_id)
#endif
#ifndef FD_IS_NVIDIACTL
#define FD_IS_NVIDIACTL(st) is_nvidiactl(&(st))
#endif
#ifndef DRIVER_IOCTL
#define DRIVER_IOCTL(fd, request, arg) syscall(SYS_ioctl, fd, request, arg)  // straight to the driver, past every hook
#endif

typedef int (*ioctl_fn)(int, unsigned long, ...);
typedef int (*find_object_fn)(void *, struct dl_find_object *);

static ioctl_fn real_ioctl;         // resolved in the constructor so that the hook never calls dlsym
static find_object_fn find_object;  // _dl_find_object (glibc 2.35+), only used to name the caller in debug output
static int debug, disabled;

__attribute__((constructor)) static void setup(void) {
    real_ioctl = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    find_object = (find_object_fn)dlsym(RTLD_DEFAULT, "_dl_find_object");
    const char *d = secure_getenv("NVCODEC_CONTAINER_FIX_DEBUG"), *x = secure_getenv("NVCODEC_CONTAINER_FIX_DISABLE");
    debug = d && !strcmp(d, "1");
    disabled = x && !strcmp(x, "1");
}

// Raw system calls: no stat/fstat symbols (glibc exports them only since 2.33), no locks, no cancellation points
static int stat_path(const char *path, struct stat *st) { return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st, 0); }
static int stat_fd(int fd, struct stat *st) { return (int)syscall(SYS_fstat, fd, st); }
static int open_read(int dir_fd, const char *path, int flags) {
    return (int)syscall(SYS_openat, dir_fd, path, O_RDONLY | O_CLOEXEC | flags);
}
static void close_fd(int fd) { syscall(SYS_close, fd); }

// Text building in fixed buffers: each call appends before limit and returns the new end, or NULL (also passed on
// by later calls) once the text does not fit, so a cut-off path is never used
static char *put_str(char *out, char *limit, const char *s) {
    for (; out && *s; s++) {
        if (out == limit) return NULL;
        *out++ = *s;
    }
    return out;
}
static char *put_uint(char *out, char *limit, uint32_t value, uint32_t base) {
    char digits[16], *d = digits + sizeof(digits);
    *--d = '\0';
    do *--d = "0123456789abcdef"[value % base];
    while (value /= base);
    return put_str(out, limit, d);
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses 1 to 8 hex digits at *s and advances *s past them
static int parse_hex(const char **s, uint32_t *value) {
    int n = 0;
    for (*value = 0; n < 8 && hex_digit(**s) >= 0; n++, (*s)++) *value = *value << 4 | (uint32_t)hex_digit(**s);
    return n > 0;
}

// Parses a /proc/driver/nvidia/gpus entry name, domain:bus:device.function
static int parse_pci_address(const char *s, uint32_t *domain, uint32_t *bus, uint32_t *device) {
    uint32_t function;
    return parse_hex(&s, domain) && *s++ == ':' && parse_hex(&s, bus) && *s++ == ':' && parse_hex(&s, device) &&
           *s++ == '.' && parse_hex(&s, &function) && *s == '\0';
}

// Finds the number on the "Device Minor:" line of an information file
static int parse_device_minor(const char *text, uint32_t *minor_id) {
    static const char key[] = "Device Minor:";
    const char *s = text;
    while (strncmp(s, key, sizeof(key) - 1) != 0) {
        s = strchr(s, '\n');
        if (!s) return 0;
        s++;
    }
    s += sizeof(key) - 1;
    while (*s == ' ' || *s == '\t') s++;
    int digits = 0;
    for (*minor_id = 0; *s >= '0' && *s <= '9' && digits < 9; s++, digits++)
        *minor_id = *minor_id * 10 + (uint32_t)(*s - '0');
    return digits > 0 && !(*s >= '0' && *s <= '9');
}

// Reads the file at path (relative to dir_fd) into buf as a string; -1 if it cannot be read or does not fit
static int read_text(int dir_fd, const char *path, char *buf, size_t size) {
    int fd = open_read(dir_fd, path, 0);
    if (fd < 0) return -1;
    size_t len = 0;
    long n;
    do {
        n = syscall(SYS_read, fd, buf + len, size - 1 - len);
        if (n > 0) len += (size_t)n;
    } while (n > 0 && len < size - 1);
    close_fd(fd);
    if (n != 0) return -1;
    buf[len] = '\0';
    return 0;
}

// Membership of the GPU whose device minor is minor_id, from its /dev/nvidia{minor} node
static int node_membership(uint32_t minor_id) {
    char path[512];
    char *limit = path + sizeof(path) - 1;
    char *end = put_uint(put_str(put_str(path, limit, DEV_DIR), limit, "/nvidia"), limit, minor_id, 10);
    struct stat st;
    if (!end) return UNKNOWN;
    *end = '\0';
    if (stat_path(path, &st) != 0) return errno == ENOENT ? ABSENT : UNKNOWN;
    return NODE_IS_GPU(st, minor_id) ? PRESENT : ABSENT;
}

// Writes the GPUs that CARD_INFO lists on the nvidiactl descriptor fd into gpus; returns their count, -1 on failure
static int read_card_info(int fd, host_gpu_t *gpus) {
    card_info_t cards[MAX_IDS];
    if (DRIVER_IOCTL(fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_CARD_INFO, card_info_t[MAX_IDS]), cards) != 0) return -1;
    int n = 0;
    for (int i = 0; i < MAX_IDS; i++)
        if (cards[i].valid)
            gpus[n++] = (host_gpu_t){cards[i].gpu_id, cards[i].pci_info.domain, cards[i].pci_info.bus,
                                     cards[i].pci_info.slot, node_membership(cards[i].minor_number)};
    return n;
}

// Membership of the GPU whose /proc entry is name, from the Device Minor in its information file
static int proc_gpu_membership(int dir_fd, const char *name) {
    char path[64], text[4096];
    char *limit = path + sizeof(path) - 1;
    char *end = put_str(put_str(path, limit, name), limit, "/information");
    uint32_t minor_id;
    if (!end) return UNKNOWN;
    *end = '\0';
    if (read_text(dir_fd, path, text, sizeof(text)) != 0 || !parse_device_minor(text, &minor_id)) return UNKNOWN;
    return node_membership(minor_id);
}

// Writes the GPUs in /proc/driver/nvidia/gpus into gpus; returns their count, 0 if the directory cannot be listed
static int read_proc_gpus(host_gpu_t *gpus) {
    int dir_fd = open_read(AT_FDCWD, PROC_GPUS_DIR, O_DIRECTORY);
    if (dir_fd < 0) return 0;
    uint64_t records[128];  // 8-byte aligned, as getdents64 records need
    long size = 0;
    int n = 0;
    while (n < MAX_HOST_GPUS && (size = syscall(SYS_getdents64, dir_fd, records, sizeof(records))) > 0)
        for (long offset = 0; offset < size && n < MAX_HOST_GPUS;) {
            const dirent64_t *entry = (const dirent64_t *)((const char *)records + offset);
            uint32_t domain, bus, device;
            offset += entry->d_reclen;
            if (parse_pci_address(entry->d_name, &domain, &bus, &device))
                gpus[n++] = (host_gpu_t){INVALID_ID, domain, bus & 0xff, device & 0xff,
                                         proc_gpu_membership(dir_fd, entry->d_name)};
        }
    close_fd(dir_fd);
    return size < 0 ? 0 : n;
}

// Membership of the GPU with this gpuId among the /proc GPUs, matched by the PCI address RM encodes in the id
static int proc_membership(uint32_t gpu_id, const host_gpu_t *gpus, int num_gpus) {
    uint32_t domain = gpu_id >> 16, bus = (gpu_id >> 8) & 0xff, device = gpu_id & 0xff;
    int exact = -1, same_bus_device = 0, last = -1;
    for (int i = 0; i < num_gpus; i++) {
        if (gpus[i].bus != bus || gpus[i].device != device) continue;
        same_bus_device++;
        last = i;
        if ((gpus[i].domain & 0xffff) == domain) exact = i;
    }
    // Some driver branches leave the PCI domain out of gpuId: an id with domain 0 is ambiguous if several GPUs match
    // on bus+device, and an id without an exact match is matched on bus+device if exactly one GPU does
    if (domain == 0 && same_bus_device > 1) return UNKNOWN;
    if (exact >= 0) return gpus[exact].membership;
    return same_bus_device == 1 ? gpus[last].membership : UNKNOWN;
}

static int membership_of(host_t *host, uint32_t gpu_id) {
    for (int i = 0; i < host->num_cards; i++)
        if (host->cards[i].gpu_id == gpu_id) return host->cards[i].membership;
    if (host->num_procs < 0) host->num_procs = read_proc_gpus(host->procs);
    return proc_membership(gpu_id, host->procs, host->num_procs);
}

// Whether at least one host GPU is known to be in the container and at least one known not to be, judged by
// CARD_INFO if it lists any GPU and by /proc otherwise
static int host_is_partial(host_t *host) {
    const host_gpu_t *gpus = host->cards;
    int num_gpus = host->num_cards, present = 0, absent = 0;
    if (num_gpus <= 0) {
        if (host->num_procs < 0) host->num_procs = read_proc_gpus(host->procs);
        gpus = host->procs;
        num_gpus = host->num_procs;
    }
    for (int i = 0; i < num_gpus; i++) {
        present += gpus[i].membership == PRESENT;
        absent += gpus[i].membership == ABSENT;
    }
    return present > 0 && absent > 0;
}

// Removes the entries whose GPU is certainly not in the container from count entries of stride words, each starting
// with a gpuId; flags, if not NULL, is a parallel array compacted along with them. Returns the new count; the list is
// left as is if nothing or everything would be removed
static uint32_t filter_entries(uint32_t *entries, uint32_t count, uint32_t stride, uint32_t *flags, host_t *host) {
    uint8_t keep[MAX_ACTIVE_DEVICES];
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        keep[i] = membership_of(host, entries[i * stride]) != ABSENT;
        kept += keep[i];
    }
    if (kept == 0 || kept == count) return count;
    for (uint32_t i = 0, w = 0; i < count; i++)
        if (keep[i]) {
            memmove(entries + w * stride, entries + i * stride, stride * sizeof(uint32_t));
            if (flags) flags[w] = flags[i];
            w++;
        }
    return kept;
}

static const char *caller_name(void *address) {
    struct dl_find_object object;
    const char *path = NULL;
    if (find_object && find_object(address, &object) == 0 && object.dlfo_link_map) path = object.dlfo_link_map->l_name;
    if (!path || !*path) return "?";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void debug_removed(void *caller, uint32_t cmd, uint32_t removed) {
    char line[512], *end;
    char *limit = line + sizeof(line);
    end = put_str(put_str(line, limit, "[nvcodec_container_fix] "), limit, caller_name(caller));
    end = put_uint(put_str(end, limit, " cmd=0x"), limit, cmd, 16);
    end = put_uint(put_str(end, limit, ": removed "), limit, removed, 10);
    end = put_str(end, limit, " gpus not in this container\n");
    if (end) syscall(SYS_write, STDERR_FILENO, line, (size_t)(end - line));
}

// Filters the result of a successful RM control on nvidiactl if it is one of the three GPU list queries above
static void filter_rm_control(int fd, rm_control_t *p, void *caller) {
    uint32_t *params = (uint32_t *)(uintptr_t)p->params;
    if (p->status != 0 || !params) return;
    int id_list = (p->cmd == CMD_GET_ATTACHED_IDS && p->paramsSize == MAX_IDS * 4) ||
                  (p->cmd == CMD_GET_PROBED_IDS && p->paramsSize == 3 * MAX_IDS * 4);
    if (!id_list && !(p->cmd == CMD_GET_ACTIVE_DEVICE_IDS && p->paramsSize == 4 + MAX_ACTIVE_DEVICES * 12)) return;
    host_t host;
    host.num_cards = read_card_info(fd, host.cards);
    host.num_procs = -1;
    if (!host_is_partial(&host)) return;
    uint32_t count, kept;
    if (id_list) {
        uint32_t *flags = p->cmd == CMD_GET_PROBED_IDS ? params + 2 * MAX_IDS : NULL;  // excludedGpuIds is left as is
        for (count = 0; count < MAX_IDS && params[count] != INVALID_ID;) count++;
        kept = filter_entries(params, count, 1, flags, &host);
        if (kept < count)
            for (uint32_t i = kept; i < MAX_IDS; i++) {
                params[i] = INVALID_ID;
                if (flags) flags[i] = 0;
            }
    } else {
        count = params[0] < MAX_ACTIVE_DEVICES ? params[0] : MAX_ACTIVE_DEVICES;
        kept = filter_entries(params + 1, count, 3, NULL, &host);
        if (kept < count) params[0] = kept;
    }
    if (debug && kept < count) debug_removed(caller, p->cmd, count - kept);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int ret = real_ioctl ? real_ioctl(fd, request, arg) : (int)syscall(SYS_ioctl, fd, request, arg);
    if (ret != 0 || disabled || !arg || _IOC_TYPE(request) != NV_IOCTL_MAGIC) return ret;
    int direct = _IOC_NR(request) == NV_ESC_RM_CONTROL && _IOC_SIZE(request) == sizeof(rm_control_t);
    int wrapped = _IOC_NR(request) == NV_ESC_IOCTL_XFER_CMD && _IOC_SIZE(request) == sizeof(xfer_t);
    if (!direct && !wrapped) return ret;
    int saved_errno = errno;
    struct stat st;
    if (stat_fd(fd, &st) == 0 && FD_IS_NVIDIACTL(st)) {  // only then is arg known to be the driver's struct
        rm_control_t *p = arg;
        if (wrapped) {  // the RM control travels by pointer; anything else in the wrapper is not ours to read
            const xfer_t *xfer = arg;
            p = xfer->cmd == NV_ESC_RM_CONTROL && xfer->size == sizeof(*p) ? (rm_control_t *)(uintptr_t)xfer->ptr
                                                                           : NULL;
        }
        if (p) filter_rm_control(fd, p, __builtin_return_address(0));
    }
    errno = saved_errno;
    return ret;
}
