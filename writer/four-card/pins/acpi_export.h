#ifndef CMP_ACPI_EXPORT_H
#define CMP_ACPI_EXPORT_H
#include <stdint.h>
#include <stddef.h>

/* Standalone, freestanding-compatible parser. No I/O except caller read callbacks. */
#define AE_TABLE_LIMIT (512U * 1024U)
#define AE_TOTAL_LIMIT (1024U * 1024U)
#define AE_MAX_TABLES 64U
#define AE_MAX_ENTRIES 128U
#define AE_MAX_CLAIMS (AE_MAX_ENTRIES + 4U)
enum ae_status {
    AE_OK, AE_BAD_INPUT, AE_BOUNDS, AE_READ, AE_SIGNATURE, AE_LENGTH,
    AE_CHECKSUM, AE_OVERLAP, AE_CYCLE, AE_MISSING_FADT, AE_DUPLICATE, AE_CHANGED
};
enum ae_kind { AE_RSDP, AE_XSDT, AE_RSDT, AE_FADT, AE_DSDT, AE_SSDT, AE_OTHER };
struct ae_reader {
    void *context;
    /* Both callbacks are required; range must return exactly 1. */
    int (*range)(void *, uint64_t, size_t);
    /* Exact count or failure; a short read is never retried. */
    int (*read)(void *, uint64_t, uint8_t *, size_t);
};
struct ae_table {
    enum ae_kind kind;
    uint64_t address;
    uint32_t length, workspace_offset;
    uint8_t revision;
};
/* Unknown tables: header only. Their body checksums are NOT validated. */
struct ae_skipped { uint64_t address; uint32_t length; uint8_t signature[4]; };
struct ae_result {
    enum ae_status status;
    uint64_t error_address;
    uint32_t error_length, table_count, total_bytes, skipped_count;
    unsigned route_authorized; /* Always 0, including success. No AML evaluation. */
    struct ae_table tables[AE_MAX_TABLES];
    struct ae_skipped skipped[AE_MAX_ENTRIES];
};
struct ae_claim { uint64_t address; uint32_t length; };
struct ae_context {
    const struct ae_reader *reader;
    struct ae_result *result;
    uint8_t *workspace;
    size_t capacity;
    uint64_t rsdp, root;
    unsigned claim_count;
    struct ae_claim claims[AE_MAX_CLAIMS];
};
static uint64_t ae_le(const uint8_t *p, unsigned n)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= (uint64_t)p[i] << (8U * i);
    return value;
}
static int ae_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static int ae_checksum(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}
static enum ae_status ae_fail(struct ae_context *c, enum ae_status status,
                              uint64_t address, size_t length)
{
    c->result->status = status;
    c->result->error_address = address;
    c->result->error_length = (uint32_t)length;
    return status;
}
static enum ae_status ae_range(struct ae_context *c, uint64_t a, size_t n)
{
    if (!a || !n || n > AE_TABLE_LIMIT || a > UINT64_MAX - (n - 1) ||
        c->reader->range(c->reader->context, a, n) != 1)
        return ae_fail(c, AE_BOUNDS, a, n);
    return AE_OK;
}
static enum ae_status ae_read(struct ae_context *c, uint64_t a, uint8_t *p, size_t n)
{
    if (ae_range(c, a, n) != AE_OK) return c->result->status;
    if (c->reader->read(c->reader->context, a, p, n) != (int)n)
        return ae_fail(c, AE_READ, a, n);
    return AE_OK;
}
/* Check even the initial header before dereferencing a new pointer. */
static enum ae_status ae_disjoint(struct ae_context *c, uint64_t a, size_t n)
{
    if (!a || !n || a > UINT64_MAX - (n - 1))
        return ae_fail(c, AE_BOUNDS, a, n);
    for (unsigned i = 0; i < c->claim_count; ++i) {
        struct ae_claim *old = &c->claims[i];
        if (a == old->address)
            return ae_fail(c, a == c->rsdp || a == c->root ? AE_CYCLE : AE_DUPLICATE, a, n);
        if (a <= old->address + old->length - 1 && old->address <= a + n - 1)
            return ae_fail(c, AE_OVERLAP, a, n);
    }
    return AE_OK;
}
static enum ae_status ae_claim(struct ae_context *c, uint64_t a, size_t n)
{
    if (ae_disjoint(c, a, n) != AE_OK) return c->result->status;
    if (ae_range(c, a, n) != AE_OK) return c->result->status;
    if (c->claim_count == AE_MAX_CLAIMS) return ae_fail(c, AE_LENGTH, a, n);
    c->claims[c->claim_count++] = (struct ae_claim){a, (uint32_t)n};
    return AE_OK;
}
static enum ae_status ae_save(struct ae_context *c, enum ae_kind kind,
                              uint64_t a, const uint8_t *header, size_t hn,
                              size_t length, uint8_t revision, uint8_t **saved)
{
    struct ae_result *r = c->result;
    if (r->table_count == AE_MAX_TABLES || length > AE_TABLE_LIMIT ||
        length > AE_TOTAL_LIMIT - r->total_bytes ||
        r->total_bytes > c->capacity || length > c->capacity - r->total_bytes)
        return ae_fail(c, AE_LENGTH, a, length);
    uint8_t *p = c->workspace + r->total_bytes;
    if (ae_read(c, a, p, length) != AE_OK) return r->status;
    if (!ae_equal(header, p, hn)) return ae_fail(c, AE_CHANGED, a, length);
    if (!ae_checksum(p, length)) return ae_fail(c, AE_CHECKSUM, a, length);
    r->tables[r->table_count++] = (struct ae_table){kind, a, (uint32_t)length,
                                                  r->total_bytes, revision};
    r->total_bytes += (uint32_t)length;
    *saved = p;
    return AE_OK;
}
static enum ae_kind ae_kind(const uint8_t *h)
{
    if (ae_equal(h, (const uint8_t *)"FACP", 4)) return AE_FADT;
    if (ae_equal(h, (const uint8_t *)"SSDT", 4)) return AE_SSDT;
    if (ae_equal(h, (const uint8_t *)"DSDT", 4)) return AE_DSDT;
    if (ae_equal(h, (const uint8_t *)"XSDT", 4)) return AE_XSDT;
    if (ae_equal(h, (const uint8_t *)"RSDT", 4)) return AE_RSDT;
    return AE_OTHER;
}
static enum ae_status ae_sdt(struct ae_context *c, uint64_t a,
                             enum ae_kind expected, uint8_t **saved)
{
    uint8_t header[36];
    *saved = NULL;
    if (ae_disjoint(c, a, sizeof(header)) != AE_OK ||
        ae_read(c, a, header, sizeof(header)) != AE_OK) return c->result->status;
    enum ae_kind kind = ae_kind(header);
    if (expected != AE_OTHER && expected != kind)
        return ae_fail(c, AE_SIGNATURE, a, sizeof(header));
    /* Root entries may be FADT, SSDT or unknown headers, never nested roots/DSDT. */
    if (expected == AE_OTHER && (kind == AE_XSDT || kind == AE_RSDT || kind == AE_DSDT))
        return ae_fail(c, AE_CYCLE, a, sizeof(header));
    for (unsigned i = 0; i < 4; ++i)
        if (!((header[i] >= 'A' && header[i] <= 'Z') ||
              (header[i] >= '0' && header[i] <= '9') || header[i] == '_'))
            return ae_fail(c, AE_SIGNATURE, a, sizeof(header));
    size_t length = (size_t)ae_le(header + 4, 4);
    if (length < sizeof(header) || length > AE_TABLE_LIMIT ||
        (kind == AE_FADT && length < 116))
        return ae_fail(c, AE_LENGTH, a, length);
    if (kind == AE_XSDT || kind == AE_RSDT) {
        size_t width = kind == AE_XSDT ? 8U : 4U;
        if (length == 36 || (length - 36) % width ||
            (length - 36) / width > AE_MAX_ENTRIES)
            return ae_fail(c, AE_LENGTH, a, length);
    }
    if (ae_claim(c, a, length) != AE_OK) return c->result->status;
    if (kind == AE_OTHER) {
        struct ae_result *r = c->result;
        if (r->skipped_count == AE_MAX_ENTRIES) return ae_fail(c, AE_LENGTH, a, length);
        struct ae_skipped *skipped = &r->skipped[r->skipped_count++];
        skipped->address = a; skipped->length = (uint32_t)length;
        for (unsigned i = 0; i < 4; ++i) skipped->signature[i] = header[i];
        return AE_OK;
    }
    return ae_save(c, kind, a, header, sizeof(header), length, header[8], saved);
}
static enum ae_status ae_export(const struct ae_reader *reader, uint64_t rsdp,
                                uint8_t *workspace, size_t capacity,
                                struct ae_result *result)
{
    if (!result) return AE_BAD_INPUT;
    /* Explicitly invalidate earlier results on every invocation. */
    result->status = AE_BAD_INPUT; result->route_authorized = 0;
    result->error_address = 0; result->error_length = 0;
    result->table_count = result->total_bytes = result->skipped_count = 0;
    if (!reader || !reader->range || !reader->read || !workspace || !capacity)
        return AE_BAD_INPUT;
    struct ae_context c = {.reader=reader, .result=result, .workspace=workspace,
                           .capacity=capacity, .rsdp=rsdp};
    uint8_t header[36], *rp = NULL, *root = NULL, *fadt = NULL;
    if (ae_read(&c, rsdp, header, 20) != AE_OK) return result->status;
    if (!ae_equal(header, (const uint8_t *)"RSD PTR ", 8))
        return ae_fail(&c, AE_SIGNATURE, rsdp, 20);
    if (!ae_checksum(header, 20)) return ae_fail(&c, AE_CHECKSUM, rsdp, 20);
    size_t rsdp_length = 20;
    uint8_t revision = header[15];
    if (revision == 1) return ae_fail(&c, AE_LENGTH, rsdp, 20);
    if (revision >= 2) {
        uint8_t extended[36];
        if (ae_read(&c, rsdp, extended, sizeof(extended)) != AE_OK) return result->status;
        if (!ae_equal(header, extended, 20)) return ae_fail(&c, AE_CHANGED, rsdp, 36);
        for (unsigned i = 0; i < 36; ++i) header[i] = extended[i];
        rsdp_length = (size_t)ae_le(header + 20, 4);
        if (rsdp_length < 36 || rsdp_length > 64)
            return ae_fail(&c, AE_LENGTH, rsdp, rsdp_length);
    }
    if (ae_claim(&c, rsdp, rsdp_length) != AE_OK ||
        ae_save(&c, AE_RSDP, rsdp, header, revision >= 2 ? 36 : 20,
                rsdp_length, revision, &rp) != AE_OK) return result->status;
    enum ae_kind root_kind = AE_RSDT;
    c.root = ae_le(rp + 16, 4);
    if (revision >= 2 && ae_le(rp + 24, 8)) {
        c.root = ae_le(rp + 24, 8); root_kind = AE_XSDT;
    }
    if (ae_sdt(&c, c.root, root_kind, &root) != AE_OK) return result->status;
    size_t root_length = (size_t)ae_le(root + 4, 4);
    unsigned width = root_kind == AE_XSDT ? 8U : 4U;
    size_t count = (root_length - 36) / width;
    if ((root_length - 36) % width || !count || count > AE_MAX_ENTRIES)
        return ae_fail(&c, AE_LENGTH, c.root, root_length);
    for (size_t i = 0; i < count; ++i) {
        uint64_t a = ae_le(root + 36 + width * i, width);
        uint8_t *child = NULL;
        if (ae_sdt(&c, a, AE_OTHER, &child) != AE_OK) return result->status;
        if (child && ae_kind(child) == AE_FADT) {
            if (fadt) return ae_fail(&c, AE_DUPLICATE, a, (size_t)ae_le(child + 4, 4));
            fadt = child;
        }
    }
    if (!fadt) return ae_fail(&c, AE_MISSING_FADT, c.root, root_length);
    uint64_t dsdt = ae_le(fadt + 40, 4);
    if (ae_le(fadt + 4, 4) >= 148 && ae_le(fadt + 140, 8)) dsdt = ae_le(fadt + 140, 8);
    uint8_t *dsdt_copy = NULL;
    if (ae_sdt(&c, dsdt, AE_DSDT, &dsdt_copy) != AE_OK) return result->status;
    result->status = AE_OK;
    return AE_OK;
}
#endif
