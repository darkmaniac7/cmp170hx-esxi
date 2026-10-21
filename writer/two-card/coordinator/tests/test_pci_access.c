/* All handles/configurations/ownership proofs in this test are SYNTHETIC.
 * This exercises ABI calls; it never contacts real firmware or a device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pci_access.h"
static struct pa_context c;
static struct gr_state reservation;
static struct hf_session handoff;
static GR_DXE dxe;
static GR_DESCRIPTOR map[3],original;
static UINTN map_count;
static EFI_BOOT_SERVICES bs;
static EFI_PCI_IO_PROTOCOL ep,br;
static uint32_t config[2][1024];
static unsigned calls,reads,writes,locations,fail_at,fail_allones,open_flags,drift_location;
static char trace[4096];
static int failure(char kind){assert(calls<sizeof trace);trace[calls++]=kind;return calls==fail_at;}
static int readable(void *unused,uintptr_t p,size_t n){(void)unused;return p&&n&&p<=UINTPTR_MAX-n;}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *pci,UINTN *seg,UINTN *bus,UINTN *dev,UINTN *fn){
 assert(pci==&ep||pci==&br);locations++;*seg=0;*bus=pci==&ep?1:0;*dev=pci==&ep?0:1;*fn=pci==&ep?0:1;
 if(drift_location)*seg=1;
 return failure('l')?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI readcfg(EFI_PCI_IO_PROTOCOL *pci,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *out){
 assert(pci==&ep||pci==&br);assert(count==1&&(width==EfiPciIoWidthUint16||width==EfiPciIoWidthUint32));
 reads++;uint32_t v=config[pci==&ep?0:1][off/4];int bad=failure('r');
 if(bad&&fail_allones)v=UINT32_MAX;
 if(width==EfiPciIoWidthUint16)*(UINT16 *)out=(UINT16)(v>>((off&3)*8));else *(UINT32 *)out=v;
 return bad&&!fail_allones?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI writecfg(EFI_PCI_IO_PROTOCOL *pci,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *in){
 assert(pci==&ep||pci==&br);assert(count==1);writes++;
 unsigned index=pci==&ep?0:1;UINT32 value=width==EfiPciIoWidthUint16?*(UINT16 *)in:*(UINT32 *)in;
 if(off!=4)assert(!(config[0][1]&6)&&!(config[1][1]&6));
 if(index==1&&off==4)assert(!(config[0][1]&6));
 if(width==EfiPciIoWidthUint16)config[index][off/4]=(config[index][off/4]&0xffff0000U)|value;else config[index][off/4]=value;
 if(index==0&&off==0xdcc)config[0][0xbbc/4]=BT_CFG_CAP_OBSERVED;
 /* The device may have consumed a failed write. */
 return failure('w')?EFI_DEVICE_ERROR:0;
}
static EFI_STATUS EFIAPI getmap(UINTN *n,GR_DESCRIPTOR **out){
 if(failure('m'))return EFI_DEVICE_ERROR;
 *n=map_count;*out=malloc(*n*sizeof **out);assert(*out);memcpy(*out,map,*n*sizeof **out);return 0;
}
static EFI_STATUS EFIAPI freepool(VOID *p){free(p);return failure('f')?EFI_DEVICE_ERROR:0;}
static EFI_STATUS EFIAPI alloc(UINT32 policy,UINT32 type,UINTN alignment,UINT64 length,EFI_PHYSICAL_ADDRESS *base,EFI_HANDLE image,EFI_HANDLE device){
 assert(policy==2&&type==3&&alignment==36&&length==GR_LENGTH&&*base==GR_BASE);
 original=map[0];map_count=3;map[0].Length=GR_BASE-original.BaseAddress;
 map[1]=original;map[1].BaseAddress=GR_BASE;map[1].Length=GR_LENGTH;map[1].ImageHandle=image;map[1].DeviceHandle=device;
 map[2]=original;map[2].BaseAddress=GR_LIMIT+1;map[2].Length=original.BaseAddress+original.Length-map[2].BaseAddress;
 return 0;
}
static EFI_STATUS EFIAPI freemem(EFI_PHYSICAL_ADDRESS base,UINT64 length){(void)base;(void)length;assert(0);return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI opens(EFI_HANDLE handle,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **entries,UINTN *n){
 EFI_GUID expected=EFI_PCI_IO_PROTOCOL_GUID;assert((handle==(VOID *)2||handle==(VOID *)3)&&!memcmp(guid,&expected,sizeof expected));
 if(failure('o'))return EFI_DEVICE_ERROR;
 *n=open_flags?1:0;*entries=NULL;
 if(*n){*entries=malloc(sizeof **entries);assert(*entries);**entries=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){(VOID *)99,NULL,open_flags,1};}
 return 0;
}
static int cfg32(void *unused,uint32_t offset,uint32_t *out){(void)unused;*out=config[0][offset/4];return 0;}
static void plan_fixture(struct bt_plan *p){memset(p,0,sizeof(*p));
 config[0][0]=0x20c210de;config[1][0]=0x14831022;config[0][1]=7;config[1][1]=7;config[0][2]=0x030200a1;config[1][2]=0x06040001;config[1][3]=0x00010000;
 config[0][4]=0xf3000000;config[0][5]=0xfc00000c;config[0][6]=0x7fb;config[0][7]=0xfa00000c;config[0][8]=0x7fb;
 config[1][6]=0x00010100;config[1][8]=0xf3f0f300;config[1][9]=0xfff1fa01;config[1][10]=0x7fb;config[1][11]=0x7fb;
 config[1][0x50/4]=0x00420010;config[0][0x78/4]=0x00020010;config[0][0x80/4]=0;
 config[0][0x100/4]=(0xbb0U<<20)|0x10001;config[0][0xbb0/4]=0x10015;
 config[0][0xbb4/4]=1U<<8;config[0][0xbb8/4]=(3U<<5)|(4U<<8);
 config[0][0xbbc/4]=1U<<10;config[0][0xbc0/4]=0x2000|(6U<<8)|1;
 config[0][0xbc4/4]=1U<<9;config[0][0xbc8/4]=(5U<<8)|3;
 config[0][0x724/4]=0x10;config[0][0xdcc/4]=2;
 p->apply=1;p->proofs=BT_REQUIRED_PROOFS;p->endpoint=BT_ENDPOINT;p->bridge=BT_BRIDGE;p->pcie_cap=0x78;p->bridge_pcie_cap=0x50;
 p->bar1_base=UINT64_C(0x64000000000);p->bar1_size=BT_BAR1_SIZE;p->bar3_base=p->bar1_base+BT_BAR1_SIZE;p->bar3_size=BT_BAR3_SIZE;
 p->bridge_base=p->bar1_base;p->bridge_limit=p->bar3_base+BT_BAR3_SIZE-1;p->root_start=UINT64_C(0x63d20200000);p->root_end=UINT64_C(0x7fbffffffff);
 p->reservation_base=p->bar1_base;p->reservation_length=BT_SPAN;p->reservation_owner=1;
 memcpy(p->endpoint_before,config[0],64);memcpy(p->bridge_before,config[1],64);p->cya_before=config[0][0x724/4];p->cfg_before=config[0][0xdcc/4];p->rebar_before=decode_rebar(cfg32,NULL);
}

static void setup(void){
 memset(&c,0,sizeof c);memset(&reservation,0,sizeof reservation);memset(&handoff,0,sizeof handoff);
 memset(&bs,0,sizeof bs);memset(&dxe,0,sizeof dxe);memset(&ep,0,sizeof ep);memset(&br,0,sizeof br);memset(config,0,sizeof config);
 ep.GetLocation=br.GetLocation=location;ep.Pci.Read=br.Pci.Read=readcfg;ep.Pci.Write=br.Pci.Write=writecfg;
 plan_fixture(&c.plan);
 c.endpoint=(struct pa_device){(VOID *)2,&ep,BT_ENDPOINT,config[0][0]};
 c.bridge=(struct pa_device){(VOID *)3,&br,BT_BRIDGE,config[1][0]};c.readable=readable;
 bs.FreePool=freepool;bs.OpenProtocolInformation=opens;c.bs=&bs;
 dxe.Hdr.Signature=UINT64_C(0x565245535f455844);dxe.Hdr.HeaderSize=sizeof dxe;
 dxe.GetMemorySpaceMap=getmap;dxe.AllocateMemorySpace=alloc;dxe.FreeMemorySpace=freemem;
 reservation.dxe=&dxe;reservation.bs=&bs;reservation.image=(VOID *)1;reservation.device=(VOID *)2;reservation.readable=readable;
 map_count=1;map[0]=(GR_DESCRIPTOR){.BaseAddress=UINT64_C(0x63d20200000),.Length=UINT64_C(0x1becfe00000),.Capabilities=UINT64_C(0xc000000000026001),.Attributes=1,.GcdMemoryType=3};
 calls=reads=writes=locations=fail_at=fail_allones=open_flags=drift_location=0;
 assert(gr_reserve(&reservation)==GR_OWNED);c.reservation=&reservation;
 handoff.binding.parent=1;handoff.binding.device=77;handoff.child=88;handoff.phase=HF_MUTATING;handoff.status=HF_OK;handoff.mutation_started=1;c.handoff=&handoff;c.ready=1;
 calls=reads=writes=locations=0;
}
static enum bt_status apply(struct bt_state *state){struct bt_io io={&c,pa_read,pa_write,pa_owned};return bt_apply(&c.plan,&io,state);}
int main(void){
 unsigned checks=0;uint32_t v=0;struct bt_state state;
#define CHECK(x) do{checks++;assert(x);}while(0)
 setup();CHECK(pa_read(&c,BT_ENDPOINT,0,4,&v)==4&&v==0x20c210de&&writes==0);
 setup();memset(&state,0,sizeof state);CHECK(apply(&state)==BT_APPLIED&&writes==13&&c.next_write==13);
 CHECK(reservation.pci_attempted&&c.write_attempted&&!c.failed&&!(config[0][1]&6)&&config[1][1]==7);
 CHECK(gr_release(&reservation)==GR_OWNED);
 unsigned total=calls;char successful[4096];memcpy(successful,trace,total);
 for(unsigned at=1;at<=total;at++)for(unsigned ones=0;ones<2;ones++){
  if(ones&&successful[at-1]!='r')continue;
  setup();memset(&state,0,sizeof state);fail_at=at;fail_allones=ones;
  CHECK(apply(&state)!=BT_APPLIED);
  CHECK(!writes||state.status==BT_COLD_RECOVERY_REQUIRED);
  unsigned before_calls=calls,before_writes=writes;CHECK(apply(&state)!=BT_APPLIED&&calls==before_calls&&writes==before_writes);
  CHECK(!writes||reservation.pci_attempted);
 }
 for(unsigned flag=0;flag<32;flag++){
  setup();open_flags=1U<<flag;memset(&state,0,sizeof state);
  CHECK((apply(&state)==BT_APPLIED)==(flag<=2));
  if(flag>2)CHECK(writes==0);
 }
 for(unsigned bad=0;bad<19;bad++){
  setup();memset(&state,0,sizeof state);
  if(bad==0)c.ready=0;
  if(bad==1)handoff.phase=HF_PREPARED;
  if(bad==2)handoff.mutation_started=0;
  if(bad==3)handoff.start_called=1;
  if(bad==4)handoff.status=HF_STATE;
  if(bad==5)map[1].DeviceHandle=(VOID *)9;
  if(bad==6)c.endpoint.handle=(VOID *)9;
  if(bad==7)c.endpoint.bdf=BT_BRIDGE;
  if(bad==8)c.bridge.identity=0x14831023;
  if(bad==9)drift_location=1;
  if(bad==10)c.next_write=1;
  if(bad==11)reservation.allocate_status=EFI_DEVICE_ERROR;
  if(bad==12)bs.OpenProtocolInformation=NULL;
  if(bad==13)handoff.binding.parent=9;
  if(bad==14)handoff.child=0;
  if(bad==15)handoff.child=handoff.binding.parent;
  if(bad==16)handoff.binding.device=0;
  if(bad==17)reservation.bs=NULL;
  if(bad==18)c.bridge.handle=c.endpoint.handle;
  CHECK(apply(&state)!=BT_APPLIED&&writes==0);
 }
 for(unsigned bad=0;bad<8;bad++){
  setup();uint32_t bdf=BT_ENDPOINT,value=1;uint16_t off=4;unsigned width=2;
  if(bad==0)bdf=0x0200;
  if(bad==1)bdf=BT_BRIDGE;
  if(bad==2)off=0x724;
  if(bad==3)width=4;
  if(bad==4)value=7;
  if(bad==5)off=5;
  if(bad==6)c.next_write=13;
  if(bad==7)c.write_busy=1;
  CHECK(pa_write(&c,bdf,off,width,value)==0&&writes==0&&c.failed);
  CHECK(pa_write(&c,BT_ENDPOINT,4,2,1)==0&&writes==0);
 }
 setup();CHECK(pa_read(&c,0x0200,0,4,&v)==0&&calls==0);
 CHECK(pa_read(&c,BT_ENDPOINT,4094,4,&v)==0&&calls==0);
 CHECK(pa_read(&c,BT_ENDPOINT,1,2,&v)==0&&calls==0);
 printf("%u PCI_IO transaction ABI/fault checks PASS (%u success callbacks)\n",checks,total);return 0;
}
