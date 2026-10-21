#ifndef EFIINV_PCI_CONVENTIONAL_H
#define EFIINV_PCI_CONVENTIONAL_H
#include <stdint.h>
/* Read-only snapshot of the conventional capability chain, not an allocation
 * authorization. PCI_CONV_NO_EA is the only accepted complete result. Caller
 * must bind the initial identity and BDF to its separately captured inventory.
 * Constants: Linux v6.8 include/uapi/linux/pci_regs.h (PCI_STATUS_CAP_LIST,
 * PCI_HEADER_TYPE, PCI_CAPABILITY_LIST, PCI_CAP_ID_EA). Conservative policy:
 * do not mask malformed pointers or tolerate ff capability IDs.
 * ID zero is opaque, not a terminator: follow its checked next pointer as
 * Linux v6.8 drivers/pci/pci.c __pci_find_next_cap_ttl does. This proves only
 * traversal, not the semantics of an ID-zero capability.
 * https://raw.githubusercontent.com/torvalds/linux/v6.8/include/uapi/linux/pci_regs.h
 */
typedef int (*pci_conv_read32)(void *, uint32_t, uint32_t *);
enum pci_conv_status { PCI_CONV_INCOMPLETE, PCI_CONV_NO_EA, PCI_CONV_EA_PRESENT };
enum pci_conv_reason {
    PCI_CONV_REASON_NONE, PCI_CONV_REASON_ARGUMENT, PCI_CONV_REASON_READ,
    PCI_CONV_REASON_ALL_ONES, PCI_CONV_REASON_IDENTITY, PCI_CONV_REASON_HEADER_TYPE,
    PCI_CONV_REASON_FLAG_POINTER, PCI_CONV_REASON_OFFSET, PCI_CONV_REASON_CAP_ID,
    PCI_CONV_REASON_CYCLE, PCI_CONV_REASON_LIMIT, PCI_CONV_REASON_CHANGED,
    PCI_CONV_REASON_EA
};
struct pci_conv_result {
    uint32_t status, reason, visited, read_count, ea_present;
    uint32_t identity_raw, command_status_raw, header_raw, pointer_raw;
    uint32_t initial_valid, error_offset, error_value;
};
static struct pci_conv_result pci_conv_fail(struct pci_conv_result result,
                                          uint32_t reason, uint32_t offset,
                                          uint32_t value)
{
    result.status=PCI_CONV_INCOMPLETE;
    result.reason=reason; result.error_offset=offset; result.error_value=value;
    return result;
}
/* Output is initialized before callbacks; failed reads never attest raw data.
 * The adapter is responsible for reporting nonzero on short/failed reads and
 * bounding callback execution. This pure routine cannot interrupt a callback. */
static int pci_conv_get(pci_conv_read32 read, void *context,
                        struct pci_conv_result *result, uint32_t offset,
                        uint32_t *value)
{
    *value=0; result->read_count++;
    if (read(context,offset,value)) {
        *result=pci_conv_fail(*result,PCI_CONV_REASON_READ,offset,0);
        return 0;
    }
    if (*value==UINT32_MAX) {
        *result=pci_conv_fail(*result,PCI_CONV_REASON_ALL_ONES,offset,*value);
        return 0;
    }
    return 1;
}
static struct pci_conv_result pci_conventional_walk(pci_conv_read32 read, void *context)
{
    struct pci_conv_result result={0};
    uint32_t offsets[52]={0,4,12,52}, values[52]={0};
    uint32_t count=4, offset, head, i, value;
    uint64_t seen=0;
    if (!read) return pci_conv_fail(result,PCI_CONV_REASON_ARGUMENT,0,0);
    for (i=0;i<4;i++) {
        if (!pci_conv_get(read,context,&result,offsets[i],&values[i])) return result;
        result.initial_valid |= 1u<<i;
        switch (i) {
            case 0: result.identity_raw=values[i]; break;
            case 1: result.command_status_raw=values[i]; break;
            case 2: result.header_raw=values[i]; break;
            case 3: result.pointer_raw=values[i]; break;
        }
    }
    /* Device ID zero is valid. Vendor zero/ffff and device ffff are refused. */
    if (!(values[0]&0xffff) || (values[0]&0xffff)==0xffff || (values[0]>>16)==0xffff)
        return pci_conv_fail(result,PCI_CONV_REASON_IDENTITY,0,values[0]);
    if (((values[2]>>16)&0x7f)>1)
        return pci_conv_fail(result,PCI_CONV_REASON_HEADER_TYPE,12,values[2]);
    offset=values[3]&255;
    if (!!(values[1]&0x00100000u) != !!offset)
        return pci_conv_fail(result,PCI_CONV_REASON_FLAG_POINTER,52,values[3]);
    while (offset) {
        if (offset<0x40 || offset>0xfc || (offset&3))
            return pci_conv_fail(result,PCI_CONV_REASON_OFFSET,offset,offset);
        i=(offset-0x40)/4;
        if (seen & (UINT64_C(1)<<i))
            return pci_conv_fail(result,PCI_CONV_REASON_CYCLE,offset,0);
        if (result.visited>=48 || count>=52)
            return pci_conv_fail(result,PCI_CONV_REASON_LIMIT,offset,0);
        seen |= UINT64_C(1)<<i;
        result.visited++;
        if (!pci_conv_get(read,context,&result,offset,&head)) return result;
        offsets[count]=offset; values[count++]=head;
        if ((head&255)==255)
            return pci_conv_fail(result,PCI_CONV_REASON_CAP_ID,offset,head);
        if ((head&255)==0x14) result.ea_present=1;
        offset=(head>>8)&255;
    }
    /* Replay the exact captured sequence rather than follow changed pointers.
     * No masks: even a status or capability-specific upper-word change makes
     * this snapshot incomplete. Two matching passes are not an atomic lock. */
    for (i=0;i<count;i++) {
        if (!pci_conv_get(read,context,&result,offsets[i],&value)) return result;
        if (value!=values[i])
            return pci_conv_fail(result,PCI_CONV_REASON_CHANGED,offsets[i],value);
    }
    result.status=result.ea_present ? PCI_CONV_EA_PRESENT : PCI_CONV_NO_EA;
    result.reason=result.ea_present ? PCI_CONV_REASON_EA : PCI_CONV_REASON_NONE;
    return result;
}
#endif
