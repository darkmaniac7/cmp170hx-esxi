#ifndef EFIINV_PCI_DECODE_H
#define EFIINV_PCI_DECODE_H
#include <stdint.h>
#include <stddef.h>

/* Pure decoder: the only supplied operation is a read of one config dword. */
typedef int (*config_read32)(void *, uint32_t, uint32_t *);
enum cap_status { CAP_MISSING, CAP_FOUND, CAP_READ_ERROR, CAP_MALFORMED, CAP_CYCLE };
enum cap_reason {
    CAP_REASON_NONE, CAP_REASON_OFFSET, CAP_REASON_CYCLE, CAP_REASON_READ,
    CAP_REASON_ZERO_HEADER, CAP_REASON_ALL_ONES, CAP_REASON_VERSION,
    CAP_REASON_DUPLICATE_REBAR, CAP_REASON_REBAR_BOUNDS, CAP_REASON_COUNT,
    CAP_REASON_NEXT_OVERLAP, CAP_REASON_ENTRY_VALUE, CAP_REASON_DUPLICATE_BAR,
    CAP_REASON_CHANGED
};
struct rebar_entry { uint32_t offset, capability, control; };
struct cap_result {
    enum cap_status status;
    uint32_t offset, visited, count;
    uint32_t error_reason, error_offset, error_value;
    struct rebar_entry entries[6];
};
static struct cap_result cap_error(struct cap_result r, enum cap_status status,
                                  enum cap_reason reason, uint32_t offset, uint32_t value)
{
    r.status=status; r.error_reason=reason; r.error_offset=offset; r.error_value=value;
    return r;
}
static struct cap_result decode_rebar(config_read32 read, void *ctx)
{
    struct cap_result result = {0};
    uint8_t seen[1024] = {0};
    uint32_t offset = 0x100, head, next, i, cap, ctrl, first_ctrl, bars = 0;
    while (offset) {
        if (offset < 0x100 || offset > 0xffc || (offset & 3)) {
            return cap_error(result,CAP_MALFORMED,CAP_REASON_OFFSET,offset,0);
        }
        if (seen[offset / 4]) return cap_error(result,CAP_CYCLE,CAP_REASON_CYCLE,offset,0);
        seen[offset / 4] = 1; result.visited++;
        if (read(ctx, offset, &head)) return cap_error(result,CAP_READ_ERROR,CAP_REASON_READ,offset,0);
        if (head == 0) {
            if (result.visited == 1) return result;
            return cap_error(result,CAP_MALFORMED,CAP_REASON_ZERO_HEADER,offset,head);
        }
        if (head == UINT32_MAX)
            return cap_error(result,CAP_MALFORMED,CAP_REASON_ALL_ONES,offset,head);
        /* PCI-SIG assigns ID 0000h to the Null Capability. The actual GA100
         * chain contains 0xbb010000 at 0x900: version 1, next 0xbb0. Keep the
         * existing version/bounds/cycle guards; ID zero alone is not failure. */
        if (!((head >> 16) & 15))
            return cap_error(result,CAP_MALFORMED,CAP_REASON_VERSION,offset,head);
        next = head >> 20;
        if ((head & 0xffff) == 0x15) {
            if (result.offset) return cap_error(result,CAP_MALFORMED,CAP_REASON_DUPLICATE_REBAR,offset,head);
            if (offset > 0xff4) return cap_error(result,CAP_MALFORMED,CAP_REASON_REBAR_BOUNDS,offset,head);
            result.offset = offset;
            if (read(ctx, offset + 8, &ctrl)) return cap_error(result,CAP_READ_ERROR,CAP_REASON_READ,offset+8,0);
            first_ctrl = ctrl;
            result.count = (ctrl >> 5) & 7;
            /* Count is literal, not N-1. A zero count is retained as invalid. */
            if (!result.count || result.count > 6)
                return cap_error(result,CAP_MALFORMED,CAP_REASON_COUNT,offset+8,ctrl);
            if (offset + 4 + result.count * 8 > 4096)
                return cap_error(result,CAP_MALFORMED,CAP_REASON_REBAR_BOUNDS,offset,head);
            if (next > offset && offset + 4 + result.count * 8 > next)
                return cap_error(result,CAP_MALFORMED,CAP_REASON_NEXT_OVERLAP,offset,head);
            for (i = 0; i < result.count; i++) {
                if (read(ctx, offset + 4 + 8*i, &cap))
                    return cap_error(result,CAP_READ_ERROR,CAP_REASON_READ,offset+4+8*i,0);
                if (read(ctx, offset + 8 + 8*i, &ctrl))
                    return cap_error(result,CAP_READ_ERROR,CAP_REASON_READ,offset+8+8*i,0);
                if (i == 0 && ctrl != first_ctrl)
                    return cap_error(result,CAP_MALFORMED,CAP_REASON_CHANGED,offset+8,ctrl);
                result.entries[i] = (struct rebar_entry){offset + 4 + 8*i, cap, ctrl};
                if (cap == UINT32_MAX)
                    return cap_error(result,CAP_MALFORMED,CAP_REASON_ENTRY_VALUE,offset+4+8*i,cap);
                if (ctrl == UINT32_MAX || (ctrl & 7) > 5)
                    return cap_error(result,CAP_MALFORMED,CAP_REASON_ENTRY_VALUE,offset+8+8*i,ctrl);
                if (bars & (1U << (ctrl & 7)))
                    return cap_error(result,CAP_MALFORMED,CAP_REASON_DUPLICATE_BAR,offset+8+8*i,ctrl);
                bars |= 1U << (ctrl & 7);
            }
        }
        offset = next;
    }
    result.status = result.offset ? CAP_FOUND : CAP_MISSING;
    return result;
}

static uint64_t inv_le(const uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++) value |= (uint64_t)p[i] << (8*i);
    return value;
}
#endif
