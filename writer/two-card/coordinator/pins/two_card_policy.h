#ifndef CMP_TWO_CARD_POLICY_R1_H
#define CMP_TWO_CARD_POLICY_R1_H
#include <stdint.h>
#include <stddef.h>
/* Fixed cold-layout policy, not an allocator or hardware authorization. */
/* HOST-SPECIFIC: the endpoint/port/bus and BAR placement addresses in the tables
 * below are this host's measured values and must be regenerated for another host;
 * the writer refuses on any mismatch by design. */
struct pair_target {
 uint32_t endpoint,port,bus;
 uint64_t bar0,bar1,bar3,bar1_size,bar3_size,reservation_size;
};
static const struct pair_target pair_targets[2]={
 {0x0100,0x0009,1,UINT64_C(0xf3000000),UINT64_C(0x64000000000),UINT64_C(0x65000000000),
  UINT64_C(0x1000000000),UINT64_C(0x02000000),UINT64_C(0x1002000000)},
 {0x0200,0x000b,2,UINT64_C(0xf2000000),UINT64_C(0x66000000000),UINT64_C(0x67000000000),
  UINT64_C(0x1000000000),UINT64_C(0x02000000),UINT64_C(0x1002000000)}
};
/* Retained cold CONFIG32 values for this host. Neither the prior enlarged layout
 * nor a partial layout resumes. */
static const uint64_t pair_cold_bases[2][3]={
 {UINT64_C(0xf3000000),UINT64_C(0x7fbf8000000),UINT64_C(0x7fbfc000000)},
 {UINT64_C(0xf2000000),UINT64_C(0x7fbf0000000),UINT64_C(0x7fbf4000000)}
};
static const uint32_t pair_cold_windows[2][4]={
 {0xf3f0f300,0xfdf1f801,0x7fb,0x7fb},
 {0xf2f0f200,0xf5f1f001,0x7fb,0x7fb}
};
static int pair_policy_matches(const struct pair_target *p,size_t count)
{
 if(!p || count!=2)return 0;
 for(unsigned i=0;i<2;i++){
  const struct pair_target *v=&p[i],*pin=&pair_targets[i];
  if(v->endpoint!=pin->endpoint || v->port!=pin->port || v->bus!=pin->bus ||
     v->bar0!=pin->bar0 || v->bar1!=pin->bar1 || v->bar3!=pin->bar3 ||
     v->bar1_size!=pin->bar1_size || v->bar3_size!=pin->bar3_size ||
     v->reservation_size!=pin->reservation_size)return 0;
 }
 return 1;
}
#endif
