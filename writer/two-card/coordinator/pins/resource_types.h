#ifndef CMP_RESOURCE_TYPES_R1_H
#define CMP_RESOURCE_TYPES_R1_H
#include <stdint.h>
#include <stddef.h>
#include "two_card_policy.h"
#define RC_FUNCTIONS 112U
#define RC_ROOTS 4U
#define RC_QUERIES 568U
#define RC_LIST_MAX 8192U
#define RC_LIST_TOTAL_MAX (256U*1024U)
#define RC_MAP_MAX (256U*1024U)
#define RC_AML_TOTAL_MAX (1024U*1024U)
/* HOST-SPECIFIC: this host's BAR1 proposal window; regenerate for another host. */
#define RC_PROPOSAL_START UINT64_C(0x64000000000)
#define RC_PROPOSAL_END UINT64_C(0x65001ffffff)
struct rc_blob { const uint8_t *data; size_t size; };
/* Exact transport outcomes, not decoded approval flags. */
struct rc_read { uint32_t offset,value; uint64_t status; };
struct rc_function {
    uint32_t bdf; /* segment zero, bus<<8 | device<<3 | function */
    uint32_t header[16];
    const struct rc_read *conventional; size_t conventional_count;
};
struct rc_root { uint32_t segment; uint64_t status; struct rc_blob resources; };
struct rc_query {
    uint32_t owner_index,query_index; /* index into functions; never BAR binding */
    uint64_t status,supports; struct rc_blob resources;
};
struct rc_memory {
    uint64_t status,map_key; uint32_t descriptor_size,descriptor_version;
    struct rc_blob raw;
};
struct rc_acpi { struct rc_blob dsdt,ssdt[6]; };
struct rc_target_extcap { uint32_t bdf; const struct rc_read *trace; size_t count; };
struct rc_snapshot {
    const struct rc_function *functions; size_t function_count;
    const struct rc_root *roots; size_t root_count;
    const struct rc_query *queries; size_t query_count;
    struct rc_memory memory; struct rc_acpi acpi;
    uint32_t target_extcap_bdf;
    const struct rc_read *target_extcap; size_t target_extcap_count;
    /* Legacy fields above remain for unchanged single-card baseline callers.
     * The pair API ignores the legacy target trace, and requires these fields. */
    const struct pair_target *plans; size_t plan_count;
    struct rc_target_extcap target_extcaps[2];
    /* Per-function BAR dword observation bits, including paired upper dwords.
     * Set only after successful actual reads. Zero values remain valid.
     * This is collector completeness, not pointer freshness/authenticity. */
    uint8_t raw_slot_present[RC_FUNCTIONS];
};
enum rc_status { RC_REJECTED, RC_COVERED };
enum rc_reason { RC_NONE,RC_ARGUMENT,RC_IDENTITY,RC_CONVENTIONAL,RC_ROOT,
    RC_BRIDGE,RC_BAR,RC_QUERY,RC_MEMORY,RC_ACPI,RC_REBAR,RC_COLLISION };
struct rc_result {
    enum rc_status status; enum rc_reason reason; uint32_t owner,index;
    uint32_t raw_slot_count,bridge_count,query_count,query_memory_intervals;
    uint32_t efi_memory_intervals,upstream_bounded_64bit_bars,conventional_read_count;
    uint64_t aperture_start,aperture_end,proposal_start,proposal_end;
    uint64_t target_bases[3],target_sizes[3]; /* BAR0, BAR1, BAR3 */
    unsigned trusted_firmware_aperture_match,standard_bar_collision_excluded;
    unsigned hardware_decode_observed,hardware_authorized; /* always zero */
};
struct rc_pair_result {
    struct rc_result coverage; /* one shared inventory; legacy target facts=01 */
    uint64_t target_bases[2][3],target_sizes[2][3]; /* BAR0, BAR1, BAR3 */
    unsigned atomic_hardware_isolation_observed; /* always zero */
};
/* Defined in resource_validate.h; CPU-only. Inputs must remain stable throughout
 * this call. The adapter owns bounded collection/completeness and same-boot
 * rereads; this library cannot attest the origin or freshness of a pointer. */
#endif
