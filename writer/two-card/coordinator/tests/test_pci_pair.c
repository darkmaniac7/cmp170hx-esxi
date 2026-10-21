/* Synthetic EFI transport for the actual pair access and transaction C.
 * Historical second-card extended trace is retained independently of01. */
#define main legacy_access_main
#include "test_pci_access.c"
#undef main
#include "../pins/resource_types.h"
#include "../../fixtures/second_target_extended.h"
#include "../../fixtures/native_extended.h"
static struct pa_context contexts[2];
static struct pa_pair_state pair;
static struct gr_pair_state pair_gcd;
static EFI_PCI_IO_PROTOCOL devices[4];
static uint32_t pair_config[4][1024];
static GR_DESCRIPTOR pair_map[5];
static unsigned pair_owners,pair_writes,bad_handle,reenter_read,reenter_write;
static struct bt_state states[2];
static void pair_rebuild(void){
 uint64_t ends[]={UINT64_C(0x63d20200000),pair_targets[0].bar1,pair_targets[0].bar1+BT_SPAN,pair_targets[1].bar1,pair_targets[1].bar1+BT_SPAN,UINT64_C(0x7fc00000000)};
 for(unsigned i=0;i<5;i++){
  pair_map[i]=(GR_DESCRIPTOR){.BaseAddress=ends[i],.Length=ends[i+1]-ends[i],.Capabilities=UINT64_C(0xc000000000026001),.Attributes=1,.GcdMemoryType=3};
  if(i%2&&(pair_owners&(1U<<(i/2)))){pair_map[i].ImageHandle=(VOID *)1;pair_map[i].DeviceHandle=(VOID *)(uintptr_t)(2+i/2*2);}
 }
}
static EFI_STATUS EFIAPI pair_getmap(UINTN *n,GR_DESCRIPTOR **out){
 if(failure('m'))return EFI_DEVICE_ERROR;
 *n=5;*out=malloc(sizeof pair_map);assert(*out);memcpy(*out,pair_map,sizeof pair_map);return 0;
}
static EFI_STATUS EFIAPI pair_alloc(UINT32 policy,UINT32 type,UINTN align,UINT64 len,EFI_PHYSICAL_ADDRESS *base,EFI_HANDLE image,EFI_HANDLE device){
 unsigned t=pair_owners?1:0;assert(policy==2&&type==3&&align==36&&len==BT_SPAN&&*base==pair_targets[t].bar1&&image==(VOID *)1&&device==(VOID *)(uintptr_t)(2+2*t));
 pair_owners|=1U<<t;pair_rebuild();return 0;
}
static EFI_STATUS EFIAPI pair_location(EFI_PCI_IO_PROTOCOL *p,UINTN *seg,UINTN *bus,UINTN *dev,UINTN *fn){
 unsigned i=(unsigned)(p-devices);assert(i<4);uint32_t bdf=i%2?pair_targets[i/2].port:pair_targets[i/2].endpoint;
 *seg=0;*bus=bdf>>8;*dev=bdf>>3&31;*fn=bdf&7;return failure('l')?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI pair_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *out){
 if(reenter_read){reenter_read=0;assert(pa_apply(&contexts[0],&states[0])!=BT_APPLIED);}
 unsigned i=(unsigned)(p-devices);assert(i<4&&count==1);uint32_t v=pair_config[i][off/4];int bad=failure('r');
 if(bad&&fail_allones)v=UINT32_MAX;
 if(width==EfiPciIoWidthUint16)*(UINT16 *)out=(UINT16)(v>>((off&3)*8));else *(UINT32 *)out=v;
 return bad&&!fail_allones?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI pair_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *in){
 unsigned i=(unsigned)(p-devices);assert(i<4&&count==1&&i/2==pair_writes/13);
 assert(pair_gcd.pci_attempted&&pair.attempted==pair_writes+1);
 uint32_t v=width==EfiPciIoWidthUint16?*(UINT16 *)in:*(UINT32 *)in;
 if(width==EfiPciIoWidthUint16)pair_config[i][off/4]=(pair_config[i][off/4]&0xffff0000U)|v;else pair_config[i][off/4]=v;
 if(!(i%2)&&off==0xdcc)pair_config[i][0xbbc/4]=BT_CFG_CAP_OBSERVED;
 pair_writes++;
 if(reenter_write){reenter_write=0;assert(pa_apply(&contexts[0],&states[0])!=BT_APPLIED);}
 return failure('w')?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI pair_handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out){
 EFI_GUID expected=EFI_PCI_IO_PROTOCOL_GUID;assert(!memcmp(guid,&expected,sizeof expected));
 unsigned i=(unsigned)(uintptr_t)h-2;if(failure('h')||i>=4)return EFI_DEVICE_ERROR;
 *out=&devices[bad_handle?(i^2):i];return 0;
}
static EFI_STATUS EFIAPI pair_opens(EFI_HANDLE h,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **entries,UINTN *n){
 (void)guid;assert((uintptr_t)h>=2&&(uintptr_t)h<=5);*entries=NULL;*n=0;return failure('o')?EFI_DEVICE_ERROR:0;
}
static void pair_setup(void){
 memset(contexts,0,sizeof contexts);memset(&pair,0,sizeof pair);memset(&pair_gcd,0,sizeof pair_gcd);memset(states,0,sizeof states);
 memset(devices,0,sizeof devices);memset(pair_config,0,sizeof pair_config);memset(&handoff,0,sizeof handoff);memset(&bs,0,sizeof bs);memset(&dxe,0,sizeof dxe);
 calls=fail_at=fail_allones=pair_writes=pair_owners=bad_handle=reenter_read=reenter_write=0;
 bs.FreePool=freepool;bs.OpenProtocolInformation=pair_opens;bs.HandleProtocol=pair_handle;
 dxe.Hdr.Signature=UINT64_C(0x565245535f455844);dxe.Hdr.HeaderSize=sizeof dxe;
 dxe.AllocateMemorySpace=pair_alloc;dxe.FreeMemorySpace=freemem;dxe.GetMemorySpaceMap=pair_getmap;
 pair_gcd.dxe=&dxe;pair_gcd.bs=&bs;pair_gcd.image=(VOID *)1;pair_gcd.readable=readable;
 pair_gcd.slot[0].device=(VOID *)2;pair_gcd.slot[1].device=(VOID *)4;pair_rebuild();assert(gr_pair_reserve(&pair_gcd)==GR_OWNED);
 handoff.binding.parent=1;handoff.binding.device=77;handoff.child=88;handoff.phase=HF_MUTATING;handoff.status=HF_OK;handoff.mutation_started=1;
 for(unsigned t=0;t<2;t++){
  struct pa_context *x=&contexts[t];plan_fixture(&x->plan);memcpy(pair_config[2*t],config[0],sizeof config[0]);memcpy(pair_config[2*t+1],config[1],sizeof config[1]);
  if(!t)for(unsigned j=0;j<sizeof native_pairs/sizeof *native_pairs;j++)pair_config[0][native_pairs[j].offset/4]=native_pairs[j].before;
  else for(unsigned j=0;j<sizeof second_target_trace/sizeof *second_target_trace;j++)pair_config[2][second_target_trace[j].offset/4]=second_target_trace[j].value;
  pair_config[2*t+1][6]=(t+1)*0x10100;
  struct bt_plan *p=&x->plan;p->target_index=t;p->endpoint=pair_targets[t].endpoint;p->bridge=pair_targets[t].port;
  p->bar1_base=p->bridge_base=p->reservation_base=pair_targets[t].bar1;p->bar3_base=pair_targets[t].bar3;p->bridge_limit=p->bar3_base+BT_BAR3_SIZE-1;
  memcpy(p->endpoint_before,pair_config[2*t],64);memcpy(p->bridge_before,pair_config[2*t+1],64);
  memcpy(config[0],pair_config[2*t],sizeof config[0]);p->rebar_before=decode_rebar(cfg32,NULL);
  for(unsigned j=0;j<2;j++){devices[2*t+j].GetLocation=pair_location;devices[2*t+j].Pci.Read=pair_read;devices[2*t+j].Pci.Write=pair_write;}
  x->endpoint=(struct pa_device){(VOID *)(uintptr_t)(2+2*t),&devices[2*t],p->endpoint,0x20c210de};
  x->bridge=(struct pa_device){(VOID *)(uintptr_t)(3+2*t),&devices[2*t+1],p->bridge,0x14831022};x->bs=&bs;x->readable=readable;x->handoff=&handoff;x->ready=1;
 }
 pair.reservation=&pair_gcd;pair.handoff=&handoff;calls=0;
}
static int run_pair(void){return pa_pair_bind(&pair,contexts)&&pa_apply(&contexts[0],&states[0])==BT_APPLIED&&pa_apply(&contexts[1],&states[1])==BT_APPLIED;}
int main(void){unsigned checks=0;
#define Q(x) do{checks++;assert(x);}while(0)
 pair_setup();Q(run_pair());Q(pair_writes==26&&pair.attempted==26&&pair.completed_mask==3&&!pair.failed);
 unsigned total=calls;char success[4096];memcpy(success,trace,total);
 for(unsigned at=1;at<=total;at++)for(unsigned ones=0;ones<2;ones++){
  if(ones&&success[at-1]!='r')continue;
  pair_setup();fail_at=at;fail_allones=ones;Q(!run_pair());Q(pair.failed);
  unsigned count=pair_writes,events=calls;
  Q(pa_apply(&contexts[1],&states[1])!=BT_APPLIED&&pair_writes==count&&calls==events);
  Q(!pa_pair_bind(&pair,contexts)&&pair_writes==count&&calls==events);
 }
 unsigned write13=0,seen_writes=0;
 for(unsigned i=0;i<total;i++)if(success[i]=='w'&&++seen_writes==13){write13=i+1;break;}
 Q(write13&&success[write13]=='l'&&success[write13+2]=='r');
 pair_setup();fail_at=write13+3;Q(!run_pair()&&pair_writes==13&&pair.completed_mask==0&&states[0].status==BT_COLD_RECOVERY_REQUIRED);
 for(unsigned t=0;t<2;t++)for(unsigned bad=0;bad<10;bad++){
  pair_setup();struct pa_context *x=&contexts[t];
  if(bad==0)x->plan.target_index^=1;
  if(bad==1)x->endpoint.handle=contexts[1-t].endpoint.handle;
  if(bad==2)x->endpoint.pci=contexts[1-t].endpoint.pci;
  if(bad==3)x->endpoint.bdf=0x4700;
  if(bad==4)x->bridge.bdf=0x4800;
  if(bad==5)pair_gcd.slot[t].device=(VOID *)99;
  if(bad==6)x->plan.reservation_owner=99;
  if(bad==7)x->plan.bar1_base+=BT_BAR1_SIZE;
  if(bad==8)bad_handle=1;
  if(bad==9)x->bridge.handle=contexts[1-t].bridge.handle;
  Q(!run_pair()&&pair_writes==0&&pair.failed);
 }
 pair_setup();Q(pa_pair_bind(&pair,contexts));Q(pa_apply(&contexts[1],&states[1])!=BT_APPLIED&&pair_writes==0&&pair.failed);
 pair_setup();Q(pa_pair_bind(&pair,contexts));struct bt_io io={&contexts[0],pa_read,pa_write,pa_owned};
 Q(bt_apply(&contexts[0].plan,&io,&states[0])==BT_APPLIED&&pair.completed_mask==0);
 Q(pa_apply(&contexts[1],&states[1])!=BT_APPLIED&&pair_writes==13&&pair.failed);
 for(unsigned t=0;t<2;t++)for(unsigned k=0;k<4;k++){
  pair_setup();Q(pa_pair_bind(&pair,contexts));if(t)Q(pa_apply(&contexts[0],&states[0])==BT_APPLIED);
  unsigned previous=pair_writes;uint32_t wrong[]={0x4700,0x4800,pair_targets[1-t].endpoint,pair_targets[1-t].port};
  Q(pa_write(&contexts[t],wrong[k],4,2,1)==0&&pair_writes==previous&&pair.failed);
 }
 for(unsigned t=0;t<2;t++)for(unsigned bad=0;bad<8;bad++){
  pair_setup();Q(pa_pair_bind(&pair,contexts));if(t)Q(pa_apply(&contexts[0],&states[0])==BT_APPLIED);
  unsigned previous=pair_writes;
  if(bad==0)contexts[t].pair_slot^=1;
  if(bad==1)contexts[t].endpoint.handle=contexts[1-t].endpoint.handle;
  if(bad==2)contexts[t].handoff=NULL;
  if(bad==3)pair_gcd.slot[t].device=(VOID *)99;
  if(bad==4)handoff.binding.device++;
  if(bad==5)handoff.phase=HF_PREPARED;
  if(bad==6)contexts[t].plan.target_index^=1;
  if(bad==7)bad_handle=1;
  Q(pa_apply(&contexts[t],&states[t])!=BT_APPLIED&&pair_writes==previous&&pair.failed);
 }
 pair_setup();Q(pa_pair_bind(&pair,contexts));reenter_read=1;
 Q(pa_apply(&contexts[0],&states[0])!=BT_APPLIED&&pair.failed&&pair_writes==0);
 pair_setup();Q(pa_pair_bind(&pair,contexts));reenter_write=1;
 Q(pa_apply(&contexts[0],&states[0])==BT_COLD_RECOVERY_REQUIRED&&pair.failed&&pair_writes==1);
 pair_setup();Q(run_pair());unsigned prior=pair_writes;
 Q(pa_apply(&contexts[0],&states[0])==BT_COLD_RECOVERY_REQUIRED&&pair.failed&&pair_writes==prior);
 printf("%u shared pair PCI_IO checks PASS (%u success callbacks,26 stores)\n",checks,total);return 0;
}
