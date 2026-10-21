/* Actual reservation C with synthetic EFI callbacks, no hardware I/O.
 * Breaks caught: separate baselines, wrong owner/span, silent callback errors,
 * uncertain cleanup retries, and cleanup crossing the first PCI attempt. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gcd_reservation.h"
#ifdef GR_PAIR_API
static struct gr_pair_state s;
#else
/* RED compatibility: the predecessor can reserve only its first span. */
static struct gr_state s;
#endif
static GR_DXE dxe;
static EFI_BOOT_SERVICES bs;
static GR_DESCRIPTOR map[32];
static UINTN count;
static unsigned allocations,frees,maps,pools,free_order[2];
static unsigned map_fail,pool_fail,read_fail,alloc_fail,free_fail;
static unsigned no_allocate,no_release,wrong_base,change,change_at,shape,bad_map_at,bad_map;
static const uint64_t bases[2]={UINT64_C(0x64000000000),UINT64_C(0x66000000000)};
static const uint64_t length=UINT64_C(0x1002000000);
static unsigned owners,foreign_owner,pci_at_map;
static GR_DESCRIPTOR original;
static int readable(void *ctx,uintptr_t p,size_t n){(void)ctx;return maps!=read_fail&&p&&n&&p<=UINTPTR_MAX-n;}
static void rebuild(void){
 uint64_t ends[]={original.BaseAddress,bases[0],bases[0]+length,bases[1],bases[1]+length,original.BaseAddress+original.Length};
 count=0;
 for(unsigned i=0;i<5;i++){
  GR_DESCRIPTOR d=original;d.BaseAddress=ends[i];d.Length=ends[i+1]-ends[i];
  if(i==1&&(owners&1)){d.ImageHandle=(VOID *)1;d.DeviceHandle=(VOID *)2;}
  if(i==3&&(owners&2)){d.ImageHandle=(VOID *)1;d.DeviceHandle=(VOID *)3;}
  if(foreign_owner&&(i==0||i==2)){d.ImageHandle=(VOID *)8;d.DeviceHandle=(VOID *)9;}
  if(shape==1&&count&&map[count-1].ImageHandle==d.ImageHandle&&map[count-1].DeviceHandle==d.DeviceHandle){map[count-1].Length+=d.Length;continue;}
  if(shape==2){map[count]=d;map[count++].Length=0x1000;d.BaseAddress+=0x1000;d.Length-=0x1000;}
  map[count++]=d;
 }
}
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
 unsigned i=allocations++;assert(i<2);
 assert(policy==2&&type==3&&align==36&&len==length&&*base==bases[i]&&image==(VOID *)1&&device==(VOID *)(uintptr_t)(2+i));
 if(wrong_base==i+1)*base+=0x1000;
 if(no_allocate!=i+1)owners|=1U<<i;
 rebuild();
 if(change_at==i+1){
  unsigned k=change==8?3:change==9?2:change==10?4:change==11?1:0;
  if(change==1||change>=8)map[k].ImageHandle=(VOID *)9;
  if(change==2)map[k].DeviceHandle=(VOID *)9;
  if(change==3)map[k].Attributes^=2;
  if(change==4)map[k].Capabilities^=2;
  if(change==5)map[k].GcdMemoryType=2;
  if(change==6)map[1+2*i].DeviceHandle=(VOID *)9;
  if(change==7){map[1].Length++;map[2].BaseAddress++;map[2].Length--;}
 }
 return alloc_fail==i+1?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI release(EFI_PHYSICAL_ADDRESS base,UINT64 len){
 unsigned i=base==bases[1]?1:0;assert(base==bases[i]&&len==length&&frees<2);free_order[frees++]=i;
 if(no_release!=frees){owners&=~(1U<<i);rebuild();}
 return free_fail==frees?EFI_DEVICE_ERROR:0;
}
static void setup(void){
 memset(&s,0,sizeof s);memset(&dxe,0,sizeof dxe);memset(&bs,0,sizeof bs);
 dxe.Hdr.Signature=UINT64_C(0x565245535f455844);dxe.Hdr.HeaderSize=sizeof dxe;
 dxe.AllocateMemorySpace=alloc;dxe.FreeMemorySpace=release;dxe.GetMemorySpaceMap=getmap;bs.FreePool=pool;
 s.dxe=&dxe;s.bs=&bs;s.image=(VOID *)1;s.readable=readable;
#ifdef GR_PAIR_API
 s.slot[0].device=(VOID *)2;s.slot[1].device=(VOID *)3;
#else
 s.device=(VOID *)2;
#endif
 original=(GR_DESCRIPTOR){.BaseAddress=UINT64_C(0x63d20200000),.Length=UINT64_C(0x1becfe00000),.Capabilities=UINT64_C(0xc000000000026001),.Attributes=1,.GcdMemoryType=3};
 allocations=frees=maps=pools=map_fail=pool_fail=alloc_fail=free_fail=0;
 read_fail=~0U;no_allocate=no_release=wrong_base=change=change_at=shape=owners=foreign_owner=pci_at_map=bad_map_at=bad_map=0;
 rebuild();
}
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);abort();}}while(0)
#ifdef GR_PAIR_API
static void terminal(void){
 enum gr_pair_failure first=s.first_failure;EFI_STATUS status=s.first_status;unsigned slot=s.first_slot;
 unsigned a=allocations,f=frees,m=maps,p=pools;
 CHECK(s.status==GR_UNCERTAIN);CHECK(!gr_pair_check_owned(&s));
 CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN);CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN);
 CHECK(allocations==a&&frees==f&&maps==m&&pools==p);
 CHECK(s.first_failure==first&&s.first_status==status&&s.first_slot==slot);
}
#endif
int main(void){
 setup();
#ifdef GR_PAIR_API
 CHECK(gr_pair_reserve(&s)==GR_OWNED);
#else
 CHECK(gr_reserve(&s)==GR_OWNED);
#endif
 CHECK(allocations==2); /* Fails on unchanged predecessor's single reservation. */
#ifdef GR_PAIR_API
 CHECK(maps==3&&pools==3&&s.owned_mask==3&&s.count==5);
 CHECK(s.slot[0].allocate_verified&&s.slot[1].allocate_verified);
 CHECK(s.slot[0].base==bases[0]&&s.slot[1].base==bases[1]);
 CHECK(s.slot[0].length==length&&s.slot[1].length==length);
 CHECK(gr_pair_check_owned(&s));CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED);
 CHECK(frees==2&&free_order[0]==1&&free_order[1]==0&&s.owned_mask==0);
 CHECK(s.slot[0].free_verified&&s.slot[1].free_verified);
 CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED&&frees==2);
 CHECK(gr_pair_reserve(&s)==GR_RELEASED&&allocations==2);
 for(unsigned sh=1;sh<=2;sh++){
  setup();shape=sh;rebuild();CHECK(gr_pair_reserve(&s)==GR_OWNED);
  shape=3-sh;rebuild();CHECK(gr_pair_check_owned(&s));CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED);
 }
 setup();foreign_owner=1;rebuild();CHECK(gr_pair_reserve(&s)==GR_OWNED);
 CHECK(gr_pair_release_before_mutation(&s)==GR_RELEASED);
 /* Both target spans must be free before either allocation. */
 for(unsigned i=0;i<2;i++)for(unsigned fault=0;fault<4;fault++){
  setup();unsigned k=1+2*i;
  if(fault==0)map[k].ImageHandle=(VOID *)9;
  if(fault==1)map[k].GcdMemoryType=2;
  if(fault==2)map[k].BaseAddress++;
  if(fault==3)map[k].Length--;
  CHECK(gr_pair_reserve(&s)==GR_REFUSED&&allocations==0);
 }
 for(unsigned i=1;i<=2;i++){
  for(unsigned c=1;c<=11;c++){
   setup();change=c;change_at=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);terminal();
  }
  for(unsigned side=0;side<2;side++){
   setup();alloc_fail=i;if(!side)no_allocate=i;
   CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);
   CHECK(s.first_failure==GR_PAIR_ALLOCATE&&s.first_status==EFI_DEVICE_ERROR&&s.first_slot==i-1);
   CHECK(!s.slot[i-1].allocate_verified);terminal();
  }
  setup();wrong_base=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);terminal();
  setup();no_allocate=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i);terminal();
 }
 for(unsigned i=1;i<=3;i++){
  setup();map_fail=i;CHECK(gr_pair_reserve(&s)==GR_UNCERTAIN&&allocations==i-1);
  CHECK(s.first_failure==GR_PAIR_MAP&&s.first_status==EFI_DEVICE_ERROR);terminal();
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
 for(unsigned i=4;i<=7;i++){
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);map_fail=i;
  CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN);CHECK(frees==(i-4+1)/2);terminal();
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);pool_fail=i;
  CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN);CHECK(frees==(i-4+1)/2);terminal();
 }
 for(unsigned i=4;i<=7;i++)for(unsigned fault=4;fault<=9;fault++){
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);bad_map_at=i;bad_map=fault;
  CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN&&frees==(i-4+1)/2);terminal();
 }
 /* Recheck the global boundary after observation, immediately before each free. */
 for(unsigned i=4;i<=6;i+=2){
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);pci_at_map=i;
  gr_pair_release_before_mutation(&s);CHECK(frees==(i==4?0U:1U));
  unsigned f=frees;gr_pair_release_before_mutation(&s);CHECK(frees==f);
 }
 for(unsigned i=1;i<=2;i++)for(unsigned side=0;side<2;side++){
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);free_fail=i;if(!side)no_release=i;
  CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN&&frees==i);
  CHECK(s.first_failure==GR_PAIR_FREE&&s.first_status==EFI_DEVICE_ERROR&&s.first_slot==2-i);terminal();
 }
 setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);no_release=1;
 CHECK(gr_pair_release_before_mutation(&s)==GR_UNCERTAIN&&frees==1);terminal();
 setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);map[2].Attributes^=2;
 CHECK(!gr_pair_check_owned(&s));terminal();
 /* Consumer boundary exercise: both reservations before each of 26 simulated
  * allowed stores; release cannot cross any global PCI attempt, even card01. */
 for(unsigned stop=0;stop<26;stop++){
  setup();CHECK(gr_pair_reserve(&s)==GR_OWNED);
  for(unsigned store=0;store<=stop;store++){
   CHECK(allocations==2&&s.slot[0].allocate_verified&&s.slot[1].allocate_verified);
   CHECK(gr_pair_check_owned(&s));s.pci_attempted=1;
  }
  unsigned before=maps;CHECK(gr_pair_release_before_mutation(&s)==GR_OWNED&&frees==0&&maps==before);
 }
 for(unsigned fault=0;fault<7;fault++){
  setup();
  if(fault==0)s.slot[1].device=s.slot[0].device;
  if(fault==1)s.slot[1].device=s.image;
  if(fault==2)s.slot[0].device=NULL;
  if(fault==3)bs.FreePool=NULL;
  if(fault==4)dxe.AllocateMemorySpace=NULL;
  if(fault==5)dxe.FreeMemorySpace=NULL;
  if(fault==6)dxe.GetMemorySpaceMap=NULL;
  CHECK(gr_pair_reserve(&s)==GR_REFUSED&&maps==0&&allocations==0);
 }
 CHECK(gr_pair_reserve(NULL)==GR_REFUSED);CHECK(!gr_pair_check_owned(NULL));
 CHECK(gr_pair_release_before_mutation(NULL)==GR_REFUSED);
#endif
 printf("%u shared GCD pair ABI/fault checks PASS\n",checks);return 0;
}
