#ifndef CMP_PCI_ACCESS_H
#define CMP_PCI_ACCESS_H
#include "gcd_reservation.h"
#include "../pins/bar_transaction.h"
#include "../pins/handoff.h"
struct pa_device {EFI_HANDLE handle;EFI_PCI_IO_PROTOCOL *pci;uint32_t bdf,identity;};
enum pa_failure {PA_FAILURE_NONE,PA_FAILURE_ARGUMENT,PA_FAILURE_PROTOCOL,
 PA_FAILURE_LOCATION,PA_FAILURE_IDENTITY,PA_FAILURE_READ,PA_FAILURE_ALL_ONES,
 PA_FAILURE_OPEN_INFO,PA_FAILURE_DRIVER,PA_FAILURE_POOL,PA_FAILURE_OWNERSHIP,
 PA_FAILURE_WRITE_GUARD,PA_FAILURE_WRITE};
struct pa_diagnostic {
 unsigned reason,bdf,offset,width,expected_valid,expected,observed_valid,observed,status_valid;
 EFI_STATUS status;
};
struct pa_pair_state;
struct pa_context {
 struct pa_device endpoint,bridge;
 EFI_BOOT_SERVICES *bs;struct gr_state *reservation;struct hf_session *handoff;
 struct bt_plan plan;
 struct pa_pair_state *pair;unsigned pair_slot;
 int (*readable)(void *,uintptr_t,size_t);void *read_context;
 unsigned ready,failed,next_write,write_attempted,write_busy;
 EFI_STATUS last_status;uint32_t last_bdf,last_offset,last_width;
 struct pa_diagnostic failure;
};
/* Access authority is local CPU state, never a hardware isolation claim.
 * Task4 owns these objects and calls bind once, then pa_apply in slot order. */
struct pa_pair_state {
 struct gr_pair_state *reservation;struct hf_session *handoff;
 struct pa_context *contexts[PT_COUNT];struct bt_plan plans[PT_COUNT];struct pa_device devices[PT_COUNT][2];
 struct hf_binding binding;uintptr_t child;
 unsigned bound,failed,attempted,completed_mask,apply_busy,store_busy;
};
static int pa_failure(struct pa_context *c,unsigned reason,uint32_t bdf,uint32_t off,unsigned width,
 unsigned expected_valid,uint32_t expected,unsigned observed_valid,uint32_t observed,
 unsigned status_valid,EFI_STATUS status){
 if(c&&c->pair){c->failed=1;c->pair->failed=1;}
 if(c&&!c->failure.reason)c->failure=(struct pa_diagnostic){reason,bdf,off,width,
  expected_valid,expected,observed_valid,observed,status_valid,status};
 return 0;
}
static int pa_range(struct pa_context *c,const void *p,size_t n){
 return c&&c->readable&&p&&n&&(uintptr_t)p<=UINTPTR_MAX-n&&c->readable(c->read_context,(uintptr_t)p,n);
}
static struct pa_device *pa_device(struct pa_context *c,uint32_t bdf){
 if(!c)return NULL;
 if(bdf==c->plan.endpoint)return &c->endpoint;
 if(bdf==c->plan.bridge)return &c->bridge;
 return NULL;
}
static int pa_location(struct pa_context *c,struct pa_device *d){
 if(!d||!d->handle||!pa_range(c,d->pci,sizeof *d->pci)||!d->pci->GetLocation||!d->pci->Pci.Read||!d->pci->Pci.Write)
  return pa_failure(c,PA_FAILURE_PROTOCOL,d?d->bdf:0,0,0,0,0,0,0,0,0);
 UINTN seg=UINT64_MAX,bus=UINT64_MAX,dev=UINT64_MAX,fn=UINT64_MAX;
 c->last_status=uefi_call_wrapper(d->pci->GetLocation,5,d->pci,&seg,&bus,&dev,&fn);
 if(c->last_status||seg||bus>255||dev>31||fn>7||(bus<<8|dev<<3|fn)!=d->bdf)
  return pa_failure(c,PA_FAILURE_LOCATION,d->bdf,0,0,1,d->bdf,
   !c->last_status&&!seg&&bus<=255&&dev<=31&&fn<=7,(uint32_t)(bus<<8|dev<<3|fn),1,c->last_status);
 uint32_t expected=d->bdf==c->plan.endpoint?0x20c210deU:d->bdf==c->plan.bridge?0x14831022U:0;
 UINT32 identity=UINT32_MAX;
 c->last_status=uefi_call_wrapper(d->pci->Pci.Read,5,d->pci,EfiPciIoWidthUint32,0,1,&identity);
 if(c->last_status||!expected||d->identity!=expected||identity!=expected)
  return pa_failure(c,PA_FAILURE_IDENTITY,d->bdf,0,4,1,expected,!c->last_status,identity,1,c->last_status);
 return 1;
}
static int pa_read(void *context,uint32_t bdf,uint16_t offset,unsigned width,uint32_t *value){
 struct pa_context *c=context;struct pa_device *d=pa_device(c,bdf);
 if(!c||c->failed||(c->pair&&c->pair->failed)||!value||!d||d->bdf!=bdf||(width!=2&&width!=4)||
    offset>4096-width||(offset&(width-1)))return pa_failure(c,PA_FAILURE_ARGUMENT,bdf,offset,width,0,0,0,0,0,0);
 if(!pa_location(c,d))return 0;
 c->last_bdf=bdf;c->last_offset=offset;c->last_width=width;
 UINT32 dword=UINT32_MAX;UINT16 word=UINT16_MAX;
 c->last_status=uefi_call_wrapper(d->pci->Pci.Read,5,d->pci,
  width==2?EfiPciIoWidthUint16:EfiPciIoWidthUint32,offset,1,width==2?(VOID *)&word:(VOID *)&dword);
 if(c->last_status)return pa_failure(c,PA_FAILURE_READ,bdf,offset,width,0,0,0,0,1,c->last_status);
 if(width==2?word==UINT16_MAX:dword==UINT32_MAX)
  return pa_failure(c,PA_FAILURE_ALL_ONES,bdf,offset,width,0,0,1,width==2?word:dword,1,c->last_status);
 *value=width==2?word:dword;return (int)width;
}
/* No DisconnectController is permitted. A managing/child/exclusive open
 * refuses the transaction rather than guessing that its driver is idle. */
static int pa_no_driver(struct pa_context *c,struct pa_device *d){
 if(!pa_range(c,c->bs,sizeof *c->bs)||!c->bs->OpenProtocolInformation||!c->bs->FreePool)
  return pa_failure(c,PA_FAILURE_PROTOCOL,d?d->bdf:0,0,0,0,0,0,0,0,0);
 if(!pa_location(c,d))return 0;
 EFI_GUID guid=EFI_PCI_IO_PROTOCOL_GUID;EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN n=0;
 c->last_status=uefi_call_wrapper(c->bs->OpenProtocolInformation,4,d->handle,&guid,&entries,&n);
 if(c->last_status)return pa_failure(c,PA_FAILURE_OPEN_INFO,d->bdf,0,0,0,0,0,0,1,c->last_status);
 int valid=n<=64&&(!n||pa_range(c,entries,n*sizeof *entries));
 if(valid)for(UINTN i=0;i<n;i++){
  UINT32 flags=entries[i].Attributes;
  if(!entries[i].AgentHandle||!entries[i].OpenCount||!flags||
     (flags&~(EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL|EFI_OPEN_PROTOCOL_GET_PROTOCOL|EFI_OPEN_PROTOCOL_TEST_PROTOCOL))){valid=0;break;}
 }
 if(!valid)pa_failure(c,PA_FAILURE_DRIVER,d->bdf,0,0,0,0,0,0,0,0);
 if(entries){c->last_status=uefi_call_wrapper(c->bs->FreePool,1,entries);if(c->last_status){
  valid=0;pa_failure(c,PA_FAILURE_POOL,d->bdf,0,0,0,0,0,0,1,c->last_status);}}
 return valid;
}
static int pa_handle_matches(struct pa_context *c,struct pa_device *d){
 if(!pa_range(c,c->bs,sizeof *c->bs)||!c->bs->HandleProtocol)return 0;
 EFI_GUID guid=EFI_PCI_IO_PROTOCOL_GUID;EFI_PCI_IO_PROTOCOL *pci=NULL;
 c->last_status=uefi_call_wrapper(c->bs->HandleProtocol,3,d->handle,&guid,(VOID **)&pci);
 return !c->last_status&&pci==d->pci&&pa_location(c,d);
}
static int pa_pair_authority(struct pa_pair_state *p){
 if(!p||!p->bound||p->failed||!p->reservation||!p->handoff||
  memcmp(&p->binding,&p->handoff->binding,sizeof p->binding)||p->child!=p->handoff->child)return 0;
 for(unsigned t=0;t<PT_COUNT;t++){
  struct pa_context *c=p->contexts[t];
  if(!c||c->pair!=p||c->pair_slot!=t||c->failed||!c->ready||c->handoff!=p->handoff||
   c->bs!=p->reservation->bs||memcmp(&c->plan,&p->plans[t],sizeof c->plan)||
   memcmp(&c->endpoint,&p->devices[t][0],sizeof c->endpoint)||memcmp(&c->bridge,&p->devices[t][1],sizeof c->bridge)||
   c->plan.target_index!=t||!bt_plan_valid(&c->plan)||
   c->plan.reservation_owner!=(uintptr_t)p->reservation->image||
   p->reservation->slot[t].device!=c->endpoint.handle||
   p->reservation->slot[t].base!=c->plan.reservation_base||p->reservation->slot[t].length!=c->plan.reservation_length||
   c->endpoint.bdf!=c->plan.endpoint||c->bridge.bdf!=c->plan.bridge||
   !pa_handle_matches(c,&c->endpoint)||!pa_handle_matches(c,&c->bridge))return 0;
 }
 return !p->failed&&gr_pair_check_owned(p->reservation);
}
static int pa_pair_bind(struct pa_pair_state *p,struct pa_context contexts[PT_COUNT]){
 if(!p)return 0;
 if(p->bound||p->failed||p->attempted||p->completed_mask||p->apply_busy||p->store_busy||!contexts||
  !p->reservation||!p->handoff||p->reservation->pci_attempted){p->failed=1;return 0;}
 p->bound=1;memcpy(&p->binding,&p->handoff->binding,sizeof p->binding);p->child=p->handoff->child;
 for(unsigned t=0;t<PT_COUNT;t++){
  struct pa_context *c=&contexts[t];
  if(c->pair||c->next_write||c->write_attempted||c->write_busy||c->failed){p->failed=1;return 0;}
  p->contexts[t]=c;memcpy(&p->plans[t],&c->plan,sizeof c->plan);p->devices[t][0]=c->endpoint;p->devices[t][1]=c->bridge;
  c->pair=p;c->pair_slot=t;
 }
 for(unsigned i=0;i<2*PT_COUNT;i++)for(unsigned j=0;j<i;j++)if(
  p->devices[i/2][i%2].handle==p->devices[j/2][j%2].handle||
  p->devices[i/2][i%2].pci==p->devices[j/2][j%2].pci){p->failed=1;return 0;}
 if(!pa_pair_authority(p)){p->failed=1;return 0;}
 return 1;
}
static int pa_owned(void *context,const struct bt_plan *plan){
 struct pa_context *c=context;
 if(c&&c->pair){
  int okay=plan==&c->plan&&c->pair_slot<PT_COUNT&&c->pair->contexts[c->pair_slot]==c&&pa_pair_authority(c->pair);
  return okay?1:pa_failure(c,PA_FAILURE_OWNERSHIP,0,0,0,0,0,0,0,0,0);
 }
 int okay=c&&c->ready&&!c->failed&&plan==&c->plan&&c->reservation&&
  c->reservation->status==GR_OWNED&&!c->reservation->allocate_status&&
  c->reservation->image==(EFI_HANDLE)(uintptr_t)plan->reservation_owner&&
  c->reservation->device==c->endpoint.handle&&plan->reservation_base==GR_BASE&&
  plan->reservation_length==GR_LENGTH&&gr_check_owned(c->reservation);
 return okay?1:pa_failure(c,PA_FAILURE_OWNERSHIP,0,0,0,0,0,0,0,0,0);
}
static int pa_expected_write(const struct pa_context *c,uint32_t bdf,uint16_t offset,unsigned width,uint32_t value){
 const struct bt_plan *p=&c->plan;uint32_t ebdf=p->endpoint,off=4,w=4,want=0;
 uint32_t ctrl=0;int found=0;
 for(uint32_t i=0;i<p->rebar_before.count&&i<6;i++)if((p->rebar_before.entries[i].control&7U)==1){ctrl=p->rebar_before.entries[i].control;found++;}
 if(found!=1)return 0;
 switch(c->next_write){
  case 0:w=2;want=(p->endpoint_before[1]&0xffffU)&~6U;break;
  case 1:ebdf=p->bridge;w=2;want=(p->bridge_before[1]&0xffffU)&~6U;break;
  case 2:off=0x724;want=0x30;break;
  case 3:off=0xdcc;want=0x8000000aU;break;
  case 4:off=0xbc0;want=(ctrl&~0x1f00U)|(16U<<8);break;
  case 5:off=0x14;want=(uint32_t)p->bar1_base|12U;break;
  case 6:off=0x18;want=(uint32_t)(p->bar1_base>>32);break;
  case 7:off=0x1c;want=(uint32_t)p->bar3_base|12U;break;
  case 8:off=0x20;want=(uint32_t)(p->bar3_base>>32);break;
  case 9:ebdf=p->bridge;off=0x24;want=(uint32_t)((p->bridge_base>>16)&0xfff0U)|1U|(((uint32_t)((p->bridge_limit>>16)&0xfff0U)|1U)<<16);break;
  case 10:ebdf=p->bridge;off=0x28;want=(uint32_t)(p->bridge_base>>32);break;
  case 11:ebdf=p->bridge;off=0x2c;want=(uint32_t)(p->bridge_limit>>32);break;
  case 12:ebdf=p->bridge;w=2;want=p->bridge_before[1]&0xffffU;break;
  default:return 0;
 }
 return bdf==ebdf&&offset==off&&width==w&&value==want;
}
static int pa_write(void *context,uint32_t bdf,uint16_t offset,unsigned width,uint32_t value){
 struct pa_context *c=context;struct pa_device *d=pa_device(c,bdf);int okay=0;
 if(!c)return 0;
 if(c->failed||c->write_busy||(c->pair&&(c->pair->failed||c->pair->store_busy))){c->failed=1;return pa_failure(c,PA_FAILURE_WRITE_GUARD,bdf,offset,width,1,value,0,0,0,0);}
 c->write_busy=1;
 if(c->pair)c->pair->store_busy=1;
 EFI_HANDLE image=c->pair?(c->pair->reservation?c->pair->reservation->image:NULL):(c->reservation?c->reservation->image:NULL);
 if(c->pair&&(c->pair_slot>=PT_COUNT||c->pair->completed_mask!=((1U<<c->pair_slot)-1U)||
  c->pair->attempted!=c->pair_slot*13+c->next_write))goto done;
 if(!d||!c->ready||!c->handoff||(!c->pair&&(!c->reservation||c->reservation->bs!=c->bs))||
    c->endpoint.handle==c->bridge.handle||c->endpoint.pci==c->bridge.pci||
    c->handoff->binding.parent!=(uintptr_t)image||
    !c->handoff->binding.device||!c->handoff->child||
    c->handoff->child==c->handoff->binding.parent||c->handoff->child==c->handoff->binding.device||
    c->handoff->phase!=HF_MUTATING||
    c->handoff->status!=HF_OK||c->handoff->mutation_started!=1||c->handoff->start_called||
    !bt_plan_valid(&c->plan)||!pa_expected_write(c,bdf,offset,width,value)||
    !pa_owned(c,&c->plan)||!pa_no_driver(c,&c->endpoint)||!pa_no_driver(c,&c->bridge))goto done;
 uint32_t command=0;
 if(c->next_write&&(!pa_read(c,c->plan.endpoint,4,2,&command)||(command&6U)))goto done;
 if(c->next_write>=2&&(!pa_read(c,c->plan.bridge,4,2,&command)||(command&6U)))goto done;
 if(!pa_location(c,d))goto done;
 if(c->pair&&c->pair->failed)goto done;
 c->write_attempted=1;c->next_write++;
 if(c->pair){c->pair->reservation->pci_attempted=1;c->pair->attempted++;}
 else c->reservation->pci_attempted=1;
 c->last_bdf=bdf;c->last_offset=offset;c->last_width=width;
 UINT32 dword=value;UINT16 word=(UINT16)value;
 c->last_status=uefi_call_wrapper(d->pci->Pci.Write,5,d->pci,
  width==2?EfiPciIoWidthUint16:EfiPciIoWidthUint32,offset,1,width==2?(VOID *)&word:(VOID *)&dword);
 okay=!c->last_status&&!c->failed&&(!c->pair||!c->pair->failed);
 if(!okay)pa_failure(c,PA_FAILURE_WRITE,bdf,offset,width,1,value,0,0,1,c->last_status);
done:
 c->write_busy=0;if(c->pair)c->pair->store_busy=0;
 if(!okay){c->failed=1;pa_failure(c,PA_FAILURE_WRITE_GUARD,bdf,offset,width,1,value,0,0,0,0);}
 return okay?(int)width:0;
}
/* The only pair completion boundary. A13th store alone cannot grant the next slot. */
static enum bt_status pa_apply(struct pa_context *c,struct bt_state *state){
 struct pa_pair_state *p=c?c->pair:NULL;
 if(!p)return BT_REFUSED;
 if(p->failed||p->apply_busy||!state||state->status!=BT_NEW||c->pair_slot>=PT_COUNT||
  p->contexts[c->pair_slot]!=c||p->completed_mask!=((1U<<c->pair_slot)-1U)||
  c->next_write||p->attempted!=c->pair_slot*13){p->failed=1;return p->attempted?BT_COLD_RECOVERY_REQUIRED:BT_REFUSED;}
 p->apply_busy=1;
 struct bt_io io={c,pa_read,pa_write,pa_owned};enum bt_status result=bt_apply(&c->plan,&io,state);
 p->apply_busy=0;
 if(result!=BT_APPLIED||p->failed||c->next_write!=13||p->attempted!=(c->pair_slot+1)*13){
  p->failed=1;state->status=p->attempted?BT_COLD_RECOVERY_REQUIRED:BT_REFUSED;return state->status;
 }
 p->completed_mask|=1U<<c->pair_slot;return BT_APPLIED;
}
#endif
