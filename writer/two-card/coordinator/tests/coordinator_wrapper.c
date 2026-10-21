/* Entire EFI provider is synthetic. Real rc_validate and transaction execute. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../src/coordinator.h"
static struct pc_pair_session session;
static struct pc_environment env;
static EFI_BOOT_SERVICES bs;
static GR_DXE dxe;
static EFI_PCI_IO_PROTOCOL devices[RC_FUNCTIONS];
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL roots[RC_ROOTS];
static uint32_t cfg[RC_FUNCTIONS][1024];
static const struct rc_snapshot *fixture;
static struct hf_session hf;
static unsigned mode,writes,allocs,frees,acpi_reads,pcienums,hfchecks,starts;
static unsigned target[2],port[2];
static GR_DESCRIPTOR gcd[5],original;
static unsigned owned,mutation_checks,start_checks;
/* Fault positions cover actual firmware/handoff callback entries, including
 * output/side-effect ambiguity for stores, allocation, free and pool release. */
enum event_kind {EV_ENUM,EV_HANDLE,EV_LOCATION,EV_READ,EV_WRITE,EV_OPEN,EV_ATTR,
 EV_ROOT,EV_MEMORY,EV_GCD,EV_ALLOC,EV_FREE,EV_POOL,EV_ACPI,EV_CURRENT,EV_CHILD,EV_START};
static unsigned calls,fail_at,effect,event_hit,hit_writes,hit_kind;
static unsigned range_mode,range_fail,range_calls;
static int event(unsigned kind){
 calls++;if(calls!=fail_at)return 0;
 event_hit=1;hit_writes=writes;hit_kind=kind;return 1;
}
static void *held[2048];static unsigned held_count;
static void *pool(size_t n){void *p=malloc(n);assert(p&&held_count<2048);held[held_count++]=p;return p;}
static void pool_drop(void *p){for(unsigned i=0;i<held_count;i++)if(held[i]==p){held[i]=held[--held_count];free(p);return;}assert(0);}

static UINTN gcd_count;
static int readable(void *c,uintptr_t p,size_t n){
 (void)c;
 if(range_mode&&++range_calls==range_fail){event_hit=1;hit_writes=writes;hit_kind=17;return 0;}
 return p&&n&&p<=UINTPTR_MAX-n;
}
static EFI_STATUS EFIAPI freepool(VOID *p){int fail=event(EV_POOL);if(!fail||effect)pool_drop(p);return fail?EFI_DEVICE_ERROR:0;}
static EFI_STATUS EFIAPI enumerate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out){
 if(event(EV_ENUM))return EFI_DEVICE_ERROR;
 EFI_GUID pg=EFI_PCI_IO_PROTOCOL_GUID,rg=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;assert(type==ByProtocol&&!key);
 int ispci=!memcmp(guid,&pg,sizeof pg);assert(ispci||!memcmp(guid,&rg,sizeof rg));
 if(ispci){pcienums++;if(mode==5&&pcienums==2)cfg[50][15]^=1;if(mode==6&&pcienums==3)cfg[50][15]^=1;if(mode==14&&pcienums==5)cfg[50][15]^=1;if(mode==23&&pcienums==4)cfg[target[0]][0x724/4]^=1;}
 *n=ispci?RC_FUNCTIONS:RC_ROOTS;*out=pool(*n*sizeof **out);assert(*out);
 for(UINTN i=0;i<*n;i++)(*out)[i]=(VOID *)(uintptr_t)(i+(ispci?100:1000));
 return 0;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out){
 if(event(EV_HANDLE))return EFI_DEVICE_ERROR;
 EFI_GUID pg=EFI_PCI_IO_PROTOCOL_GUID;int ispci=!memcmp(guid,&pg,sizeof pg);
 unsigned i=(unsigned)(uintptr_t)h-(ispci?100:1000);assert(i<(ispci?RC_FUNCTIONS:RC_ROOTS));
 *out=ispci?(VOID *)&devices[i]:(VOID *)&roots[i];return 0;
}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *p,UINTN *seg,UINTN *bus,UINTN *dev,UINTN *fn){
 if(event(EV_LOCATION))return EFI_DEVICE_ERROR;
 unsigned i=(unsigned)(p-devices);assert(i<RC_FUNCTIONS);uint32_t b=fixture->functions[i].bdf;
 *seg=0;*bus=b>>8;*dev=b>>3&31;*fn=b&7;return 0;
}
static EFI_STATUS EFIAPI readcfg(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN count,VOID *out){
 if(event(EV_READ))return EFI_DEVICE_ERROR;
 unsigned i=(unsigned)(p-devices);assert(i<RC_FUNCTIONS&&count==1&&(w==EfiPciIoWidthUint16||w==EfiPciIoWidthUint32));
 uint32_t v=cfg[i][off/4];if(w==EfiPciIoWidthUint16)*(UINT16 *)out=(UINT16)(v>>((off&3)*8));else *(UINT32 *)out=v;return 0;
}
static EFI_STATUS EFIAPI writecfg(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN count,VOID *in){
 unsigned i=(unsigned)(p-devices),t=writes/13;assert(t<2&&(i==target[t]||i==port[t])&&count==1);writes++;
 assert(allocs==2&&owned==3&&acpi_reads==2);
 int fail=event(EV_WRITE);if(fail&&!effect)return EFI_DEVICE_ERROR;
 assert(hf.phase==HF_MUTATING&&hf.mutation_started&&!hf.start_called);
 uint32_t v=w==EfiPciIoWidthUint16?*(UINT16 *)in:*(UINT32 *)in;
 if(off!=4)assert(!(cfg[target[t]][1]&6)&&!(cfg[port[t]][1]&6));
 if(w==EfiPciIoWidthUint16)cfg[i][off/4]=(cfg[i][off/4]&0xffff0000U)|v;else cfg[i][off/4]=v;
 if(mode==32&&writes==20)cfg[target[0]][0x724/4]^=1;
 if(i==target[t]&&off==0xdcc)cfg[i][0xbbc/4]=BT_CFG_CAP_OBSERVED;
 if(fail)return EFI_DEVICE_ERROR;
 if(mode>=100&&mode<126&&writes==mode-99)return EFI_DEVICE_ERROR;
 if(mode>=140&&mode<166&&writes==mode-139)cfg[i][off/4]^=0x100;
 return 0;
}
static EFI_STATUS EFIAPI opens(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n){
 if(event(EV_OPEN))return EFI_DEVICE_ERROR;
 (void)g;assert(h==(VOID *)(uintptr_t)(target[0]+100)||h==(VOID *)(uintptr_t)(port[0]+100)||h==(VOID *)(uintptr_t)(target[1]+100)||h==(VOID *)(uintptr_t)(port[1]+100));*n=0;*out=NULL;
 if(mode==7||(mode==15&&pcienums==5)||(mode==26&&h==(VOID *)(uintptr_t)(target[1]+100)&&writes==13)){*n=1;*out=pool(sizeof **out);**out=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){(VOID *)8,NULL,EFI_OPEN_PROTOCOL_BY_DRIVER,1};}return 0;
}
static EFI_STATUS EFIAPI attributes(EFI_PCI_IO_PROTOCOL *p,UINT8 index,UINT64 *supports,VOID **out){
 assert(!writes);if(event(EV_ATTR))return EFI_DEVICE_ERROR;
 unsigned owner=(unsigned)(p-devices);for(size_t i=0;i<fixture->query_count;i++){
  const struct rc_query *q=&fixture->queries[i];if(q->owner_index!=owner||q->query_index!=index)continue;
  *supports=q->supports;*out=NULL;if(q->resources.size){*out=pool(q->resources.size);memcpy(*out,q->resources.data,q->resources.size);}return q->status;
 }assert(0);return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI rootconf(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *p,VOID **out){
 assert(!writes);if(event(EV_ROOT))return EFI_DEVICE_ERROR;
 unsigned i=(unsigned)(p-roots);assert(i<RC_ROOTS);*out=(VOID *)fixture->roots[i].resources.data;return fixture->roots[i].status;
}
static EFI_STATUS EFIAPI memorymap(UINTN *n,EFI_MEMORY_DESCRIPTOR *out,UINTN *key,UINTN *stride,UINT32 *version){
 if(event(EV_MEMORY))return EFI_DEVICE_ERROR;
 const struct rc_memory *m=&fixture->memory;assert(*n>=m->raw.size);memcpy(out,m->raw.data,m->raw.size);*n=m->raw.size;*key=m->map_key;*stride=m->descriptor_size;*version=m->descriptor_version;
 if(mode==8&&acpi_reads==2)out->PhysicalStart=GR_BASE;
 return m->status;
}
static EFI_STATUS EFIAPI getgcd(UINTN *n,GR_DESCRIPTOR **out){
 if(event(EV_GCD))return EFI_DEVICE_ERROR;
 *n=gcd_count;*out=pool(*n*sizeof **out);memcpy(*out,gcd,*n*sizeof **out);
 if(mode==9&&pcienums==5)(*out)[1].ImageHandle=(VOID *)66;
 if(mode==22&&writes>=13)(*out)[1].Attributes^=1;
 return 0;
}
static void gcd_rebuild(void){
 gcd_count=0;uint64_t cursor=original.BaseAddress;
 for(unsigned t=0;t<2;t++)if(owned&(1U<<t)){
  uint64_t base=pair_targets[t].bar1;
  gcd[gcd_count]=original;gcd[gcd_count].BaseAddress=cursor;gcd[gcd_count++].Length=base-cursor;
  gcd[gcd_count]=original;gcd[gcd_count].BaseAddress=base;gcd[gcd_count].Length=GR_LENGTH;
  gcd[gcd_count].ImageHandle=(VOID *)1;gcd[gcd_count++].DeviceHandle=(VOID *)(uintptr_t)(target[t]+100);
  cursor=base+GR_LENGTH;
 }
 gcd[gcd_count]=original;gcd[gcd_count].BaseAddress=cursor;
 gcd[gcd_count++].Length=original.BaseAddress+original.Length-cursor;
}
static EFI_STATUS EFIAPI allocate(UINT32 kind,UINT32 type,UINTN align,UINT64 length,EFI_PHYSICAL_ADDRESS *base,EFI_HANDLE image,EFI_HANDLE device){
 unsigned t=allocs++;assert(t<2&&kind==2&&type==3&&align==36&&length==GR_LENGTH&&*base==pair_targets[t].bar1&&image==(VOID *)1&&device==(VOID *)(uintptr_t)(target[t]+100));
 int fail=event(EV_ALLOC);if(fail&&!effect)return EFI_DEVICE_ERROR;
 if(mode==10||(mode==27&&t==1))return EFI_OUT_OF_RESOURCES;
 if(mode==19){*base+=BT_BAR1_SIZE;return 0;}
 owned|=1U<<t;gcd_rebuild();
 if(fail)return EFI_DEVICE_ERROR;
 return mode==11||(mode==28&&t==1)?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI releasegcd(EFI_PHYSICAL_ADDRESS base,UINT64 length){
 assert(!writes&&length==GR_LENGTH);frees++;
 unsigned t=base==pair_targets[0].bar1?0:1;assert(base==pair_targets[t].bar1);
 int fail=event(EV_FREE);if(fail&&!effect)return EFI_DEVICE_ERROR;
 owned&=~(1U<<t);gcd_rebuild();return fail?EFI_DEVICE_ERROR:0;
}
static int acpi(void *c,struct rc_acpi *out){(void)c;if(event(EV_ACPI))return 0;acpi_reads++;*out=fixture->acpi;if(mode==20&&acpi_reads==2)out->dsdt.size--;return !(mode==4&&acpi_reads==2);}
static uint64_t current(void *c,enum hf_check_stage st,const struct hf_binding *b){
 if(st==HF_CHECK_BEFORE_MUTATION)assert(++mutation_checks==1);
 if(st==HF_CHECK_BEFORE_START)assert(++start_checks==1);
 if(event(EV_CURRENT))return 1;
 (void)c;assert(b==&hf.binding);hfchecks++;return (mode==12&&st==HF_CHECK_BEFORE_MUTATION)||(mode==16&&st==HF_CHECK_BEFORE_START)?1:0;
}
static uint64_t child(void *c,uintptr_t h,struct hf_child *out){
 if(event(EV_CHILD))return 1;
 (void)c;assert(h==hf.child);*out=(struct hf_child){hf.binding.parent,hf.binding.device,hf.binding.file_dp,hf.options,hf.binding.file_dp_size,hf.options_size};return (mode==24&&hf.phase==HF_MUTATION_CHECKING)||(mode==25&&hf.phase==HF_FINAL_CHECKING)?1:0;
}
static uint64_t start(void *c,uintptr_t h){(void)c;assert(h==hf.child);starts++;
 assert(session.pair.handoff==&hf&&session.pair.contexts[0]==&session.access[0]&&
 session.pair.contexts[1]==&session.access[1]&&session.pair.reservation==&session.reservation&&
 session.final_checks&&session.applied_mask==3&&session.pair.completed_mask==3&&
 session.pci[4].status==PI_PCI_CAPTURED&&acpi_reads==2&&pcienums==5);
 return event(EV_START)?1:0;}
static void setup(const struct rc_snapshot *f,unsigned m){
 while(held_count)free(held[--held_count]);
 calls=event_hit=hit_writes=hit_kind=0;
 memset(&session,0,sizeof session);memset(&env,0,sizeof env);memset(&bs,0,sizeof bs);memset(&dxe,0,sizeof dxe);memset(devices,0,sizeof devices);memset(roots,0,sizeof roots);memset(cfg,0,sizeof cfg);memset(&hf,0,sizeof hf);
 fixture=f;mode=m;writes=allocs=frees=acpi_reads=pcienums=hfchecks=starts=0;
 for(unsigned i=0;i<RC_FUNCTIONS;i++){
  memcpy(cfg[i],f->functions[i].header,64);for(size_t j=0;j<f->functions[i].conventional_count;j++){const struct rc_read *r=&f->functions[i].conventional[j];cfg[i][r->offset/4]=r->value;}
  devices[i].GetLocation=location;devices[i].Pci.Read=readcfg;devices[i].Pci.Write=writecfg;devices[i].GetBarAttributes=attributes;
  for(unsigned t=0;t<2;t++){if(f->functions[i].bdf==pair_targets[t].endpoint)target[t]=i;
  if(f->functions[i].bdf==pair_targets[t].port)port[t]=i;}
  if(f->functions[i].header[0]==0x20c210de){unsigned t=f->functions[i].bdf==0x200?1:0;for(size_t j=0;j<f->target_extcaps[t].count;j++){const struct rc_read *r=&f->target_extcaps[t].trace[j];cfg[i][r->offset/4]=r->value;}cfg[i][0x724/4]=0x10;cfg[i][0xdcc/4]=2;}
 }
 /* Synthetic PCIe conventional capabilities, explicitly not captured values. */
 for(unsigned t=0;t<2;t++){
 cfg[target[t]][1]|=0x100000;cfg[target[t]][13]=0x78;cfg[target[t]][0x78/4]=0x00020010;cfg[target[t]][0x80/4]=0;
 cfg[port[t]][1]|=0x100000;cfg[port[t]][13]=0x50;cfg[port[t]][0x50/4]=0x00420010;cfg[port[t]][0x58/4]=0;
 }
 for(unsigned i=0;i<RC_ROOTS;i++){roots[i].SegmentNumber=f->roots[i].segment;roots[i].Configuration=rootconf;}
 bs.LocateHandleBuffer=enumerate;bs.HandleProtocol=handle;bs.FreePool=freepool;bs.GetMemoryMap=memorymap;bs.OpenProtocolInformation=opens;
 dxe.Hdr.Signature=UINT64_C(0x565245535f455844);dxe.Hdr.HeaderSize=sizeof dxe;dxe.GetMemorySpaceMap=getgcd;dxe.AllocateMemorySpace=allocate;dxe.FreeMemorySpace=releasegcd;
 gcd_count=1;gcd[0]=(GR_DESCRIPTOR){.BaseAddress=UINT64_C(0x63d20200000),.Length=UINT64_C(0x1becfe00000),.Capabilities=UINT64_C(0xc000000000026001),.Attributes=1,.GcdMemoryType=3};
 original=gcd[0];owned=mutation_checks=start_checks=0;
 hf.phase=HF_PREPARED;hf.status=HF_OK;hf.child=88;hf.binding.parent=1;hf.binding.device=77;
 const uint8_t uuid[16]={0x15,0x2e,0xa0,0x5a,0xa2,0x0e,0x43,0xf0,0x8c,0x04,0x9c,0x6b,0,0xcf,0x57,0xa2};memcpy(hf.binding.host_uuid,uuid,16);hf.binding.boot_instance[0]=1;
 hf.binding.loader_sha256[0]=1;hf.binding.boot_sha256[0]=1;hf.binding.loader_size=1;hf.callbacks.check_current=current;hf.callbacks.inspect_child=child;hf.callbacks.start=start;
 inv_sha256(hf.options,0,hf.binding.options_sha256);
 env=(struct pc_environment){&bs,&dxe,(VOID *)1,&hf,readable,NULL,acpi,NULL,PC_POLICY_TRUSTED_FIRMWARE_R1};
 if(mode==1)env.policy=0;
 if(mode==2)hf.binding.host_uuid[0]^=1;
 if(mode==3)hf.phase=HF_TRANSACTION_OK;
 if(mode==13)cfg[target[0]][0x80/4]=0x00200000;
 if(mode==29)cfg[target[1]][0x80/4]=0x00200000;
 if(mode==30)cfg[target[0]][0x78/4]=0;
 if(mode==31)cfg[target[1]][0x78/4]=0;
 if(mode==17)gcd[0].ImageHandle=(VOID *)66;
 if(mode==18)gcd[0].GcdMemoryType=1;
}
int pc_test(const struct rc_snapshot *f,unsigned m,uint64_t out[18]){
 setup(f,m);enum pc_status rc=pc_pair_run(&session,&env);unsigned original_writes=writes;
 enum pc_status again=pc_pair_run(&session,&env);assert(again==rc&&writes==original_writes);
 out[0]=rc;out[1]=writes;out[2]=allocs;out[3]=frees;out[4]=session.reservation.status;out[5]=hf.phase;out[6]=session.transaction[1].status;out[7]=session.final_checks;out[8]=session.hardware_decode_observed;out[9]=session.atomic_hardware_isolation_observed;out[10]=session.coverage[0].coverage.reason;out[11]=session.coverage[1].coverage.reason;out[12]=session.pair.failed;out[13]=pcienums;out[14]=acpi_reads;out[15]=session.pair.attempted;out[16]=session.applied_mask;out[17]=owned;return rc;
}

int pc_start_test(const struct rc_snapshot *f,unsigned m,uint64_t out[8]){
 setup(f,m);assert(pc_pair_start(&session)==PC_NEW&&!starts&&!writes);pc_pair_run(&session,&env);out[0]=session.status;
 if(m==40)session.final_checks=0;
 if(m==41)session.applied_mask=1;
 if(m==42)session.pair.completed_mask=1;
 if(m==43)session.access[1].ready=0;
 if(m==44)session.transaction[1].status=BT_NEW;
 if(m==45)session.pair.failed=1;
 pc_pair_start(&session);out[1]=session.status;out[2]=starts;out[3]=writes;out[4]=frees;out[5]=hf.phase;
 unsigned before=pcienums;pc_pair_start(&session);out[6]=pcienums-before;out[7]=session.final_checks;return (int)session.status;
}
size_t pc_size(void){return sizeof(struct pc_pair_session);}

/* Return raw observations; Python asserts whole-attempt safety after every
 * success-path position, with the same fixture retained throughout the loop. */
int pc_fault_test(const struct rc_snapshot *f,unsigned position,unsigned after,unsigned scenario,uint64_t out[14]){
 fail_at=position;effect=after;setup(f,scenario);
 pc_pair_run(&session,&env);unsigned ready=session.status==PC_READY_FOR_HANDOFF;
 if(ready)pc_pair_start(&session);
 out[0]=calls;out[1]=event_hit;out[2]=hit_writes;out[3]=hit_kind;
 out[4]=session.status;out[5]=writes;out[6]=frees;out[7]=starts;
 out[8]=session.reservation.status;out[9]=owned;out[10]=session.applied_mask;
 out[11]=session.pair.attempted;out[12]=hf.phase;out[13]=ready;
 unsigned before=calls;pc_pair_run(&session,&env);pc_pair_start(&session);assert(calls==before);
 fail_at=0;return 1;
}

int pc_range_fault_test(const struct rc_snapshot *f,unsigned position,unsigned scenario,uint64_t out[14]){
 range_mode=1;range_fail=position;range_calls=0;
 pc_fault_test(f,0,0,scenario,out);out[0]=range_calls;
 range_mode=0;return 1;
}
