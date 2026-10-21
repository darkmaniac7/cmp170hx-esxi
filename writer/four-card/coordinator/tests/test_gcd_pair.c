/* Actual reservation C with synthetic EFI callbacks, no hardware I/O.
 * Four slots (01,02,47,48) across the two unowned r8 root regions; slot order
 * is NOT address order (47/48 sit below 01/02). Breaks caught: separate
 * baselines, wrong owner/span, silent callback errors, uncertain cleanup
 * retries, and cleanup crossing the first PCI attempt. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gcd_reservation.h"
#ifndef GR_PAIR_API
#error four-slot reservation API required
#endif
static struct gr_pair_state s;
static GR_DXE dxe;
static EFI_BOOT_SERVICES bs;
static GR_DESCRIPTOR map[64];
static UINTN count;
static unsigned allocations,frees,maps,pools,free_order[PT_COUNT];
static unsigned map_fail,pool_fail,read_fail,alloc_fail,free_fail;
static unsigned no_allocate,no_release,wrong_base,change,change_at,change_index,shape,bad_map_at,bad_map;
static const uint64_t length=UINT64_C(0x1002000000);
static unsigned owners,foreign_owner,pci_at_map;
struct region {uint64_t start,end;};
/* r8 unowned MMIO: S0D2 0x47e20200000-0x63cefffffff and PCI0 0x63d20200000-0x7fbefffffff. */
static const struct region regions[2]={{UINT64_C(0x47e20200000),UINT64_C(0x63cefffffff)},{UINT64_C(0x63d20200000),UINT64_C(0x7fbefffffff)}};
static int readable(void *ctx,uintptr_t p,size_t n){(void)ctx;return maps!=read_fail&&p&&n&&p<=UINTPTR_MAX-n;}
static int slot_at(uint64_t address){for(unsigned i=0;i<PT_COUNT;i++)if(address>=pair_targets[i].bar1&&address<=pair_targets[i].bar1+length-1)return (int)i;return -1;}
static void push(GR_DESCRIPTOR d){
 if(shape==1&&count&&map[count-1].ImageHandle==d.ImageHandle&&map[count-1].DeviceHandle==d.DeviceHandle&&
    map[count-1].BaseAddress+map[count-1].Length==d.BaseAddress){map[count-1].Length+=d.Length;return;}
 if(shape==2){map[count]=d;map[count++].Length=0x1000;d.BaseAddress+=0x1000;d.Length-=0x1000;}
 map[count++]=d;
}
static void rebuild(void){
 count=0;
 for(unsigned r=0;r<2;r++){
  uint64_t cuts[2+2*PT_COUNT];unsigned n=0;cuts[n++]=regions[r].start;
  for(unsigned i=0;i<PT_COUNT;i++){uint64_t b=pair_targets[i].bar1;if(b>=regions[r].start&&b+length-1<=regions[r].end){cuts[n++]=b;cuts[n++]=b+length;}}
  cuts[n++]=regions[r].end+1;
  for(unsigned i=1;i<n;i++)for(unsigned j=i;j&&cuts[j-1]>cuts[j];j--){uint64_t t=cuts[j];cuts[j]=cuts[j-1];cuts[j-1]=t;}
  for(unsigned i=0;i+1<n;i++){
   GR_DESCRIPTOR d={.BaseAddress=cuts[i],.Length=cuts[i+1]-cuts[i],.Capabilities=UINT64_C(0xc000000000026001),.Attributes=1,.GcdMemoryType=3};
   int slot=slot_at(cuts[i]);
   if(slot>=0&&(owners&(1U<<slot))){d.ImageHandle=(VOID *)1;d.DeviceHandle=(VOID *)(uintptr_t)(2+slot);}
   else if(foreign_owner&&slot<0){d.ImageHandle=(VOID *)8;d.DeviceHandle=(VOID *)9;}
   push(d);
  }
 }
}
static unsigned span_index(unsigned slot){for(unsigned i=0;i<count;i++)if(map[i].BaseAddress==pair_targets[slot].bar1)return i;assert(0);return 0;}
static EFI_STATUS EFIAPI getmap(UINTN *n,GR_DESCRIPTOR **out){
 maps++;if(maps==pci_at_map)s.pci_attempted=1;
 if(maps==map_fail)return EFI_DEVICE_ERROR;
 *n=count;*out=malloc(count*sizeof **out);assert(*out);memcpy(*out,map,count*sizeof **out);
 if(maps==bad_map_at){
  if(bad_map==1)*n=GR_MAX_DESCRIPTORS+1;
  if(bad_map==2)*n=0;
  if(bad_map==3){free(*out);*out=NULL;}
  if(bad_map==4)(*out)[0].Length=0;
  if(bad_map==5)(*out)[1]=(*out)[0];
  if(bad_map==6){GR_DESCRIPTOR t=(*out)[0];(*out)[0]=(*out)[1];(*out)[1]=t;}
  if(bad_map==8)(*out)[2].ImageHandle=(VOID *)9;
  if(bad_map==9)(*out)[0].Attributes^=2;
  if(bad_map==7){(*out)[count-1].BaseAddress=UINT64_MAX-1;(*out)[count-1].Length=3;}
 }
 return 0;
}
static EFI_STATUS EFIAPI pool(VOID *p){assert(p);pools++;free(p);return pools==pool_fail?EFI_OUT_OF_RESOURCES:0;}
static EFI_STATUS EFIAPI alloc(UINT32 policy,UINT32 type,UINTN align,UINT64 len,EFI_PHYSICAL_ADDRESS *base,EFI_HANDLE image,EFI_HANDLE device){
 unsigned i=allocations++;assert(i<PT_COUNT);
 assert(policy==2&&type==3&&align==36&&len==length&&*base==pair_targets[i].bar1&&image==(VOID *)1&&device==(VOID *)(uintptr_t)(2+i));
 if(wrong_base==i+1)*base+=0x1000;
 if(no_allocate!=i+1)owners|=1U<<i;
 rebuild();
 if(change_at==i+1){
  unsigned k=change>=6?span_index(i):change_index;assert(k<count&&(change<7||k));
  if(change==1)map[k].ImageHandle=(VOID *)9;
  if(change==2)map[k].DeviceHandle=(VOID *)9;
  if(change==3)map[k].Attributes^=2;
  if(change==4)map[k].Capabilities^=2;
  if(change==5)map[k].GcdMemoryType=2;
  if(change==6)map[k].DeviceHandle=(VOID *)9;
  /* One owned byte of the just-allocated span moves into the free neighbour.
   * (A boundary shift between two equally unowned descriptors is a legitimate
   * split and is accepted by design; shape 1/2 cover that above.) */
  if(change==7){map[k-1].Length++;map[k].BaseAddress++;map[k].Length--;}
 }
 return alloc_fail==i+1?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI release(EFI_PHYSICAL_ADDRESS base,UINT64 len){
 int i=slot_at(base);assert(i>=0&&base==pair_targets[i].bar1&&len==length&&frees<PT_COUNT);free_order[frees++]=(unsigned)i;
 if(no_release!=frees){owners&=~(1U<<i);rebuild();}
 return free_fail==frees?EFI_DEVICE_ERROR:0;
}
static void setup(void){
 memset(&s,0,sizeof s);memset(&dxe,0,sizeof dxe);memset(&bs,0,sizeof bs);
 dxe.Hdr.Signature=UINT64_C(0x565245535f455844);dxe.Hdr.HeaderSize=sizeof dxe;
 dxe.AllocateMemorySpace=alloc;dxe.FreeMemorySpace=release;dxe.GetMemorySpaceMap=getmap;bs.FreePool=pool;
 s.dxe=&dxe;s.bs=&bs;s.image=(VOID *)1;s.readable=readable;
 for(unsigned i=0;i<PT_COUNT;i++)s.slot[i].device=(VOID *)(uintptr_t)(2+i);
 allocations=frees=maps=pools=map_fail=pool_fail=alloc_fail=free_fail=0;
 read_fail=~0U;no_allocate=no_release=wrong_base=change=change_at=change_index=shape=owners=foreign_owner=pci_at_map=bad_map_at=bad_map=0;
 rebuild();
}
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);abort();}}while(0)
static void terminal(void){
 enum gr_pair_failure first=s.first_failure;EFI_STATUS status=s.first_status;unsigned slot=s.first_slot;
 unsigned a=allocations,f=frees,m=maps,p=pools;
 CHECK(s.status==GR_UNCERTAIN);CHECK(!gr_pair_check_owned(&s));
 CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN);CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN);
 CHECK(allocations==a&&frees==f&&maps==m&&pools==p);
 CHECK(s.first_failure==first&&s.first_status==status&&s.first_slot==slot);
}
/* Release observations: 1 ownership recheck, then (pre-free recheck for every
 * slot but the first freed) + post-free recheck: 1 + (PT_COUNT-1) + PT_COUNT. */
#define RELEASE_MAPS (2*PT_COUNT)
#define RESERVE_MAPS (PT_COUNT+1)
static unsigned frees_before_release_map(unsigned m){return m/2;}
int main(void){
 CHECK(PT_COUNT==4&&PT_ALL_MASK==15);
 CHECK(pair_targets[2].bar1<pair_targets[0].bar1&&pair_targets[3].bar1<pair_targets[0].bar1); /* slot order != address order */
 for(unsigned m=0;m<=PT_ALL_MASK;m++)CHECK(pair_prefix_mask(m)==(m==0||m==1||m==3||m==7||m==15));
 CHECK(!pair_prefix_mask(16)&&!pair_prefix_mask(31));
 setup();CHECK(count==10);
 CHECK(gr_pair_reserve(&s)==GR_OWNED);
 CHECK(allocations==PT_COUNT);
 CHECK(maps==RESERVE_MAPS&&pools==RESERVE_MAPS&&s.owned_mask==PT_ALL_MASK&&s.count==10);
 for(unsigned i=0;i<PT_COUNT;i++){
  CHECK(s.slot[i].allocate_verified&&s.slot[i].base==pair_targets[i].bar1&&s.slot[i].length==length&&s.slot[i].returned_base==s.slot[i].base);
 }
 CHECK(gr_pair_check_owned(&s));CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED);
 CHECK(frees==PT_COUNT&&s.owned_mask==0&&maps==RESERVE_MAPS+1+RELEASE_MAPS); /* +1: the explicit ownership check above */
 for(unsigned i=0;i<PT_COUNT;i++)CHECK(free_order[i]==PT_COUNT-1-i&&s.slot[i].free_verified);
 CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED&&frees==PT_COUNT);
 CHECK(gr_pair_reserve(&s)==GR_RELEASED&&allocations==PT_COUNT);
 for(unsigned sh=1;sh<=2;sh++){
  setup();shape=sh;rebuild();CHECK(gr_pair_reserve(&s)==GR_OWNED);
  shape=3-sh;rebuild();CHECK(gr_pair_check_owned(&s));CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED);
 }
 setup();foreign_owner=1;rebuild();CHECK(gr_pair_reserve(&s)==GR_OWNED);
 CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED);
 /* Every target span must be free before the first allocation. */
 for(unsigned i=0;i<PT_COUNT;i++)for(unsigned fault=0;fault<4;fault++){
  setup();unsigned k=span_index(i);
  if(fault==0)map[k].ImageHandle=(VOID *)9;
  if(fault==1)map[k].GcdMemoryType=2;
  if(fault==2)map[k].BaseAddress++;
  if(fault==3)map[k].Length--;
  CHECK(gr_pair_reserve(&s)==GR_REFUSED&&allocations==0);
 }
 /* Any unrelated or related map change after allocation i is terminal uncertainty. */
 for(unsigned i=1;i<=PT_COUNT;i++){
  for(unsigned c=1;c<=5;c++)for(unsigned k=0;k<10;k++){
   setup();change=c;change_at=i;change_index=k;
   CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);terminal();
  }
  for(unsigned c=6;c<=7;c++){setup();change=c;change_at=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);terminal();}
  for(unsigned side=0;side<2;side++){
   setup();alloc_fail=i;if(!side)no_allocate=i;
   CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);
   CHECK(s.first_failure==GR_PAIR_ALLOCATE&&s.first_status==EFI_DEVICE_ERROR&&s.first_slot==i-1);
   CHECK(!s.slot[i-1].allocate_verified);
   for(unsigned j=0;j+1<i;j++)CHECK(s.slot[j].allocate_verified); /* earlier slots stay reserved */
   terminal();
  }
  setup();wrong_base=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);
  CHECK(s.first_failure==GR_PAIR_RETURNED_BASE&&s.first_slot==i-1);terminal();
  setup();no_allocate=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);terminal();
 }
 for(unsigned i=1;i<=RESERVE_MAPS;i++){
  setup();map_fail=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i-1);
  CHECK(s.first_failure==GR_PAIR_MAP&&s.first_status==EFI_DEVICE_ERROR&&s.first_slot==PT_COUNT);terminal();
  setup();pool_fail=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i-1);
  CHECK(s.first_failure==GR_PAIR_POOL&&s.first_status==EFI_OUT_OF_RESOURCES);terminal();
  setup();read_fail=i;CHECK(gr_pair_reserve(&s)==(i==1?GR_REFUSED:GR_UNCERTAIN)&&allocations==i-1);
  if(i>1)terminal();
  for(unsigned fault=1;fault<=7;fault++){
   setup();bad_map_at=i;bad_map=fault;
   CHECK(gr_pair_reserve(&s)==(i==1?GR_REFUSED:GR_UNCERTAIN)&&allocations==i-1);
   if(i>1)terminal();
  }
 }
 /* Preserve the first validation diagnostic even if freeing its pool fails. */
 setup();bad_map_at=2;bad_map=4;pool_fail=2;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN);
 CHECK(s.first_failure==GR_PAIR_CONTENT&&s.first_status==0&&s.pool_status==EFI_OUT_OF_RESOURCES);terminal();
 /* All map/pool failures in pre-free and post-free observations stop globally. */
 for(unsigned m=1;m<=RELEASE_MAPS;m++){
  unsigned i=RESERVE_MAPS+m;
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);map_fail=i;
  CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN);CHECK(frees==frees_before_release_map(m));terminal();
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);pool_fail=i;
  CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN);CHECK(frees==frees_before_release_map(m));terminal();
  for(unsigned fault=4;fault<=9;fault++){
   setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);bad_map_at=i;bad_map=fault;
   CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN&&frees==frees_before_release_map(m));terminal();
  }
  /* Recheck the global boundary after observation, immediately before each free. */
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);pci_at_map=i;
  gr_pair_release_before_mutation(&s);CHECK(frees==frees_before_release_map(m));
  unsigned f=frees;gr_pair_release_before_mutation(&s);CHECK(frees==f);
 }
 for(unsigned i=1;i<=PT_COUNT;i++)for(unsigned side=0;side<2;side++){
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);free_fail=i;if(!side)no_release=i;
  CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN&&frees==i);
  CHECK(s.first_failure==GR_PAIR_FREE&&s.first_status==EFI_DEVICE_ERROR&&s.first_slot==PT_COUNT-i);terminal();
 }
 setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);no_release=1;
 CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN&&frees==1);terminal();
 setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);map[1].Attributes^=2;
 CHECK(!gr_pair_check_owned(&s));terminal();
 /* Consumer boundary exercise: all four reservations before each of the 52
  * simulated allowed stores; release cannot cross any global PCI attempt. */
 for(unsigned stop=0;stop<13*PT_COUNT;stop++){
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);
  for(unsigned store=0;store<=stop;store++){
   CHECK(allocations==PT_COUNT);
   for(unsigned i=0;i<PT_COUNT;i++)CHECK(s.slot[i].allocate_verified);
   CHECK(gr_pair_check_owned(&s));s.pci_attempted=1;
  }
  unsigned before=maps;CHECK(gr_pair_release_before_mutation(&s)==GR_OWNED&&frees==0&&maps==before);
 }
 for(unsigned fault=0;fault<9;fault++){
  setup();
  if(fault==0)s.slot[1].device=s.slot[0].device;
  if(fault==1)s.slot[3].device=s.image;
  if(fault==2)s.slot[2].device=NULL;
  if(fault==3)bs.FreePool=NULL;
  if(fault==4)dxe.AllocateMemorySpace=NULL;
  if(fault==5)dxe.FreeMemorySpace=NULL;
  if(fault==6)dxe.GetMemorySpaceMap=NULL;
  if(fault==7)s.slot[3].device=s.slot[2].device;
  if(fault==8)s.slot[0].device=NULL;
  CHECK(gr_pair_reserve(&s)==GR_REFUSED&&maps==0&&allocations==0);
 }
 CHECK(gr_pair_reserve(NULL)==GR_REFUSED);CHECK(!gr_pair_check_owned(NULL));
 CHECK(gr_pair_release_before_mutation(NULL)==GR_REFUSED);
 printf("%u shared GCD four-slot ABI/fault checks PASS\n",checks);return 0;
}
