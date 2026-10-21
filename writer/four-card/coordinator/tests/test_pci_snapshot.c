#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pci_snapshot.h"
#include "historical_headers.h"
static struct pi_snapshot before,after;
static EFI_BOOT_SERVICES bs;
static EFI_PCI_IO_PROTOCOL pci[PI_FUNCTIONS];
static UINT32 config[PI_FUNCTIONS][1024];
static unsigned enumerate_count=PI_FUNCTIONS,calls,fail_at,duplicate_handle,alias_protocol,drift_location;
static int readable(void *unused,uintptr_t p,size_t n){(void)unused;return p&&n&&p<=UINTPTR_MAX-n;}
static int fail(void){return ++calls==fail_at;}
static EFI_STATUS EFIAPI enumerate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out){
 EFI_GUID expected=EFI_PCI_IO_PROTOCOL_GUID;assert(type==ByProtocol&&!key&&!memcmp(guid,&expected,sizeof expected));
 if(fail())return EFI_DEVICE_ERROR;
 *n=enumerate_count;*out=malloc(*n*sizeof **out);assert(*out);
 for(unsigned i=0;i<*n;i++)(*out)[i]=(VOID *)(uintptr_t)(i+1);
 if(duplicate_handle)(*out)[1]=(*out)[0];
 return 0;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out){
 EFI_GUID expected=EFI_PCI_IO_PROTOCOL_GUID;assert(!memcmp(guid,&expected,sizeof expected));
 if(fail())return EFI_DEVICE_ERROR;
 unsigned i=(unsigned)(uintptr_t)h-1;assert(i<PI_FUNCTIONS);*out=&pci[alias_protocol?0:i];return 0;
}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *p,UINTN *seg,UINTN *bus,UINTN *dev,UINTN *fn){
 unsigned i=(unsigned)(p-pci);assert(i<PI_FUNCTIONS);if(fail())return EFI_DEVICE_ERROR;
 *seg=drift_location?1:0;*bus=identities[i].bdf>>8;*dev=identities[i].bdf>>3&31;*fn=identities[i].bdf&7;return 0;
}
static EFI_STATUS EFIAPI readcfg(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 offset,UINTN count,VOID *out){
 unsigned i=(unsigned)(p-pci);assert(i<PI_FUNCTIONS&&width==EfiPciIoWidthUint32&&count>0&&count<=16&&offset%4==0&&offset+count*4<=4096);
 if(fail())return EFI_DEVICE_ERROR;
 memcpy(out,&config[i][offset/4],count*4);return 0;
}
static EFI_STATUS EFIAPI writecfg(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN count,VOID *in){
 (void)p;(void)w;(void)off;(void)count;(void)in;assert(0);return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI release(VOID *p){free(p);return fail()?EFI_DEVICE_ERROR:0;}
static void setup(void){
 memset(&before,0,sizeof before);memset(&after,0,sizeof after);memset(&bs,0,sizeof bs);memset(pci,0,sizeof pci);memset(config,0,sizeof config);
 bs.LocateHandleBuffer=enumerate;bs.HandleProtocol=handle;bs.FreePool=release;
 before.bs=after.bs=&bs;before.readable=after.readable=readable;
 for(unsigned i=0;i<PI_FUNCTIONS;i++){
  pci[i].GetLocation=location;pci[i].Pci.Read=readcfg;pci[i].Pci.Write=writecfg;
  memcpy(config[i],historical_headers[i],64);
  /* Explicitly synthetic conventional chains: the old capture omitted them. */
  config[i][1]&=~0x00100000U;config[i][13]&=~255U;
  if(identities[i].identity==0x20c210de){
   config[i][0x100/4]=(0x900U<<20)|0x10001;
   config[i][0x900/4]=0xbb010000;config[i][0xbb0/4]=0x10015;
   config[i][0xbb4/4]=1U<<8;config[i][0xbb8/4]=(3U<<5)|(4U<<8);
   config[i][0xbbc/4]=1U<<10;config[i][0xbc0/4]=(6U<<8)|1;
   config[i][0xbc4/4]=1U<<9;config[i][0xbc8/4]=(5U<<8)|3;
   config[i][0x724/4]=0x10;config[i][0xdcc/4]=2;
  }
 }
 enumerate_count=PI_FUNCTIONS;calls=fail_at=duplicate_handle=alias_protocol=drift_location=0;
}
static void make_plan(struct bt_plan *p){
 memset(p,0,sizeof *p);p->apply=1;p->proofs=BT_REQUIRED_PROOFS;p->endpoint=BT_ENDPOINT;p->bridge=BT_BRIDGE;
 p->bar1_base=GR_BASE;p->bar1_size=BT_BAR1_SIZE;p->bar3_base=GR_BASE+BT_BAR1_SIZE;p->bar3_size=BT_BAR3_SIZE;
 p->bridge_base=GR_BASE;p->bridge_limit=GR_LIMIT;p->root_start=UINT64_C(0x63d20200000);p->root_end=UINT64_C(0x7fbffffffff);
 p->reservation_base=GR_BASE;p->reservation_length=GR_LENGTH;p->reservation_owner=1;p->pcie_cap=0x78;p->bridge_pcie_cap=0x50;
 memcpy(p->endpoint_before,before.functions[before.endpoint_index].header,64);
 memcpy(p->bridge_before,before.functions[before.bridge_index].header,64);
 for(unsigned i=0;i<4;i++)if(before.cmp[i].index==before.endpoint_index){p->cya_before=before.cmp[i].cya;p->cfg_before=before.cmp[i].cfg;p->rebar_before=before.cmp[i].rebar;}
}
static void changed_config(const struct bt_plan *p){
 for(unsigned i=0;i<PI_FUNCTIONS;i++)for(unsigned j=0;j<16;j++)config[i][j]=pi_header_expected(&before.functions[i],j,p,1);
 unsigned i=before.endpoint_index;config[i][0xbbc/4]=BT_CFG_CAP_OBSERVED;config[i][0xbc0/4]=(config[i][0xbc0/4]&~0x1f00U)|(16U<<8);
 config[i][0x724/4]=0x30;config[i][0xdcc/4]=0x8000000a;
}
int main(void){
 unsigned checks=0;struct bt_plan p;
#define CHECK(x) do{checks++;assert(x);}while(0)
 setup();CHECK(pi_capture(&before,identities));CHECK(before.count==112&&before.cmp_count==4);
 unsigned total=calls;CHECK(pi_capture(&after,identities)&&pi_same(&before,&after,NULL,0));
 CHECK(!pi_capture(&before,identities));
 for(unsigned at=1;at<=total;at++){
  setup();fail_at=at;CHECK(!pi_capture(&before,identities)&&before.status==PI_FAILED);
  unsigned prior=calls;CHECK(!pi_capture(&before,identities)&&calls==prior);
 }
 for(unsigned bad=0;bad<8;bad++){
  setup();
  if(bad==0)enumerate_count=111;
  if(bad==1)duplicate_handle=1;
  if(bad==2)alias_protocol=1;
  if(bad==3)drift_location=1;
  if(bad==4)config[0][0]^=1;
  if(bad==5)config[0][3]|=2U<<16;
  if(bad==6){config[0][1]|=0x00100000;config[0][13]=0x40;config[0][0x40/4]=0x14;}
  if(bad==7)config[0][1]=UINT32_MAX;
  CHECK(!pi_capture(&before,identities));
 }
 for(unsigned index=0;index<PI_FUNCTIONS;index++){
  setup();CHECK(pi_capture(&before,identities));config[index][15]^=1;
  CHECK(pi_capture(&after,identities)&&!pi_same(&before,&after,NULL,0));
 }
 setup();CHECK(pi_capture(&before,identities));make_plan(&p);changed_config(&p);
 CHECK(pi_capture(&after,identities)&&pi_same(&before,&after,&p,1));
 CHECK(!pi_same(&before,&after,NULL,0));
 for(unsigned i=0;i<4;i++)for(unsigned field=0;field<4;field++){
  setup();CHECK(pi_capture(&before,identities));make_plan(&p);changed_config(&p);
  unsigned index=before.cmp[i].index;if(index==before.endpoint_index)continue;
  if(field==0)config[index][5]^=0x10000000;
  if(field==1)config[index][0x724/4]^=1;
  if(field==2)config[index][0xdcc/4]^=1;
  if(field==3)config[index][0xbbc/4]=BT_CFG_CAP_OBSERVED;
  CHECK(pi_capture(&after,identities)&&!pi_same(&before,&after,&p,1));
 }
 setup();CHECK(pi_capture(&before,identities));CHECK(pi_capture(&after,identities));
 before.endpoint_index=PI_FUNCTIONS;CHECK(!pi_same(&before,&after,NULL,0));
 printf("%u full PCI snapshot/preservation checks PASS (%u successful EFI calls)\n",checks,total);return 0;
}
