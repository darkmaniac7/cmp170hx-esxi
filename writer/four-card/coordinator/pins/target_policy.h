#ifndef CMP_TARGET_POLICY_R1_H
#define CMP_TARGET_POLICY_R1_H
#include <stdint.h>
#include <stddef.h>
/* Fixed four-card cold-layout policy, not an allocator or hardware authorization.
 * Order is the transaction order: 01, 02 (root PCI0, bus 00-3f) then 47, 48
 * (root \_SB.S0D2, bus 40-7f). Placement rule for every card: BAR1 at the lowest
 * 64 GiB-aligned base inside its root's DSDT aperture, BAR3 at the next 64 GiB
 * boundary, one 0x1002000000 reservation, 128 GiB stride (docs/FOUR_CARD_PLACEMENT.md).
 * The two root apertures are the DSDT MABL/MALL intervals (S0D2: BRB 0x40,
 * BRL 0x40, MABL 0x47e20200000, MALL 0x1bedfe00000); both roots expose unowned
 * MMIO (0x47e20200000-0x63cefffffff and 0x63d20200000-0x7fbefffffff).
 * Trusted firmware aperture assumption only. */
/* HOST-SPECIFIC: the endpoint/port/bus and BAR placement addresses in the tables
 * below are this host's measured values and must be regenerated for another host;
 * the writer refuses on any mismatch by design. */
#define PT_COUNT 4U
#define PT_ALL_MASK ((1U<<PT_COUNT)-1U)
struct pair_target {
 uint32_t endpoint,port,bus;
 uint64_t bar0,bar1,bar3,bar1_size,bar3_size,reservation_size;
};
struct pair_root {uint32_t first_bus,last_bus;uint64_t aperture_start,aperture_end;};
static const struct pair_target pair_targets[PT_COUNT]={
 {0x0100,0x0009,0x01,UINT64_C(0xf3000000),UINT64_C(0x64000000000),UINT64_C(0x65000000000),
  UINT64_C(0x1000000000),UINT64_C(0x02000000),UINT64_C(0x1002000000)},
 {0x0200,0x000b,0x02,UINT64_C(0xf2000000),UINT64_C(0x66000000000),UINT64_C(0x67000000000),
  UINT64_C(0x1000000000),UINT64_C(0x02000000),UINT64_C(0x1002000000)},
 {0x4700,0x4019,0x47,UINT64_C(0xb5000000),UINT64_C(0x48000000000),UINT64_C(0x49000000000),
  UINT64_C(0x1000000000),UINT64_C(0x02000000),UINT64_C(0x1002000000)},
 {0x4800,0x401b,0x48,UINT64_C(0xb4000000),UINT64_C(0x4a000000000),UINT64_C(0x4b000000000),
  UINT64_C(0x1000000000),UINT64_C(0x02000000),UINT64_C(0x1002000000)}
};
/* Per-target root: PCI0 for targets 0/1, S0D2 for targets 2/3. */
static const struct pair_root pair_roots[PT_COUNT]={
 {0x00,0x3f,UINT64_C(0x63d20200000),UINT64_C(0x7fbffffffff)},
 {0x00,0x3f,UINT64_C(0x63d20200000),UINT64_C(0x7fbffffffff)},
 {0x40,0x7f,UINT64_C(0x47e20200000),UINT64_C(0x63cffffffff)},
 {0x40,0x7f,UINT64_C(0x47e20200000),UINT64_C(0x63cffffffff)}
};
/* Retained cold CONFIG32 values (endpoint owners 01:00.0/02:00.0/47:00.0/48:00.0,
 * ports 00:01.1, 00:01.3, 40:03.1, 40:03.3). Neither an already enlarged layout
 * nor a partial layout resumes: every target must show its cold layout, so the
 * writer is strictly a cold-boot-only tool. These are this host's values. */
static const uint64_t pair_cold_bases[PT_COUNT][3]={
 {UINT64_C(0xf3000000),UINT64_C(0x7fbf8000000),UINT64_C(0x7fbfc000000)},
 {UINT64_C(0xf2000000),UINT64_C(0x7fbf0000000),UINT64_C(0x7fbf4000000)},
 {UINT64_C(0xb5000000),UINT64_C(0x63cf8000000),UINT64_C(0x63cfc000000)},
 {UINT64_C(0xb4000000),UINT64_C(0x63cf0000000),UINT64_C(0x63cf4000000)}
};
static const uint32_t pair_cold_windows[PT_COUNT][4]={
 {0xf3f0f300,0xfdf1f801,0x7fb,0x7fb},
 {0xf2f0f200,0xf5f1f001,0x7fb,0x7fb},
 {0xb5f0b500,0xfdf1f801,0x63c,0x63c},
 {0xb4f0b400,0xf5f1f001,0x63c,0x63c}
};
static int pair_prefix_mask(unsigned mask){return mask<=PT_ALL_MASK&&!(mask&(mask+1));}
static int pair_policy_matches(const struct pair_target *p,size_t count)
{
 if(!p || count!=PT_COUNT)return 0;
 for(unsigned i=0;i<PT_COUNT;i++){
  const struct pair_target *v=&p[i],*pin=&pair_targets[i];
  if(v->endpoint!=pin->endpoint || v->port!=pin->port || v->bus!=pin->bus ||
     v->bar0!=pin->bar0 || v->bar1!=pin->bar1 || v->bar3!=pin->bar3 ||
     v->bar1_size!=pin->bar1_size || v->bar3_size!=pin->bar3_size ||
     v->reservation_size!=pin->reservation_size)return 0;
 }
 return 1;
}
#endif
