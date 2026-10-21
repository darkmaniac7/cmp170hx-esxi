#ifndef CMP_ACPI_RANGES_H
#define CMP_ACPI_RANGES_H
#include <stdint.h>
#include <stddef.h>

/* Adapter for a separately validated, copied UEFI memory map; no EFI calls. */
#define AE_RAM_MAX_RANGES 1024U
#define AE_RAM_READ_LIMIT (512U * 1024U)
struct ae_ram_range { uint64_t address, length; uint32_t memory_type; };
struct ae_ram_adapter {
    unsigned valid;
    size_t count;
    struct ae_ram_range ranges[AE_RAM_MAX_RANGES];
    /* Provenance supplied by the caller: SHA256 of its immutable raw map. */
    uint8_t memory_map_sha256[32];
    void *copy_context;
    int (*copy_bytes)(void *, uint64_t, uint8_t *, size_t);
};
static int ae_ram_init(struct ae_ram_adapter *a, const struct ae_ram_range *ranges,
                       size_t count, const uint8_t digest[32],
                       int (*copy_bytes)(void *, uint64_t, uint8_t *, size_t),
                       void *copy_context)
{
    if (!a) return 0;
    a->valid = 0; a->count = 0; a->copy_bytes = NULL; a->copy_context = NULL;
    if (!ranges || !digest || !copy_bytes || !count || count > AE_RAM_MAX_RANGES) return 0;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 32; ++i) nonzero |= digest[i];
    if (!nonzero) return 0;
    for (size_t i = 0; i < count; ++i) {
        const struct ae_ram_range *r = &ranges[i];
        /* Known UEFI memory types only; type 16 is the sentinel, not memory. */
        if (!r->length || r->address > UINT64_MAX - (r->length - 1) ||
            r->memory_type >= 16) return 0;
        for (size_t j = 0; j < i; ++j)
            if (r->address <= ranges[j].address + ranges[j].length - 1 &&
                ranges[j].address <= r->address + r->length - 1) return 0;
        a->ranges[i] = *r;
    }
    for (unsigned i = 0; i < 32; ++i) a->memory_map_sha256[i] = digest[i];
    a->count = count; a->copy_bytes = copy_bytes; a->copy_context = copy_context;
    a->valid = 1;
    return 1;
}
static int ae_ram_contains(void *context, uint64_t address, size_t length)
{
    const struct ae_ram_adapter *a = context;
    if (!a || !a->valid || !address || !length || length > AE_RAM_READ_LIMIT ||
        address > UINT64_MAX - (length - 1)) return 0;
    for (size_t i = 0; i < a->count; ++i) {
        const struct ae_ram_range *r = &a->ranges[i];
        /* Only ACPI reclaim memory (9) and ACPI NVS (10). Never MMIO. */
        if ((r->memory_type == 9 || r->memory_type == 10) &&
            address >= r->address && address - r->address < r->length &&
            length <= r->length - (address - r->address)) return 1;
    }
    /* Reject crossing even adjacent approved ranges: no implicit coalescing. */
    return 0;
}
static int ae_ram_read(void *context, uint64_t address, uint8_t *out, size_t length)
{
    struct ae_ram_adapter *a = context;
    if (!out || !ae_ram_contains(context, address, length)) return -1;
    return a->copy_bytes(a->copy_context, address, out, length);
}
#endif
