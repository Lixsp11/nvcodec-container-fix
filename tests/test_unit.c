// Unit tests that need no GPU. The library is compiled in with a mocked driver (RM controls through real_ioctl,
// NV_ESC_CARD_INFO through DRIVER_IOCTL) and mocked /proc/driver/nvidia/gpus and /dev directories under tmp/, removed
// at the end. Prints TAP.
// make test
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static char proc_dir[256], dev_dir[256];
static long fake_card_info(int fd, unsigned long request, void *arg);
#define PROC_GPUS_DIR proc_dir
#define DEV_DIR dev_dir
// The mocked /dev holds regular files: nvidia{minor} for a GPU node, and any regular file passes as nvidiactl
#define NODE_IS_GPU(st, minor_id) S_ISREG((st).st_mode)
#define FD_IS_NVIDIACTL(st) S_ISREG((st).st_mode)
#define DRIVER_IOCTL(fd, request, arg) fake_card_info(fd, request, arg)
#include "../nvcodec_container_fix.c"

#define NUM_GPUS 8
#define ALL_GPUS 0xffu
#define GPU(i) (1u << (i))
#define END INVALID_ID

typedef struct {
    uint32_t domain, bus, minor;
} fake_gpu_t;

static const fake_gpu_t default_gpus[NUM_GPUS] = {{0, 0x16, 0}, {0, 0x38, 1}, {0, 0x49, 2}, {0, 0x5a, 3},
                                                  {1, 0x98, 4}, {1, 0xb8, 5}, {1, 0xc8, 6}, {1, 0xd8, 40}};

// The mocked host: its GPUs, how the driver writes their ids, and how it answers
static struct {
    fake_gpu_t gpus[NUM_GPUS];
    enum { IDS_ENCODED, IDS_WITHOUT_DOMAIN, IDS_OPAQUE } ids;
    int card_info_fails, card_info_skips;  // card_info_skips: a GPU that CARD_INFO leaves out, or -1
    uint32_t extra_id;                     // listed after the GPUs unless INVALID_ID: a GPU nothing can place
    uint32_t status;                       // RM control status
    unsigned long card_info_request;       // the last CARD_INFO request
} fake;

static int ctl_fd, other_fd, errno_after, tests, failures;  // errno_after: errno right after the last RM control
static char scratch[] = "tmp/test_unit.XXXXXX";             // holds the mocked /proc, /dev and nvidiactl
static uint32_t answered[MAX_IDS];                          // the gpuIds the driver returned last
static uint32_t list[3 * MAX_IDS];                          // the params buffer the tests send
static const char *lookup = "";  // which lookup the current cases go through, as a prefix of their names

static uint32_t fake_id(int i) {
    if (fake.ids == IDS_OPAQUE) return 0x5a000000u | (uint32_t)i;  // no PCI address can be read from these
    return (fake.ids == IDS_WITHOUT_DOMAIN ? 0 : fake.gpus[i].domain << 16) | fake.gpus[i].bus << 8;
}

static void answer_rm_control(rm_control_t *p) {
    uint32_t *params = (uint32_t *)(uintptr_t)p->params;
    p->status = fake.status;
    if (p->cmd == CMD_GET_ACTIVE_DEVICE_IDS) {
        uint32_t n = 0;
        for (int i = 0; i < NUM_GPUS; i++)
            for (uint32_t instance = 0; instance < (i == 2 ? 2u : 1u); instance++, n++) {  // GPU 2: two MIG instances
                params[1 + 3 * n] = fake_id(i);
                params[2 + 3 * n] = i == 2 ? instance : INVALID_ID;
                params[3 + 3 * n] = INVALID_ID;
            }
        params[0] = n;
        return;
    }
    int n = 0;
    for (int i = 0; i < NUM_GPUS; i++) answered[n++] = fake_id(i);
    if (fake.extra_id != INVALID_ID) answered[n++] = fake.extra_id;
    while (n < MAX_IDS) answered[n++] = INVALID_ID;
    memcpy(params, answered, sizeof(answered));
    if (p->cmd == CMD_GET_PROBED_IDS)
        for (int i = 0; i < MAX_IDS; i++) {
            params[MAX_IDS + i] = i == 0 ? 0x77700 : INVALID_ID;             // excludedGpuIds
            params[2 * MAX_IDS + i] = i < NUM_GPUS ? 100 + (uint32_t)i : 0;  // gpuFlags
        }
}

// Stands in for the driver behind real_ioctl. The other device (a pipe) accepts the XFER request without reading it
static int fake_driver(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (_IOC_NR(request) == NV_ESC_RM_CONTROL)
        answer_rm_control(arg);
    else if (_IOC_NR(request) == NV_ESC_IOCTL_XFER_CMD && fd == ctl_fd)
        answer_rm_control((rm_control_t *)(uintptr_t)(*(const xfer_t *)arg).ptr);
    errno = 77;  // the library must leave errno as the driver set it
    return 0;
}

// Stands in for NV_ESC_CARD_INFO with the kernel's rules (nvidia_read_card_info): the size must be a nonzero multiple
// of the 72-byte entry with a slot for every GPU, and unused slots are zeroed. The offsets are those of nv-ioctl.h,
// not taken from the library's struct
static long fake_card_info(int fd, unsigned long request, void *arg) {
    enum { ENTRY = 72, VALID = 0, DOMAIN = 4, BUS = 8, GPU_ID = 16, MINOR = 56 };
    size_t size = _IOC_SIZE(request);
    fake.card_info_request = request;
    if (fake.card_info_fails || fd != ctl_fd || _IOC_TYPE(request) != 'F' || _IOC_NR(request) != 200 ||
        _IOC_DIR(request) != (_IOC_READ | _IOC_WRITE) || size == 0 || size % ENTRY != 0 || size / ENTRY < NUM_GPUS) {
        errno = EINVAL;
        return -1;
    }
    uint8_t *slot = arg;
    memset(slot, 0, size);
    for (int i = 0; i < NUM_GPUS; i++) {
        if (i == fake.card_info_skips) continue;
        uint32_t id = fake_id(i);
        slot[VALID] = 1;
        memcpy(slot + DOMAIN, &fake.gpus[i].domain, 4);
        slot[BUS] = (uint8_t)fake.gpus[i].bus;
        memcpy(slot + GPU_ID, &id, 4);
        memcpy(slot + MINOR, &fake.gpus[i].minor, 4);
        slot += ENTRY;
    }
    return 0;
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *walk) {
    (void)st, (void)type, (void)walk;
    return remove(path);
}

static void remove_tree(const char *path) { nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS); }

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f || fputs(text, f) < 0 || fclose(f) != 0) {
        perror(path);
        exit(2);
    }
}

static void gpu_proc_path(char *path, size_t size, int i, const char *leaf) {
    snprintf(path, size, "%s/%04x:%02x:00.0%s", proc_dir, fake.gpus[i].domain, fake.gpus[i].bus, leaf);
}

// Rebuilds the mocked /proc with an entry for each GPU in proc_gpus and /dev with a node for each GPU in dev_gpus
static void build(unsigned proc_gpus, unsigned dev_gpus) {
    char path[512], text[256];
    remove_tree(proc_dir);
    remove_tree(dev_dir);
    mkdir(proc_dir, 0755);
    mkdir(dev_dir, 0755);
    for (int i = 0; i < NUM_GPUS; i++) {
        const fake_gpu_t *g = &fake.gpus[i];
        if (proc_gpus & GPU(i)) {
            gpu_proc_path(path, sizeof(path), i, "");
            mkdir(path, 0755);
            snprintf(text, sizeof(text), "Bus Location: \t %04x:%02x:00.0\nDevice Minor: \t %u\nGPU Excluded:\t No\n",
                     g->domain, g->bus, g->minor);
            gpu_proc_path(path, sizeof(path), i, "/information");
            write_file(path, text);
        }
        if (dev_gpus & GPU(i)) {
            snprintf(path, sizeof(path), "%s/nvidia%u", dev_dir, g->minor);
            write_file(path, "");
        }
    }
}

static void reset(int card_info_fails) {
    memcpy(fake.gpus, default_gpus, sizeof(default_gpus));
    fake.ids = IDS_ENCODED;
    fake.card_info_fails = card_info_fails;
    fake.card_info_skips = -1;
    fake.extra_id = INVALID_ID;
    fake.status = 0;
}

static void report(int ok, const char *name, const char *detail) {
    printf("%s %d - %s%s%s%s\n", ok ? "ok" : "not ok", ++tests, lookup, name, ok ? "" : " # ", ok ? "" : detail);
    failures += !ok;
}

static int list_length(const uint32_t *ids) {
    int n = 0;
    while (n < MAX_IDS && ids[n] != INVALID_ID) n++;
    return n;
}

static void describe_list(char *out, size_t size, const uint32_t *ids) {
    int used = snprintf(out, size, "got");
    for (int i = 0; i < list_length(ids) && used < (int)size; i++)
        used += snprintf(out + used, size - (size_t)used, " %x", ids[i]);
}

// Checks that the gpuIds in list are exactly the ids given, which end with END
static void expect_ids(const char *name, ...) {
    uint32_t want[MAX_IDS];
    int n = 0, ok;
    va_list ap;
    va_start(ap, name);
    for (unsigned id = va_arg(ap, unsigned); id != END && n < MAX_IDS; id = va_arg(ap, unsigned)) want[n++] = id;
    va_end(ap);
    ok = list_length(list) == n && memcmp(list, want, (size_t)n * sizeof(want[0])) == 0;
    char detail[400];
    describe_list(detail, sizeof(detail), list);
    report(ok, name, detail);
}

static void expect_untouched(const char *name) {
    char detail[400];
    describe_list(detail, sizeof(detail), list);
    report(memcmp(list, answered, sizeof(answered)) == 0, name, detail);
}

// Sends an RM control as the driver libraries do, directly or wrapped in NV_ESC_IOCTL_XFER_CMD
static int send_rm_control(int fd, uint32_t cmd, uint32_t params_size, int wrapped) {
    rm_control_t p = {.hClient = 0xc1d00001u, .hObject = 0xc1d00001u, .cmd = cmd};
    p.params = (uint64_t)(uintptr_t)list;
    p.paramsSize = params_size;
    xfer_t x = {.cmd = NV_ESC_RM_CONTROL, .size = sizeof(p), .ptr = (uint64_t)(uintptr_t)&p};
    int ret = wrapped ? ioctl(fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_IOCTL_XFER_CMD, xfer_t), &x)
                      : ioctl(fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, rm_control_t), &p);
    errno_after = errno;
    return ret;
}

static void get_attached_ids(void) { send_rm_control(ctl_fd, CMD_GET_ATTACHED_IDS, MAX_IDS * 4, 0); }

// Cases that hold whichever way membership is found: through CARD_INFO, or through /proc when CARD_INFO fails
static void membership_cases(int card_info_fails) {
    lookup = card_info_fails ? "/proc: " : "CARD_INFO: ";

    reset(card_info_fails);
    build(ALL_GPUS, GPU(2));
    get_attached_ids();
    expect_ids("0x201, one GPU mounted: only it", 0x4900, END);
    report(errno_after == 77, "errno left as the driver set it", "");

    send_rm_control(ctl_fd, CMD_GET_PROBED_IDS, 3 * MAX_IDS * 4, 0);
    expect_ids("0x214, one GPU mounted: only it", 0x4900, END);
    report(list[2 * MAX_IDS] == 102 && list[2 * MAX_IDS + 1] == 0, "0x214 gpuFlags follow gpuIds", "");
    report(list[MAX_IDS] == 0x77700 && list[MAX_IDS + 1] == INVALID_ID, "0x214 excludedGpuIds untouched", "");

    send_rm_control(ctl_fd, CMD_GET_ACTIVE_DEVICE_IDS, 4 + MAX_ACTIVE_DEVICES * 12, 0);
    report(list[0] == 2 && list[1] == 0x4900 && list[2] == 0 && list[4] == 0x4900 && list[5] == 1,
           "0x288, one GPU mounted: both of its MIG entries kept", "");

    send_rm_control(ctl_fd, CMD_GET_ATTACHED_IDS, MAX_IDS * 4, 1);
    expect_ids("0x201 inside NV_ESC_IOCTL_XFER_CMD: filtered too", 0x4900, END);

    build(ALL_GPUS, GPU(1) | GPU(5));
    get_attached_ids();
    expect_ids("two GPUs mounted: both, in order", 0x3800, 0x1b800, END);

    build(ALL_GPUS, GPU(7));
    get_attached_ids();
    expect_ids("device minor above 31", 0x1d800, END);

    build(ALL_GPUS, ALL_GPUS);
    get_attached_ids();
    expect_untouched("all GPUs mounted: untouched");

    build(ALL_GPUS, 0);
    get_attached_ids();
    expect_untouched("no GPU mounted: untouched");

    build(ALL_GPUS, GPU(2));
    fake.extra_id = 0x7700;
    get_attached_ids();
    expect_ids("gpuId found nowhere: kept", 0x4900, 0x7700, END);
    fake.extra_id = INVALID_ID;

    fake.ids = IDS_WITHOUT_DOMAIN;
    build(ALL_GPUS, GPU(7));
    get_attached_ids();
    expect_ids("gpuIds without PCI domain", 0xd800, END);
    fake.ids = IDS_ENCODED;

    build(ALL_GPUS, GPU(2));
    char node[512];
    snprintf(node, sizeof(node), "%s/nvidia0", dev_dir);
    if (symlink("nvidia0", node) != 0) perror(node);  // stat fails with ELOOP: membership unknown
    get_attached_ids();
    expect_ids("GPU node that cannot be checked: kept", 0x1600, 0x4900, END);
    lookup = "";
}

static void request_cases(void) {
    reset(0);
    build(ALL_GPUS, GPU(2));
    get_attached_ids();
    report(fake.card_info_request == _IOWR('F', 200, uint8_t[MAX_IDS * 72]), "CARD_INFO asks for 32 slots of 72 bytes",
           "");

    fake.status = 0x1f;
    get_attached_ids();
    expect_untouched("driver status != 0: untouched");
    fake.status = 0;

    send_rm_control(ctl_fd, CMD_GET_ATTACHED_IDS, MAX_IDS * 4 + 4, 0);
    expect_untouched("paramsSize mismatch: untouched");

    send_rm_control(other_fd, CMD_GET_ATTACHED_IDS, MAX_IDS * 4, 0);
    expect_untouched("fd is not nvidiactl: untouched");

    send_rm_control(ctl_fd, 0x202, MAX_IDS * 4, 0);
    expect_untouched("other RM control: untouched");

    rm_control_t p = {.cmd = CMD_GET_ATTACHED_IDS, .params = (uint64_t)(uintptr_t)list, .paramsSize = MAX_IDS * 4};
    ioctl(ctl_fd, _IOWR('T', NV_ESC_RM_CONTROL, rm_control_t), &p);
    expect_untouched("ioctl type is not NVIDIA's 'F': untouched");

    disabled = 1;
    get_attached_ids();
    expect_untouched("NVCODEC_CONTAINER_FIX_DISABLE: untouched");
    disabled = 0;

    xfer_t bogus = {.cmd = NV_ESC_RM_CONTROL, .size = sizeof(rm_control_t), .ptr = 8};  // not a valid address
    int ret = ioctl(other_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_IOCTL_XFER_CMD, xfer_t), &bogus);
    report(ret == 0, "XFER request on another device: its pointer is not followed", "");
}

static void card_info_cases(void) {
    reset(0);
    build(GPU(2), GPU(2));  // legacy libnvidia-container mode: /proc shows only the container's GPU
    get_attached_ids();
    expect_ids("legacy /proc with CARD_INFO: hidden GPUs removed", 0x4900, END);

    fake.card_info_fails = 1;
    get_attached_ids();
    expect_untouched("legacy /proc without CARD_INFO: untouched, hidden GPUs unknown");
    fake.card_info_fails = 0;

    fake.ids = IDS_OPAQUE;
    build(ALL_GPUS, GPU(2));
    get_attached_ids();
    expect_ids("gpuIds without a PCI encoding: matched through CARD_INFO", 0x5a000002u, END);
    fake.card_info_fails = 1;
    get_attached_ids();
    expect_untouched("gpuIds without a PCI encoding and no CARD_INFO: untouched");
    reset(0);

    fake.card_info_skips = 7;
    build(ALL_GPUS, GPU(2) | GPU(7));
    get_attached_ids();
    expect_ids("GPU missing from CARD_INFO: placed through /proc", 0x4900, 0x1d800, END);
}

static void proc_cases(void) {
    lookup = "/proc: ";
    char path[512];
    reset(1);
    build(ALL_GPUS, GPU(2));
    gpu_proc_path(path, sizeof(path), 0, "/information");
    remove(path);
    mkdir(path, 0755);  // reading a directory fails
    get_attached_ids();
    expect_ids("unreadable information: kept", 0x1600, 0x4900, END);

    build(ALL_GPUS, GPU(2));
    write_file(path, "Bus Location: \t 0000:16:00.0\nGPU Excluded:\t No\n");
    get_attached_ids();
    expect_ids("information without Device Minor: kept", 0x1600, 0x4900, END);

    fake.gpus[5].bus = 0x38;  // 0001:38:00.0 mounted, 0000:38:00.0 hidden, ids without the domain
    fake.ids = IDS_WITHOUT_DOMAIN;
    build(ALL_GPUS, GPU(5));
    get_attached_ids();
    expect_ids("gpuId without domain matching GPUs in two domains: kept", 0x3800, 0x3800, END);
    lookup = "";
}

static void predicate_cases(void) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFCHR | 0666;
    st.st_rdev = makedev(195, 3);
    report(is_gpu_node(&st, 3) && !is_gpu_node(&st, 4) && !is_nvidiactl(&st), "character device 195:3 is GPU 3 only",
           "");
    st.st_rdev = makedev(195, 255);
    report(is_nvidiactl(&st), "character device 195:255 is nvidiactl", "");
    st.st_rdev = makedev(196, 3);
    report(!is_gpu_node(&st, 3), "major 196 is not NVIDIA's", "");
    st.st_mode = S_IFREG | 0666;
    st.st_rdev = makedev(195, 3);
    report(!is_gpu_node(&st, 3), "a regular file is not a GPU node", "");
}

int main(void) {
    char ctl_path[300];
    int pipe_fds[2];
    mkdir("tmp", 0755);
    if (!mkdtemp(scratch) || pipe(pipe_fds) != 0) {
        perror(scratch);
        return 2;
    }
    snprintf(proc_dir, sizeof(proc_dir), "%s/gpus", scratch);
    snprintf(dev_dir, sizeof(dev_dir), "%s/dev", scratch);
    snprintf(ctl_path, sizeof(ctl_path), "%s/nvidiactl", scratch);
    write_file(ctl_path, "");
    ctl_fd = open(ctl_path, O_RDONLY);
    other_fd = pipe_fds[0];
    real_ioctl = fake_driver;

    membership_cases(0);
    membership_cases(1);
    request_cases();
    card_info_cases();
    proc_cases();
    predicate_cases();

    remove_tree(scratch);
    printf("1..%d\n", tests);
    if (failures) printf("# %d of %d failed\n", failures, tests);
    return failures != 0;
}
