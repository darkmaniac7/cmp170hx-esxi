#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gcd_reservation.h"
static struct gr_state s;
static GR_DXE dxe;
static EFI_BOOT_SERVICES bs;
static GR_DESCRIPTOR map[8];
static UINTN count;
static unsigned allocations,frees,maps,pools;
static unsigned map_fail,pool_fail,read_fail;
static EFI_STATUS alloc_status,release_status;
static unsigned no_allocate,no_release,wrong_base,post_change;
static GR_DESCRIPTOR original;
static int readable(void *ctx,uintptr_t p,size_t n){(void)ctx;return !read_fail&&p&&n&&p<=UINTPTR_MAX-n;}
static EFI_STATUS EFIAPI getmap(UINTN *n,GR_DESCRIPTOR **out){
 maps++;if(maps==map_fail)return EFI_DEVICE_ERROR;
 *n=count;*out=malloc(count*sizeof **out);assert(*out);memcpy(*out,map,count*sizeof **out);return 0;
}
static EFI_STATUS EFIAPI pool(VOID *p){pools++;free(p);return pools==pool_fail?EFI_DEVICE_ERROR:0;}
static EFI_STATUS EFIAPI alloc(UINT32 policy,UINT32 type,UINTN align,UINT64 length,EFI_PHYSICAL_ADDRESS *base,EFI_HANDLE image,EFI_HANDLE device){
 assert(policy==2&&type==3&&align==36&&length==GR_LENGTH&&*base==GR_BASE&&image==(VOID *)1&&device==(VOID *)2);
 allocations++;
 if(wrong_base)*base+=0x1000000000;
 if(no_allocate)return alloc_status;
 original=map[0];count=3;
 map[0]=original;map[0].Length=GR_BASE-original.BaseAddress;
 map[1]=original;map[1].BaseAddress=GR_BASE;map[1].Length=GR_LENGTH;map[1].ImageHandle=image;map[1].DeviceHandle=device;
 map[2]=original;map[2].BaseAddress=GR_LIMIT+1;map[2].Length=original.BaseAddress+original.Length-map[2].BaseAddress;
 if(post_change==1)map[0].ImageHandle=(VOID *)9;
 if(post_change==2)map[1].DeviceHandle=(VOID *)9;
 if(post_change==3)map[1].Length--;
 if(post_change==4)map[1].Capabilities^=0x10;
 if(post_change==5)map[2].Attributes^=0x10;
 if(post_change==6)map[1].GcdMemoryType=2;
 if(post_change==7){map[1].Length++;map[2].BaseAddress++;map[2].Length--;}
 return alloc_status;
}
static EFI_STATUS EFIAPI release(EFI_PHYSICAL_ADDRESS base,UINT64 length){
 assert(base==GR_BASE&&length==GR_LENGTH);frees++;
 if(!no_release){map[0]=original;count=1;}
 return release_status;
}
static void setup(void){
 memset(&s,0,sizeof s);memset(&dxe,0,sizeof dxe);memset(&bs,0,sizeof bs);
 dxe.Hdr.Signature=UINT64_C(0x565245535f455844);dxe.Hdr.HeaderSize=sizeof dxe;
 dxe.AllocateMemorySpace=alloc;dxe.FreeMemorySpace=release;dxe.GetMemorySpaceMap=getmap;bs.FreePool=pool;
 s.dxe=&dxe;s.bs=&bs;s.image=(VOID *)1;s.device=(VOID *)2;s.readable=readable;
 count=1;map[0]=(GR_DESCRIPTOR){.BaseAddress=UINT64_C(0x63d20200000),.Length=UINT64_C(0x1becfe00000),.Capabilities=UINT64_C(0xc000000000026001),.Attributes=1,.GcdMemoryType=3};
 allocations=frees=maps=pools=map_fail=pool_fail=read_fail=0;
 alloc_status=release_status=0;no_allocate=no_release=wrong_base=post_change=0;
}
int main(void){
 unsigned checks=0;
#define CHECK(x) do{checks++;assert(x);}while(0)
 setup();CHECK(gr_reserve(&s)==GR_OWNED);CHECK(allocations==1&&maps==2&&pools==2);
 CHECK(gr_check_owned(&s));CHECK(gr_release(&s)==GR_RELEASED&&frees==1);
 CHECK(gr_release(&s)==GR_RELEASED&&frees==1);CHECK(gr_reserve(&s)==GR_RELEASED&&allocations==1);
 setup();CHECK(gr_reserve(&s)==GR_OWNED);s.pci_attempted=1;
 CHECK(gr_release(&s)==GR_OWNED&&frees==0);CHECK(gr_reserve(&s)==GR_OWNED&&allocations==1);
 for(unsigned change=1;change<=7;change++){
  setup();post_change=change;CHECK(gr_reserve(&s)==GR_UNCERTAIN&&allocations==1);
  CHECK(!gr_check_owned(&s));CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==0);
 }
 setup();alloc_status=EFI_OUT_OF_RESOURCES;no_allocate=1;
 CHECK(gr_reserve(&s)==GR_REFUSED&&allocations==1);CHECK(gr_reserve(&s)==GR_REFUSED&&allocations==1);
 setup();alloc_status=EFI_DEVICE_ERROR;CHECK(gr_reserve(&s)==GR_UNCERTAIN);
 CHECK(gr_release(&s)==GR_RELEASED&&frees==1);
 setup();wrong_base=1;CHECK(gr_reserve(&s)==GR_UNCERTAIN);CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==0);
 setup();CHECK(gr_reserve(&s)==GR_OWNED);release_status=EFI_DEVICE_ERROR;
 CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==1);CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==1);
 setup();CHECK(gr_reserve(&s)==GR_OWNED);no_release=1;
 CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==1);
 for(unsigned i=1;i<=2;i++){
  setup();map_fail=i;CHECK(gr_reserve(&s)==(i==1?GR_REFUSED:GR_UNCERTAIN));CHECK(allocations==(i==1?0U:1U));
  setup();pool_fail=i;CHECK(gr_reserve(&s)==(i==1?GR_REFUSED:GR_UNCERTAIN));CHECK(allocations==(i==1?0U:1U));
 }
 setup();CHECK(gr_reserve(&s)==GR_OWNED);map_fail=3;CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==0);
 setup();CHECK(gr_reserve(&s)==GR_OWNED);map_fail=4;CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==1);
 setup();CHECK(gr_reserve(&s)==GR_OWNED);pool_fail=4;CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==1);
 for(unsigned which=0;which<12;which++){
  setup();
  if(which==0)map[0].ImageHandle=(VOID *)5;
  if(which==1)map[0].DeviceHandle=(VOID *)5;
  if(which==2)map[0].GcdMemoryType=2;
  if(which==3)map[0].Length=0;
  if(which==4){map[0].BaseAddress=UINT64_MAX-1;map[0].Length=3;}
  if(which==5){map[0].BaseAddress=GR_BASE+1;map[0].Length=GR_LENGTH;}
  if(which==6)map[0].Length=GR_LIMIT-map[0].BaseAddress;
  if(which==7){count=2;map[1]=map[0];}
  if(which==8){count=2;map[1]=map[0];map[0].BaseAddress+=map[0].Length;}
  if(which==9)dxe.Hdr.Signature=0;
  if(which==10)dxe.Hdr.HeaderSize=sizeof dxe-1;
  if(which==11)read_fail=1;
  CHECK(gr_reserve(&s)==GR_REFUSED&&allocations==0);
 }
 setup();dxe.AllocateMemorySpace=NULL;CHECK(gr_reserve(&s)==GR_REFUSED&&maps==0);
 setup();dxe.FreeMemorySpace=NULL;CHECK(gr_reserve(&s)==GR_REFUSED&&maps==0);
 setup();dxe.GetMemorySpaceMap=NULL;CHECK(gr_reserve(&s)==GR_REFUSED&&maps==0);
 setup();bs.FreePool=NULL;CHECK(gr_reserve(&s)==GR_REFUSED&&maps==0);
 setup();s.device=s.image;CHECK(gr_reserve(&s)==GR_REFUSED&&maps==0);
 setup();CHECK(gr_reserve(&s)==GR_OWNED);map[1].ImageHandle=(VOID *)9;
 CHECK(!gr_check_owned(&s)&&s.status==GR_UNCERTAIN);CHECK(gr_release(&s)==GR_UNCERTAIN&&frees==0);
 printf("%u GCD reservation ABI/fault checks PASS\n",checks);return 0;
}
