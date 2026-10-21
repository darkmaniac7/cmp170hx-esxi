#ifndef CMP_GCD_RESERVATION_H
#define CMP_GCD_RESERVATION_H
#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {
 EFI_PHYSICAL_ADDRESS BaseAddress;
 UINT64 Length,Capabilities,Attributes;
 UINT32 GcdMemoryType;
 EFI_HANDLE ImageHandle,DeviceHandle;
} GR_DESCRIPTOR;
typedef EFI_STATUS(EFIAPI *GR_ALLOCATE)(UINT32,UINT32,UINTN,UINT64,EFI_PHYSICAL_ADDRESS *,EFI_HANDLE,EFI_HANDLE);
typedef EFI_STATUS(EFIAPI *GR_FREE)(EFI_PHYSICAL_ADDRESS,UINT64);
typedef EFI_STATUS(EFIAPI *GR_MAP)(UINTN *,GR_DESCRIPTOR **);
typedef struct {
 EFI_TABLE_HEADER Hdr;
 VOID *AddMemorySpace;
 GR_ALLOCATE AllocateMemorySpace;
 GR_FREE FreeMemorySpace;
 VOID *RemoveMemorySpace,*GetMemorySpaceDescriptor,*SetMemorySpaceAttributes;
 GR_MAP GetMemorySpaceMap;
} GR_DXE;
_Static_assert(sizeof(void *)==8,"x64 only");
_Static_assert(sizeof(GR_DESCRIPTOR)==56,"PI GCD descriptor ABI");
_Static_assert(offsetof(GR_DESCRIPTOR,GcdMemoryType)==32,"PI GCD type ABI");
_Static_assert(offsetof(GR_DESCRIPTOR,ImageHandle)==40,"PI GCD image ABI");
_Static_assert(offsetof(GR_DESCRIPTOR,DeviceHandle)==48,"PI GCD device ABI");
_Static_assert(offsetof(GR_DXE,AllocateMemorySpace)==32,"PI allocate ABI");
_Static_assert(offsetof(GR_DXE,FreeMemorySpace)==40,"PI free ABI");
_Static_assert(offsetof(GR_DXE,GetMemorySpaceMap)==72,"PI map ABI");
#define GR_BASE UINT64_C(0x64000000000)
#define GR_LENGTH UINT64_C(0x1002000000)
#define GR_LIMIT (GR_BASE+GR_LENGTH-1)
#define GR_MAX_DESCRIPTORS 2048U
enum gr_status {GR_NEW,GR_REFUSED,GR_OWNED,GR_UNCERTAIN,GR_RELEASED};
struct gr_state {
 GR_DXE *dxe;EFI_BOOT_SERVICES *bs;EFI_HANDLE image,device;
 int (*readable)(void *,uintptr_t,size_t);void *read_context;
 enum gr_status status;unsigned allocate_attempted,free_attempted,pci_attempted;
 UINTN count;GR_DESCRIPTOR before[GR_MAX_DESCRIPTORS];
 EFI_STATUS allocate_status,free_status,map_status,pool_status;
 EFI_PHYSICAL_ADDRESS returned_base;
};
static int gr_readable(struct gr_state *s,const void *p,size_t n){
 return s&&s->readable&&p&&n&&(uintptr_t)p<=UINTPTR_MAX-n&&s->readable(s->read_context,(uintptr_t)p,n);
}
static int gr_map_valid(const GR_DESCRIPTOR *m,UINTN n){
 if(!m||!n||n>GR_MAX_DESCRIPTORS)return 0;
 for(UINTN i=0;i<n;i++){
  if(!m[i].Length||m[i].BaseAddress>UINT64_MAX-(m[i].Length-1)||
     m[i].GcdMemoryType>4||(!m[i].ImageHandle&&m[i].DeviceHandle))return 0;
  if(i&&m[i-1].BaseAddress+m[i-1].Length-1>=m[i].BaseAddress)return 0;
 }
 return 1;
}
static int gr_span_free(const GR_DESCRIPTOR *m,UINTN n){
 uint64_t cursor=GR_BASE;
 for(UINTN i=0;i<n&&cursor<=GR_LIMIT;i++){
  uint64_t end=m[i].BaseAddress+m[i].Length-1;
  if(end<cursor)continue;
  if(m[i].BaseAddress>cursor||m[i].GcdMemoryType!=3||m[i].ImageHandle||m[i].DeviceHandle)return 0;
  if(end>=GR_LIMIT)return 1;
  cursor=end+1;
 }
 return 0;
}
/* Compare the full map as address intervals, allowing only descriptor splits
 * and the named owner change inside the reservation. No other map change is
 * hidden by a successful target lookup. Attributes/capabilities never change. */
static int gr_map_same(struct gr_state *s,const GR_DESCRIPTOR *now,UINTN count,int owned){
 UINTN a=0,b=0;uint64_t pa=s->before[0].BaseAddress,pb=now[0].BaseAddress;
 while(a<s->count&&b<count){
  const GR_DESCRIPTOR *x=&s->before[a],*y=&now[b];
  if(pa!=pb||x->GcdMemoryType!=y->GcdMemoryType||x->Capabilities!=y->Capabilities||x->Attributes!=y->Attributes)return 0;
  uint64_t ae=x->BaseAddress+x->Length-1,be=y->BaseAddress+y->Length-1,end=ae<be?ae:be;
  int inside=pa>=GR_BASE&&pa<=GR_LIMIT;
  if(pa<GR_BASE&&end>=GR_BASE)end=GR_BASE-1;
  if(inside&&end>GR_LIMIT)end=GR_LIMIT;
  EFI_HANDLE image=inside&&owned?s->image:x->ImageHandle;
  EFI_HANDLE device=inside&&owned?s->device:x->DeviceHandle;
  if(y->ImageHandle!=image||y->DeviceHandle!=device)return 0;
  if(end==ae){a++;if(a<s->count)pa=s->before[a].BaseAddress;}else pa=end+1;
  if(end==be){b++;if(b<count)pb=now[b].BaseAddress;}else pb=end+1;
 }
 return a==s->count&&b==count;
}
/* Firmware calls are synchronous. Count/byte bounds do not preempt a hung call.
 * The caller must supply a fresh typed-RAM range validator for protocol outputs. */
static int gr_observe(struct gr_state *s,int mode){
 UINTN n=0;GR_DESCRIPTOR *m=NULL;
 s->map_status=uefi_call_wrapper(s->dxe->GetMemorySpaceMap,2,&n,&m);
 if(s->map_status)return 0;
 int valid=n&&n<=GR_MAX_DESCRIPTORS&&gr_readable(s,m,n*sizeof *m)&&gr_map_valid(m,n);
 if(valid&&mode<0){
  valid=gr_span_free(m,n);
  if(valid){for(UINTN i=0;i<n;i++)s->before[i]=m[i];s->count=n;}
 }else if(valid)valid=s->count&&gr_map_same(s,m,n,mode);
 /* A successful map call owns a pool allocation, even if its contents fail.
  * A null output is rejected without calling FreePool(NULL). */
 s->pool_status=m?uefi_call_wrapper(s->bs->FreePool,1,m):EFI_INVALID_PARAMETER;
 return valid&&!s->pool_status;
}
static int gr_context_valid(struct gr_state *s){
 return s&&s->image&&s->device&&s->image!=s->device&&
  gr_readable(s,s->bs,sizeof *s->bs)&&s->bs->FreePool&&
  gr_readable(s,s->dxe,sizeof *s->dxe)&&
  s->dxe->Hdr.Signature==UINT64_C(0x565245535f455844)&&
  s->dxe->Hdr.HeaderSize>=sizeof *s->dxe&&s->dxe->Hdr.HeaderSize<=4096&&
  s->dxe->AllocateMemorySpace&&s->dxe->FreeMemorySpace&&s->dxe->GetMemorySpaceMap;
}
static enum gr_status gr_reserve(struct gr_state *s){
 if(!s)return GR_REFUSED;
 if(s->status!=GR_NEW)return s->status;
 s->status=GR_REFUSED;
 if(s->allocate_attempted||s->free_attempted||s->pci_attempted||!gr_context_valid(s)||!gr_observe(s,-1))return s->status;
 s->allocate_attempted=1;s->status=GR_UNCERTAIN;s->returned_base=GR_BASE;
 /* PI: AllocateAddress=2, MemoryMappedIo=3, Alignment=log2(64 GiB)=36. */
 s->allocate_status=uefi_call_wrapper(s->dxe->AllocateMemorySpace,7,2,3,36,GR_LENGTH,&s->returned_base,s->image,s->device);
 if(s->returned_base!=GR_BASE)return s->status;
 if(gr_observe(s,1)){
  if(!s->allocate_status)s->status=GR_OWNED;
 }else if(s->allocate_status&&gr_observe(s,0))s->status=GR_REFUSED;
 return s->status;
}
static int gr_check_owned(struct gr_state *s){
 if(!gr_context_valid(s)||!s->allocate_attempted||s->free_attempted||
    s->returned_base!=GR_BASE||(s->status!=GR_OWNED&&s->status!=GR_UNCERTAIN))return 0;
 if(!gr_observe(s,1)){s->status=GR_UNCERTAIN;return 0;}
 return 1;
}
static enum gr_status gr_release(struct gr_state *s){
 if(!s)return GR_REFUSED;
 if(s->pci_attempted||s->free_attempted||!gr_check_owned(s))return s->status;
 s->free_attempted=1;s->status=GR_UNCERTAIN;
 s->free_status=uefi_call_wrapper(s->dxe->FreeMemorySpace,2,GR_BASE,GR_LENGTH);
 /* Error status never promotes success, even if a map later looks unowned. */
 int restored=gr_observe(s,0);
 if(!s->free_status&&restored)s->status=GR_RELEASED;
 return s->status;
}

/* Pair API is separate from the unchanged predecessor entrypoints above.
 * No recovery from uncertainty: even an allocation error with no visible side
 * effect retains every preceding reservation until external recovery. */
#define GR_PAIR_API 1
#include "../pins/target_policy.h"
enum gr_pair_failure {
 GR_PAIR_OK,GR_PAIR_CONTEXT,GR_PAIR_CONTENT,GR_PAIR_MAP,GR_PAIR_POOL,
 GR_PAIR_ALLOCATE,GR_PAIR_RETURNED_BASE,GR_PAIR_FREE
};
struct gr_pair_slot {
 EFI_HANDLE device;
 EFI_PHYSICAL_ADDRESS base,returned_base;
 UINT64 length;
 unsigned allocate_attempted,allocate_verified,free_attempted,free_verified;
 EFI_STATUS allocate_status,free_status;
};
struct gr_pair_state {
 GR_DXE *dxe;EFI_BOOT_SERVICES *bs;EFI_HANDLE image;
 int (*readable)(void *,uintptr_t,size_t);void *read_context;
 struct gr_pair_slot slot[PT_COUNT];
 enum gr_status status;
 unsigned owned_mask,pci_attempted;
 UINTN count;GR_DESCRIPTOR before[GR_MAX_DESCRIPTORS];
 EFI_STATUS map_status,pool_status;
 enum gr_pair_failure first_failure;
 EFI_STATUS first_status;
 unsigned first_slot; /* endpoint index, PT_COUNT means global map/context. */
};
static void gr_pair_fail(struct gr_pair_state *s,enum gr_pair_failure why,EFI_STATUS status,unsigned slot){
 if(s->first_failure==GR_PAIR_OK){s->first_failure=why;s->first_status=status;s->first_slot=slot;}
}
static int gr_pair_readable(struct gr_pair_state *s,const void *p,size_t n){
 return s&&s->readable&&p&&n&&(uintptr_t)p<=UINTPTR_MAX-n&&s->readable(s->read_context,(uintptr_t)p,n);
}
static int gr_pair_context_valid(struct gr_pair_state *s){
 if(!s||!s->image)return 0;
 for(unsigned i=0;i<PT_COUNT;i++){
  if(!s->slot[i].device||s->slot[i].device==s->image)return 0;
  for(unsigned j=0;j<i;j++)if(s->slot[i].device==s->slot[j].device)return 0;
 }
 return gr_pair_readable(s,s->bs,sizeof *s->bs)&&s->bs->FreePool&&
  gr_pair_readable(s,s->dxe,sizeof *s->dxe)&&
  s->dxe->Hdr.Signature==UINT64_C(0x565245535f455844)&&
  s->dxe->Hdr.HeaderSize>=sizeof *s->dxe&&s->dxe->Hdr.HeaderSize<=4096&&
  s->dxe->AllocateMemorySpace&&s->dxe->FreeMemorySpace&&s->dxe->GetMemorySpaceMap;
}
static int gr_pair_span_free(const GR_DESCRIPTOR *m,UINTN n,unsigned slot){
 uint64_t cursor=pair_targets[slot].bar1,limit=cursor+pair_targets[slot].reservation_size-1;
 for(UINTN i=0;i<n&&cursor<=limit;i++){
  uint64_t end=m[i].BaseAddress+m[i].Length-1;
  if(end<cursor)continue;
  if(m[i].BaseAddress>cursor||m[i].GcdMemoryType!=3||m[i].ImageHandle||m[i].DeviceHandle)return 0;
  if(end>=limit)return 1;
  cursor=end+1;
 }
 return 0;
}
/* The original full map is the only baseline. Split/coalesced descriptors are
 * equivalent only when every byte has identical type/capabilities/attributes
 * and the exact expected image/device owner. The inter-card gap is unchanged. */
static int gr_pair_map_same(struct gr_pair_state *s,const GR_DESCRIPTOR *now,UINTN count,unsigned mask){
 if(!pair_prefix_mask(mask)||!s->count||!count)return 0;
 UINTN a=0,b=0;uint64_t pa=s->before[0].BaseAddress,pb=now[0].BaseAddress;
 while(a<s->count&&b<count){
  const GR_DESCRIPTOR *x=&s->before[a],*y=&now[b];
  if(pa!=pb||x->GcdMemoryType!=y->GcdMemoryType||x->Capabilities!=y->Capabilities||x->Attributes!=y->Attributes)return 0;
  uint64_t ae=x->BaseAddress+x->Length-1,be=y->BaseAddress+y->Length-1,end=ae<be?ae:be;
  EFI_HANDLE image=x->ImageHandle,device=x->DeviceHandle;
  for(unsigned i=0;i<PT_COUNT;i++){
   uint64_t base=pair_targets[i].bar1,limit=base+pair_targets[i].reservation_size-1;
   if(pa<base&&end>=base)end=base-1;
   if(pa>=base&&pa<=limit){
    if(end>limit)end=limit;
    if(mask&(1U<<i)){image=s->image;device=s->slot[i].device;}
   }
  }
  if(y->ImageHandle!=image||y->DeviceHandle!=device)return 0;
  if(end==ae){a++;if(a<s->count)pa=s->before[a].BaseAddress;}else pa=end+1;
  if(end==be){b++;if(b<count)pb=now[b].BaseAddress;}else pb=end+1;
 }
 return a==s->count&&b==count;
}
/* Firmware calls remain synchronous: bounds cannot preempt a hung callback.
 * Caller supplies fresh typed-RAM validation, and associates the two opaque
 * DeviceHandles with the exact endpoint identities before entering this API. */
static int gr_pair_observe(struct gr_pair_state *s,int capture,unsigned mask){
 UINTN n=0;GR_DESCRIPTOR *m=NULL;
 s->map_status=uefi_call_wrapper(s->dxe->GetMemorySpaceMap,2,&n,&m);
 if(s->map_status){gr_pair_fail(s,GR_PAIR_MAP,s->map_status,PT_COUNT);s->status=GR_UNCERTAIN;return 0;}
 int valid=n&&n<=GR_MAX_DESCRIPTORS&&gr_pair_readable(s,m,n*sizeof *m)&&gr_map_valid(m,n);
 if(valid&&capture)for(unsigned i=0;i<PT_COUNT&&valid;i++)valid=gr_pair_span_free(m,n,i);
 else if(valid)valid=gr_pair_map_same(s,m,n,mask);
 if(!valid)gr_pair_fail(s,GR_PAIR_CONTENT,0,PT_COUNT);
 /* Capture only after both free spans pass. Never establish a second baseline. */
 if(valid&&capture){for(UINTN i=0;i<n;i++)s->before[i]=m[i];s->count=n;}
 /* A successful map owns its returned pool even if content is invalid.
  * Null output is invalid content and must never be passed to FreePool. */
 if(m){
  s->pool_status=uefi_call_wrapper(s->bs->FreePool,1,m);
  if(s->pool_status){gr_pair_fail(s,GR_PAIR_POOL,s->pool_status,PT_COUNT);s->status=GR_UNCERTAIN;return 0;}
 }
 return valid;
}
static enum gr_status gr_pair_reserve(struct gr_pair_state *s){
 if(!s)return GR_REFUSED;
 if(s->status!=GR_NEW)return s->status;
 s->status=GR_REFUSED;
 unsigned touched=0;
 for(unsigned i=0;i<PT_COUNT;i++)touched|=s->slot[i].allocate_attempted|s->slot[i].free_attempted;
 if(s->pci_attempted||s->owned_mask||s->first_failure||s->count||touched||!gr_pair_context_valid(s)){
  gr_pair_fail(s,GR_PAIR_CONTEXT,0,PT_COUNT);return s->status;
 }
 for(unsigned i=0;i<PT_COUNT;i++){s->slot[i].base=pair_targets[i].bar1;s->slot[i].length=pair_targets[i].reservation_size;}
 if(!gr_pair_observe(s,1,0))return s->status;
 for(unsigned i=0;i<PT_COUNT;i++){
  struct gr_pair_slot *p=&s->slot[i];p->allocate_attempted=1;p->returned_base=p->base;s->status=GR_UNCERTAIN;
  p->allocate_status=uefi_call_wrapper(s->dxe->AllocateMemorySpace,7,2,3,36,p->length,&p->returned_base,s->image,p->device);
  if(p->allocate_status){gr_pair_fail(s,GR_PAIR_ALLOCATE,p->allocate_status,i);return s->status;}
  if(p->returned_base!=p->base){gr_pair_fail(s,GR_PAIR_RETURNED_BASE,0,i);return s->status;}
  unsigned mask=(1U<<(i+1))-1U;
  if(!gr_pair_observe(s,0,mask))return s->status;
  p->allocate_verified=1;s->owned_mask=mask;
 }
 s->status=GR_OWNED;return s->status;
}
static int gr_pair_check_owned(struct gr_pair_state *s){
 if(!s||s->status!=GR_OWNED)return 0;
 if(s->first_failure||s->owned_mask!=PT_ALL_MASK||!gr_pair_context_valid(s))goto fail;
 for(unsigned i=0;i<PT_COUNT;i++){
  const struct gr_pair_slot *p=&s->slot[i];
  if(!p->allocate_attempted||!p->allocate_verified||p->allocate_status||p->free_attempted||
     p->base!=pair_targets[i].bar1||p->length!=pair_targets[i].reservation_size||p->returned_base!=p->base)goto fail;
 }
 if(!gr_pair_observe(s,0,PT_ALL_MASK)){s->status=GR_UNCERTAIN;return 0;}
 return 1;
fail:
 gr_pair_fail(s,GR_PAIR_CONTEXT,0,PT_COUNT);s->status=GR_UNCERTAIN;return 0;
}
static enum gr_status gr_pair_release_before_mutation(struct gr_pair_state *s){
 if(!s)return GR_REFUSED;
 if(s->pci_attempted||s->status!=GR_OWNED)return s->status;
 if(!gr_pair_check_owned(s))return s->status;
 for(unsigned remaining=PT_COUNT;remaining;remaining--){
  unsigned i=remaining-1;struct gr_pair_slot *p=&s->slot[i];
  /* Recheck the entire remaining map before each free after the first (the
   * ownership check above covers the first). Any error stops the whole
   * cleanup permanently. */
  if(i<PT_COUNT-1&&!gr_pair_observe(s,0,(1U<<(i+1))-1U)){s->status=GR_UNCERTAIN;return s->status;}
  if(s->pci_attempted)return s->status;
  p->free_attempted=1;s->status=GR_UNCERTAIN;
  p->free_status=uefi_call_wrapper(s->dxe->FreeMemorySpace,2,p->base,p->length);
  if(p->free_status){gr_pair_fail(s,GR_PAIR_FREE,p->free_status,i);return s->status;}
  unsigned mask=(1U<<i)-1U;
  if(!gr_pair_observe(s,0,mask))return s->status;
  p->free_verified=1;s->owned_mask=mask;
 }
 s->status=GR_RELEASED;return s->status;
}
#endif
