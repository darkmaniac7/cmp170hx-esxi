/*
 * cmp_pcie_speed: query or request a PCIe link speed change through the NVIDIA RM
 * control interface (NV2080_CTRL_CMD_BUS_SET_PCIE_SPEED family). Query mode (0) is
 * read-only and safe. Requesting speed 3 (Gen3) on an 8 GB CMP 170HX (10de:20c2)
 * leaves the card at Gen1 x8 until a power cycle; see README section 3. Do not.
 */
/*
 * cmp_pcie_speed: ask GSP-RM for a PCIe link speed on one CMP 170HX through
 * the RM control path and print the link state RM reports.
 *
 * Built from the 580.178.04 open kernel module headers, after the
 * lixiangwuxian/170hx_tips set_pcie_speed.c, with these changes:
 *   - the card is selected by its full BDF and matched on domain, bus AND slot
 *     (0000:0d:01.0 is slot 1; the guide matched slot 0 only);
 *   - the RM deviceInstance comes from NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, the
 *     /dev minor from /proc/driver/nvidia/gpus/<BDF>/information; they differ;
 *   - NV2080_CTRL_CMD_BUS_GET_INFO_V2 prints what RM believes about the link
 *     (GPU and upstream caps, control/status, gen info, speed-switch errors);
 *   - the device's own PCIe capability registers are read through the BAR0
 *     config mirror (sysfs resource0, read-only mmap), because the guest's
 *     config space is VMware's virtual 5 GT/s x32 link, not the card.
 *
 * usage: cmp_pcie_speed <DDDD:BB:SS.F> query|1|2|3|seq
 *   query  mapping, RM link info, mirror registers; no change
 *   1|2|3  one NV2080_CTRL_CMD_BUS_SET_PCIE_SPEED (Gen1 / Gen2 / Gen3)
 *   seq    1, 2, 3 with a one second pause, stopping at the first RM error
 * Never pass 2500/5000/8000. Root only (SET_PCIE_SPEED is a privileged control).
 * Exit 0 when every requested RM call returned NV_OK; "seq" and "3" also need
 * the mirror LnkSta speed to read 3 (8 GT/s). The ESXi host's LnkSta is the
 * verdict either way.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nvtypes.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nv-ioctl.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"

#define RM_ALLOC_CMD    _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, NVOS21_PARAMETERS)
#define RM_CONTROL_CMD  _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, NVOS54_PARAMETERS)
#define RM_FREE_CMD     _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE, NVOS00_PARAMETERS)
#define WAIT_OPEN_CMD   _IOWR(NV_IOCTL_MAGIC, NV_ESC_WAIT_OPEN_COMPLETE, nv_ioctl_wait_open_complete_t)
#define REGISTER_FD_CMD _IOWR(NV_IOCTL_MAGIC, NV_ESC_REGISTER_FD, nv_ioctl_register_fd_t)

#define MIRROR_LNKCAP   0x088084U
#define MIRROR_LNKCTLSTA 0x088088U
#define MIRROR_LNKCTL2  0x0880a8U
#define MIRROR_XP_CFG0  0x08c040U
#define MIRROR_XP_LCTRL2 0x08c1c0U

static const char *speed_name(unsigned s)
{
    static const char *n[] = { "?", "2.5GT/s", "5GT/s", "8GT/s", "16GT/s", "32GT/s", "64GT/s" };
    return (s < sizeof(n) / sizeof(n[0])) ? n[s] : "?";
}

static int rm_control(int fd, NvHandle hClient, NvHandle hObject, NvU32 cmd,
                      void *params, NvU32 size, NvU32 *pStatus)
{
    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClient = hClient;
    p.hObject = hObject;
    p.cmd = cmd;
    p.params = NV_PTR_TO_NvP64(params);
    p.paramsSize = size;
    if (ioctl(fd, RM_CONTROL_CMD, &p) != 0)
    {
        fprintf(stderr, "ioctl RM_CONTROL 0x%08x: %s\n", cmd, strerror(errno));
        *pStatus = 0xffffffffU;
        return -1;
    }
    *pStatus = p.status;
    return (p.status == 0) ? 0 : -1;
}

static int rm_alloc(int fd, NvHandle hRoot, NvHandle hParent, NvU32 hClass,
                    void *params, NvU32 size, NvHandle *phNew, NvU32 *pStatus)
{
    NVOS21_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot = hRoot;
    p.hObjectParent = hParent;
    p.hObjectNew = 0;
    p.hClass = hClass;
    p.pAllocParms = NV_PTR_TO_NvP64(params);
    p.paramsSize = size;
    if (ioctl(fd, RM_ALLOC_CMD, &p) != 0)
    {
        fprintf(stderr, "ioctl RM_ALLOC class 0x%x: %s\n", hClass, strerror(errno));
        *pStatus = 0xffffffffU;
        return -1;
    }
    *pStatus = p.status;
    *phNew = p.hObjectNew;
    return (p.status == 0) ? 0 : -1;
}

static void rm_free(int fd, NvHandle hRoot, NvHandle hParent, NvHandle hObject)
{
    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot = hRoot;
    p.hObjectParent = hParent;
    p.hObjectOld = hObject;
    (void)ioctl(fd, RM_FREE_CMD, &p);
}

static int read_minor(const char *bdf)
{
    char path[160], line[256];
    FILE *f;
    int minor = -1;
    snprintf(path, sizeof(path), "/proc/driver/nvidia/gpus/%s/information", bdf);
    f = fopen(path, "r");
    if (f == NULL)
    {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "Device Minor:", 13) == 0)
        {
            minor = atoi(line + 13);
            break;
        }
    }
    fclose(f);
    return minor;
}

/* Read-only BAR0 config-mirror snapshot of the card's PCIe capability. */
static int mirror_state(const char *bdf, unsigned *pSpeed, unsigned *pWidth, const char *tag)
{
    char path[160];
    int fd;
    NvU32 cap = 0, ctlsta = 0, ctl2 = 0, cfg0 = 0, lctrl2 = 0;
    unsigned char *m;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);
    fd = open(path, O_RDONLY | O_SYNC);
    if (fd < 0)
    {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x88000);
    if (m != MAP_FAILED)
    {
        memcpy(&cap, m + (MIRROR_LNKCAP & 0xfff), 4);
        memcpy(&ctlsta, m + (MIRROR_LNKCTLSTA & 0xfff), 4);
        memcpy(&ctl2, m + (MIRROR_LNKCTL2 & 0xfff), 4);
        munmap(m, 0x1000);
    }
    m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x8c000);
    if (m != MAP_FAILED)
    {
        memcpy(&cfg0, m + (MIRROR_XP_CFG0 & 0xfff), 4);
        memcpy(&lctrl2, m + (MIRROR_XP_LCTRL2 & 0xfff), 4);
        munmap(m, 0x1000);
    }
    close(fd);

    *pSpeed = (ctlsta >> 16) & 0xf;
    *pWidth = (ctlsta >> 20) & 0x3f;
    printf("%s mirror: LnkCap=0x%08x (max %s x%u) LnkSta=0x%04x (%s x%u) LnkCtl2=0x%04x (target %s) "
           "XP_CFG0=0x%08x (rate[19:18]=%u) XP_LCTRL2=0x%08x (target[19:16]=%u)\n",
           tag, cap, speed_name(cap & 0xf), (cap >> 4) & 0x3f,
           ctlsta >> 16, speed_name(*pSpeed), *pWidth,
           ctl2 & 0xffff, speed_name(ctl2 & 0xf),
           cfg0, (cfg0 >> 18) & 3, lctrl2, (lctrl2 >> 16) & 0xf);
    return 0;
}

static void rm_link_info(int fd, NvHandle hClient, NvHandle hSub, const char *tag)
{
    static const struct { NvU32 index; const char *name; } idx[] = {
        { NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CAPS, "gpu_link_caps" },
        { NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CTRL_STATUS, "gpu_link_ctrl_status" },
        { NV2080_CTRL_BUS_INFO_INDEX_PCIE_UPSTREAM_LINK_CAPS, "upstream_link_caps" },
        { NV2080_CTRL_BUS_INFO_INDEX_PCIE_UPSTREAM_LINK_CTRL_STATUS, "upstream_link_ctrl_status" },
        { NV2080_CTRL_BUS_INFO_INDEX_PCIE_GEN_INFO, "pcie_gen_info" },
        { NV2080_CTRL_BUS_INFO_INDEX_PCIE_UPSTREAM_GEN_INFO, "upstream_gen_info" },
        { NV2080_CTRL_BUS_INFO_INDEX_PCIE_LINK_SPEED_SWITCH_ERROR_COUNT, "speed_switch_errors" },
        { NV2080_CTRL_BUS_INFO_INDEX_INTERFACE_TYPE, "interface_type" },
    };
    NV2080_CTRL_BUS_GET_INFO_V2_PARAMS p;
    NvU32 status, i, n = sizeof(idx) / sizeof(idx[0]);

    memset(&p, 0, sizeof(p));
    p.busInfoListSize = n;
    for (i = 0; i < n; i++)
        p.busInfoList[i].index = idx[i].index;
    if (rm_control(fd, hClient, hSub, NV2080_CTRL_CMD_BUS_GET_INFO_V2, &p, sizeof(p), &status) != 0)
    {
        printf("%s rm: BUS_GET_INFO_V2 failed status=0x%x\n", tag, status);
        return;
    }
    printf("%s rm:", tag);
    for (i = 0; i < n; i++)
    {
        NvU32 d = p.busInfoList[i].data;
        printf(" %s=0x%08x", idx[i].name, d);
        if (idx[i].index == NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CAPS ||
            idx[i].index == NV2080_CTRL_BUS_INFO_INDEX_PCIE_UPSTREAM_LINK_CAPS)
            printf("(max %s x%u gen%u)", speed_name(d & 0xf), (d >> 4) & 0x3f, ((d >> 12) & 0xf) + 1);
        if (idx[i].index == NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CTRL_STATUS ||
            idx[i].index == NV2080_CTRL_BUS_INFO_INDEX_PCIE_UPSTREAM_LINK_CTRL_STATUS)
            printf("(%s x%u)", speed_name((d >> 16) & 0xf), (d >> 20) & 0x3f);
        if (idx[i].index == NV2080_CTRL_BUS_INFO_INDEX_PCIE_GEN_INFO ||
            idx[i].index == NV2080_CTRL_BUS_INFO_INDEX_PCIE_UPSTREAM_GEN_INFO)
            printf("(cap gen%u curr gen%u gpu gen%u)", ((d >> 12) & 0xf) + 1, ((d >> 16) & 0xf) + 1, ((d >> 20) & 0xf) + 1);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *bdf, *mode;
    unsigned domain, bus, slot, func, speeds[3], nspeeds = 0, want_final = 0, i;
    int minor, gfd, fd, rc = 0;
    nv_ioctl_wait_open_complete_t wait_open;
    nv_ioctl_register_fd_t reg_fd;
    NvHandle hClient = 0, hDevice = 0, hSub = 0;
    NvU32 status, instance = ~0U, subInstance = 0, gpuId = 0;
    NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS ids;
    unsigned mspeed = 0, mwidth = 0;

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <DDDD:BB:SS.F> query|1|2|3|seq\n", argv[0]);
        return 2;
    }
    bdf = argv[1];
    mode = argv[2];
    if (sscanf(bdf, "%4x:%2x:%2x.%1x", &domain, &bus, &slot, &func) != 4 || strlen(bdf) != 12)
    {
        fprintf(stderr, "BDF must look like 0000:0d:01.0\n");
        return 2;
    }
    if (strcmp(mode, "query") == 0)
        nspeeds = 0;
    else if (strcmp(mode, "seq") == 0)
    {
        speeds[0] = 1; speeds[1] = 2; speeds[2] = 3; nspeeds = 3; want_final = 3;
    }
    else if (strlen(mode) == 1 && mode[0] >= '1' && mode[0] <= '3')
    {
        speeds[0] = (unsigned)(mode[0] - '0'); nspeeds = 1; want_final = (speeds[0] == 3) ? 3 : 0;
    }
    else
    {
        fprintf(stderr, "mode must be query, 1, 2, 3 or seq (never 2500/5000/8000)\n");
        return 2;
    }

    minor = read_minor(bdf);
    if (minor < 0)
    {
        fprintf(stderr, "Device Minor not found for %s\n", bdf);
        return 1;
    }

    mirror_state(bdf, &mspeed, &mwidth, "before");

    {
        char devpath[32];
        snprintf(devpath, sizeof(devpath), "/dev/nvidia%d", minor);
        gfd = open(devpath, O_RDWR);
        if (gfd < 0) { perror(devpath); return 1; }
    }
    fd = open("/dev/nvidiactl", O_RDWR);
    if (fd < 0) { perror("/dev/nvidiactl"); return 1; }

    memset(&wait_open, 0, sizeof(wait_open));
    if (ioctl(gfd, WAIT_OPEN_CMD, &wait_open) < 0 || wait_open.rc != 0 || wait_open.adapterStatus != 0)
    {
        fprintf(stderr, "GPU open did not complete: rc=%d adapterStatus=0x%x (%s)\n",
                wait_open.rc, wait_open.adapterStatus, strerror(errno));
        return 1;
    }
    reg_fd.ctl_fd = fd;
    if (ioctl(gfd, REGISTER_FD_CMD, &reg_fd) < 0)
    {
        perror("NV_ESC_REGISTER_FD");
        return 1;
    }

    if (rm_alloc(fd, 0, 0, NV01_ROOT, NULL, 0, &hClient, &status) != 0)
    {
        fprintf(stderr, "root client alloc failed status=0x%x\n", status);
        return 1;
    }

    memset(&ids, 0xff, sizeof(ids));
    if (rm_control(fd, hClient, hClient, NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS, &ids, sizeof(ids), &status) != 0)
    {
        fprintf(stderr, "GET_ATTACHED_IDS failed status=0x%x\n", status);
        rc = 1;
        goto out;
    }
    for (i = 0; i < NV0000_CTRL_GPU_MAX_ATTACHED_GPUS && ids.gpuIds[i] != NV0000_CTRL_GPU_INVALID_ID; i++)
    {
        NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS pci;
        NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS info;

        memset(&pci, 0, sizeof(pci));
        pci.gpuId = ids.gpuIds[i];
        if (rm_control(fd, hClient, hClient, NV0000_CTRL_CMD_GPU_GET_PCI_INFO, &pci, sizeof(pci), &status) != 0)
        {
            fprintf(stderr, "GET_PCI_INFO(gpuId 0x%x) failed status=0x%x\n", pci.gpuId, status);
            rc = 1;
            goto out;
        }
        if (pci.domain != domain || pci.bus != bus || pci.slot != slot)
            continue;
        memset(&info, 0, sizeof(info));
        info.gpuId = pci.gpuId;
        if (rm_control(fd, hClient, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &info, sizeof(info), &status) != 0)
        {
            fprintf(stderr, "GET_ID_INFO_V2(gpuId 0x%x) failed status=0x%x\n", pci.gpuId, status);
            rc = 1;
            goto out;
        }
        gpuId = pci.gpuId;
        instance = info.deviceInstance;
        subInstance = info.subDeviceInstance;
        printf("%s: gpuId=0x%x deviceInstance=%u subDeviceInstance=%u gpuInstance=%u minor=%d\n",
               bdf, gpuId, instance, subInstance, info.gpuInstance, minor);
        break;
    }
    if (instance == ~0U)
    {
        fprintf(stderr, "%s is not among the attached GPUs\n", bdf);
        rc = 1;
        goto out;
    }

    {
        NV0080_ALLOC_PARAMETERS dev;
        NV2080_ALLOC_PARAMETERS sub;
        memset(&dev, 0, sizeof(dev));
        dev.deviceId = instance;
        if (rm_alloc(fd, hClient, hClient, NV01_DEVICE_0, &dev, sizeof(dev), &hDevice, &status) != 0)
        {
            fprintf(stderr, "device alloc failed status=0x%x\n", status);
            rc = 1;
            goto out;
        }
        memset(&sub, 0, sizeof(sub));
        sub.subDeviceId = subInstance;
        if (rm_alloc(fd, hClient, hDevice, NV20_SUBDEVICE_0, &sub, sizeof(sub), &hSub, &status) != 0)
        {
            fprintf(stderr, "subdevice alloc failed status=0x%x\n", status);
            rc = 1;
            goto out;
        }
    }

    rm_link_info(fd, hClient, hSub, "before");

    for (i = 0; i < nspeeds; i++)
    {
        NV2080_CTRL_BUS_SET_PCIE_SPEED_PARAMS sp;
        char tag[16];
        memset(&sp, 0, sizeof(sp));
        sp.busSpeed = speeds[i];
        if (rm_control(fd, hClient, hSub, NV2080_CTRL_CMD_BUS_SET_PCIE_SPEED, &sp, sizeof(sp), &status) != 0)
        {
            printf("SET_PCIE_SPEED(%u) FAILED status=0x%x\n", speeds[i], status);
            rc = 1;
            break;
        }
        printf("SET_PCIE_SPEED(%u) OK\n", speeds[i]);
        sleep(1);
        snprintf(tag, sizeof(tag), "after gen%u", speeds[i]);
        mirror_state(bdf, &mspeed, &mwidth, tag);
        rm_link_info(fd, hClient, hSub, tag);
    }

    if (nspeeds > 0)
    {
        mirror_state(bdf, &mspeed, &mwidth, "final");
        if (want_final != 0 && rc == 0)
        {
            if (mspeed == want_final)
                printf("RESULT: card-side LnkSta %s x%u (host-side vsish read still required)\n",
                       speed_name(mspeed), mwidth);
            else
            {
                printf("RESULT: card-side LnkSta %s x%u, not %s\n",
                       speed_name(mspeed), mwidth, speed_name(want_final));
                rc = 1;
            }
        }
    }

out:
    if (hClient != 0)
        rm_free(fd, hClient, hClient, hClient);
    close(fd);
    close(gfd);
    return rc;
}
