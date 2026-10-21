#ifndef CMP_PCI_SNAPSHOT_H
#define CMP_PCI_SNAPSHOT_H
#include "pci_access.h"
#include "../pins/pci_conventional.h"
#include "../pins/resource_types.h"
#define PI_FUNCTIONS 112U
#define PI_CONVENTIONAL_READS 104U
#define PI_EXTENDED_READS 1024U
struct pi_identity {uint32_t bdf,identity;};
struct pi_function {
 struct pa_device device;uint32_t header[16];uint16_t header_present;uint8_t raw_slot_present;
 struct pci_conv_result conventional;uint32_t conventional_count;
 struct rc_read conventional_reads[PI_CONVENTIONAL_READS];
};
struct pi_cmp {
 uint32_t index,cya,cfg,extended_count;struct cap_result rebar;
 struct rc_read extended_reads[PI_EXTENDED_READS];
};
enum pi_status {PI_NEW,PI_FAILED,PI_PCI_CAPTURED};
struct pi_snapshot {
 EFI_BOOT_SERVICES *bs;int (*readable)(void *,uintptr_t,size_t);void *read_context;
 enum pi_status status;uint32_t count,cmp_count,endpoint_index,bridge_index;
 uint32_t endpoint_indices[PT_COUNT],bridge_indices[PT_COUNT];
 struct pi_function functions[PI_FUNCTIONS];struct pi_cmp cmp[4];
 EFI_STATUS last_status;uint32_t last_index,last_offset;
};
static int pi_range(struct pi_snapshot *s,const void *p,size_t n){
 return s&&s->readable&&p&&n&&(uintptr_t)p<=UINTPTR_MAX-n&&s->readable(s->read_context,(uintptr_t)p,n);
}
struct pi_trace_context {struct pi_snapshot *snapshot;EFI_PCI_IO_PROTOCOL *pci;struct rc_read *trace;uint32_t *count,maximum;};
static int pi_trace_read(void *context,uint32_t offset,uint32_t *value){
 struct pi_trace_context *c=context;struct pi_snapshot *s=c->snapshot;
 if(!value||offset>4092||(offset&3)||*c->count>=c->maximum)return 1;
 UINT32 raw=UINT32_MAX;s->last_offset=offset;
 s->last_status=uefi_call_wrapper(c->pci->Pci.Read,5,c->pci,EfiPciIoWidthUint32,offset,1,&raw);
 c->trace[(*c->count)++]=(struct rc_read){offset,raw,s->last_status};
 if(s->last_status||raw==UINT32_MAX)return 1;
 *value=raw;return 0;
}
static int pi_config(struct pi_snapshot *s,EFI_PCI_IO_PROTOCOL *pci,uint32_t offset,uint32_t *out){
 UINT32 raw=UINT32_MAX;s->last_offset=offset;
 s->last_status=uefi_call_wrapper(pci->Pci.Read,5,pci,EfiPciIoWidthUint32,offset,1,&raw);
 if(s->last_status||raw==UINT32_MAX)return 0;
 *out=raw;return 1;
}
static int pi_location(struct pi_snapshot *s,EFI_PCI_IO_PROTOCOL *pci,uint32_t *bdf){
 UINTN seg=UINT64_MAX,bus=UINT64_MAX,dev=UINT64_MAX,fn=UINT64_MAX;
 s->last_status=uefi_call_wrapper(pci->GetLocation,5,pci,&seg,&bus,&dev,&fn);
 if(s->last_status||seg||bus>255||dev>31||fn>7)return 0;
 *bdf=(uint32_t)(bus<<8|dev<<3|fn);return 1;
}
/* Same-boot observations only. This is not the full resource collision/root
 * proof: raw query lists, memory/ACPI observations and reviewed policy are a
 * separate required input. No caller can set PI_PCI_CAPTURED via this API. */
static int pi_capture(struct pi_snapshot *s,const struct pi_identity expected[PI_FUNCTIONS]){
 if(!s||s->status!=PI_NEW)return 0;
 s->status=PI_FAILED;
 if(!pi_range(s,expected,PI_FUNCTIONS*sizeof *expected)||!pi_range(s,s->bs,sizeof *s->bs)||!s->bs->LocateHandleBuffer||!s->bs->HandleProtocol||!s->bs->FreePool)return 0;
 for(unsigned i=0;i<PI_FUNCTIONS;i++)if(expected[i].bdf>0xffff||!expected[i].identity||
    (expected[i].identity&0xffff)==0xffff||(expected[i].identity>>16)==0xffff||
    (i&&expected[i-1].bdf>=expected[i].bdf))return 0;
 EFI_GUID guid=EFI_PCI_IO_PROTOCOL_GUID;EFI_HANDLE *handles=NULL;UINTN n=0;unsigned seen[PI_FUNCTIONS]={0};int okay=0;
 s->last_status=uefi_call_wrapper(s->bs->LocateHandleBuffer,5,ByProtocol,&guid,NULL,&n,&handles);
 if(s->last_status)return 0;
 if(n!=PI_FUNCTIONS||!pi_range(s,handles,n*sizeof *handles))goto done;
 for(UINTN h=0;h<n;h++){
  if(!handles[h])goto done;
  for(UINTN j=0;j<h;j++)if(handles[j]==handles[h])goto done;
  EFI_PCI_IO_PROTOCOL *pci=NULL;
  s->last_status=uefi_call_wrapper(s->bs->HandleProtocol,3,handles[h],&guid,(VOID **)&pci);
  if(s->last_status||!pi_range(s,pci,sizeof *pci)||!pci->GetLocation||!pci->Pci.Read||!pci->Pci.Write)goto done;
  uint32_t bdf=0;if(!pi_location(s,pci,&bdf))goto done;
  unsigned index=PI_FUNCTIONS;
  for(unsigned i=0;i<PI_FUNCTIONS;i++)if(expected[i].bdf==bdf){index=i;break;}
  if(index==PI_FUNCTIONS||seen[index])goto done;
  for(unsigned i=0;i<PI_FUNCTIONS;i++)if(seen[i]&&s->functions[i].device.pci==pci)goto done;
  struct pi_function *f=&s->functions[index];s->last_index=index;
  f->device=(struct pa_device){handles[h],pci,bdf,expected[index].identity};
  for(unsigned i=0;i<16;i++){
   if(!pi_config(s,pci,i*4,&f->header[i]))goto done;
   f->header_present|=(uint16_t)(1U<<i);
   unsigned type=(f->header[3]>>16)&0x7f;
   if(i>=4&&i<(type==0?10U:type==1?6U:4U))f->raw_slot_present|=(uint8_t)(1U<<(i-4));
  }
  if(f->header[0]!=expected[index].identity||((f->header[3]>>16)&0x7f)>1)goto done;
  struct pi_trace_context cc={s,pci,f->conventional_reads,&f->conventional_count,PI_CONVENTIONAL_READS};
  f->conventional=pci_conventional_walk(pi_trace_read,&cc);
  if(f->conventional.status!=PCI_CONV_NO_EA||f->conventional.initial_valid!=15||
     f->conventional_count!=f->conventional.read_count||f->conventional.identity_raw!=f->header[0]||
     f->conventional.command_status_raw!=f->header[1]||f->conventional.header_raw!=f->header[3]||
     f->conventional.pointer_raw!=f->header[13])goto done;
  if(f->header[0]==0x20c210deU){
   if(s->cmp_count>=4||(bdf!=0x0100&&bdf!=0x0200&&bdf!=0x4700&&bdf!=0x4800))goto done;
   struct pi_cmp *cmp=&s->cmp[s->cmp_count++];cmp->index=index;
   cc=(struct pi_trace_context){s,pci,cmp->extended_reads,&cmp->extended_count,PI_EXTENDED_READS};
   cmp->rebar=decode_rebar(pi_trace_read,&cc);
   if(cmp->rebar.status!=CAP_FOUND||cmp->rebar.offset!=0xbb0||cmp->rebar.count!=3||
      !pi_config(s,pci,0x724,&cmp->cya)||!pi_config(s,pci,0xdcc,&cmp->cfg))goto done;
  }
  uint32_t after_bdf=0;if(!pi_location(s,pci,&after_bdf)||after_bdf!=bdf)goto done;
  if(bdf==BT_ENDPOINT)s->endpoint_index=index;
  if(bdf==BT_BRIDGE)s->bridge_index=index;
  for(unsigned t=0;t<PT_COUNT;t++){
   if(bdf==pair_targets[t].endpoint)s->endpoint_indices[t]=index;
   if(bdf==pair_targets[t].port)s->bridge_indices[t]=index;
  }
  seen[index]=1;s->count++;
 }
 okay=s->count==PI_FUNCTIONS&&s->cmp_count==4&&s->functions[s->endpoint_index].device.bdf==BT_ENDPOINT&&
  s->functions[s->bridge_index].device.bdf==BT_BRIDGE;
done:
 if(handles){s->last_status=uefi_call_wrapper(s->bs->FreePool,1,handles);if(s->last_status)okay=0;}
 if(okay)s->status=PI_PCI_CAPTURED;
 return okay;
}
static uint32_t pi_header_expected(const struct pi_function *f,unsigned index,const struct bt_plan *p,int changed){
 uint32_t v=f->header[index];if(!changed)return v;
 if(f->device.bdf==p->endpoint){
  if(index==1)v&=~6U;
  if(index==5)v=(uint32_t)p->bar1_base|12U;
  if(index==6)v=(uint32_t)(p->bar1_base>>32);
  if(index==7)v=(uint32_t)p->bar3_base|12U;
  if(index==8)v=(uint32_t)(p->bar3_base>>32);
 }else if(f->device.bdf==p->bridge){
  if(index==9)v=(uint32_t)((p->bridge_base>>16)&0xfff0U)|1U|(((uint32_t)((p->bridge_limit>>16)&0xfff0U)|1U)<<16);
  if(index==10)v=(uint32_t)(p->bridge_base>>32);
  if(index==11)v=(uint32_t)(p->bridge_limit>>32);
 }
 return v;
}
static int pi_same(const struct pi_snapshot *before,const struct pi_snapshot *after,const struct bt_plan *plan,int changed){
 if(!before||!after||before==after||before->status!=PI_PCI_CAPTURED||after->status!=PI_PCI_CAPTURED||
    before->bs!=after->bs||before->count!=PI_FUNCTIONS||after->count!=PI_FUNCTIONS||before->cmp_count!=4||after->cmp_count!=4||
    before->endpoint_index>=PI_FUNCTIONS||before->bridge_index>=PI_FUNCTIONS||
    after->endpoint_index!=before->endpoint_index||after->bridge_index!=before->bridge_index||
    (changed!=0&&changed!=1)||(changed&&(!plan||!bt_plan_valid(plan))))return 0;
 if(changed)for(unsigned i=0;i<16;i++)if(before->functions[before->endpoint_index].header[i]!=plan->endpoint_before[i]||
    before->functions[before->bridge_index].header[i]!=plan->bridge_before[i])return 0;
 for(unsigned i=0;i<PI_FUNCTIONS;i++){
  const struct pi_function *a=&before->functions[i],*b=&after->functions[i];
  if(a->device.handle!=b->device.handle||a->device.pci!=b->device.pci||a->device.bdf!=b->device.bdf||a->device.identity!=b->device.identity||
     a->conventional_count!=b->conventional_count||a->conventional_count>PI_CONVENTIONAL_READS)return 0;
  for(unsigned j=0;j<16;j++)if(pi_header_expected(a,j,plan,changed)!=b->header[j])return 0;
  for(unsigned j=0;j<a->conventional_count;j++){
   const struct rc_read *x=&a->conventional_reads[j],*y=&b->conventional_reads[j];uint32_t value=x->value;
   if(changed&&x->offset==4&&a->device.bdf==BT_ENDPOINT)value&=~6U;
   if(x->offset!=y->offset||x->status||y->status||value!=y->value)return 0;
  }
 }
 for(unsigned i=0;i<4;i++){
  const struct pi_cmp *a=&before->cmp[i],*b=NULL;
  for(unsigned j=0;j<4;j++)if(after->cmp[j].index==a->index){if(b)return 0;b=&after->cmp[j];}
  if(!b||a->index>=PI_FUNCTIONS||a->extended_count!=b->extended_count||a->extended_count>PI_EXTENDED_READS)return 0;
  int target=changed&&before->functions[a->index].device.bdf==BT_ENDPOINT;
  if(b->cya!=(target?0x30:a->cya)||b->cfg!=(target?0x8000000aU:a->cfg))return 0;
  for(unsigned j=0;j<a->extended_count;j++){
   const struct rc_read *x=&a->extended_reads[j],*y=&b->extended_reads[j];uint32_t value=x->value;
   if(target&&x->offset==0xbbc)value=BT_CFG_CAP_OBSERVED;
   if(target&&x->offset==0xbc0)value=(value&~0x1f00U)|(16U<<8);
   if(x->offset!=y->offset||x->status||y->status||value!=y->value)return 0;
  }
 }
 return 1;
}
static int pi_pair_complete(const struct pi_snapshot *s){
 if(!s||s->status!=PI_PCI_CAPTURED||s->count!=PI_FUNCTIONS||s->cmp_count!=4)return 0;
 for(unsigned t=0;t<PT_COUNT;t++)if(s->endpoint_indices[t]>=PI_FUNCTIONS||s->bridge_indices[t]>=PI_FUNCTIONS||
  s->functions[s->endpoint_indices[t]].device.bdf!=pair_targets[t].endpoint||
  s->functions[s->bridge_indices[t]].device.bdf!=pair_targets[t].port)return 0;
 if(s->endpoint_index!=s->endpoint_indices[0]||s->bridge_index!=s->bridge_indices[0])return 0;
 for(unsigned i=0;i<PI_FUNCTIONS;i++){
  const struct pi_function *f=&s->functions[i];unsigned type=(f->header[3]>>16)&127;
  if(type>1||f->header_present!=0xffff||f->raw_slot_present!=(type?3:63)||
   !f->device.handle||!f->device.pci||f->device.identity!=f->header[0]||
   (i&&s->functions[i-1].device.bdf>=f->device.bdf)||
   f->conventional.status!=PCI_CONV_NO_EA||f->conventional.initial_valid!=15||
   f->conventional_count!=f->conventional.read_count||!f->conventional_count||
   f->conventional_count>PI_CONVENTIONAL_READS)return 0;
 }
 unsigned cmps=0;
 for(unsigned i=0;i<4;i++){
  const struct pi_cmp *c=&s->cmp[i];if(c->index>=PI_FUNCTIONS||!c->extended_count||c->extended_count>PI_EXTENDED_READS||
   c->rebar.status!=CAP_FOUND||c->rebar.offset!=0xbb0||c->rebar.count!=3)return 0;
  uint32_t bdf=s->functions[c->index].device.bdf;unsigned bit=bdf==0x100?1:bdf==0x200?2:bdf==0x4700?4:bdf==0x4800?8:0;
  if(!bit||(cmps&bit))return 0;
  cmps|=bit;
 }
 return cmps==15;
}
static int pi_same_pair(const struct pi_snapshot *before,const struct pi_snapshot *after,
 const struct bt_plan plans[PT_COUNT],unsigned applied_mask){
 if(!plans||before==after||!pair_prefix_mask(applied_mask)||
  !pi_pair_complete(before)||!pi_pair_complete(after)||before->bs!=after->bs)return 0;
 for(unsigned t=0;t<PT_COUNT;t++){
  const struct bt_plan *p=&plans[t];
  if(p->target_index!=t||!bt_plan_valid(p)||before->endpoint_indices[t]!=after->endpoint_indices[t]||
   before->bridge_indices[t]!=after->bridge_indices[t])return 0;
  for(unsigned j=0;j<16;j++)if(before->functions[before->endpoint_indices[t]].header[j]!=p->endpoint_before[j]||
   before->functions[before->bridge_indices[t]].header[j]!=p->bridge_before[j])return 0;
  for(unsigned j=0;j<4;j++)if(before->cmp[j].index==before->endpoint_indices[t]){
   if(before->cmp[j].cya!=p->cya_before||before->cmp[j].cfg!=p->cfg_before||
    !bt_same_rebar(&before->cmp[j].rebar,&p->rebar_before))return 0;
  }
 }
 for(unsigned i=0;i<PI_FUNCTIONS;i++){
  const struct pi_function *a=&before->functions[i],*b=&after->functions[i];
  const struct bt_plan *p=NULL;
  for(unsigned t=0;t<PT_COUNT;t++)if((applied_mask&(1U<<t))&&(a->device.bdf==plans[t].endpoint||a->device.bdf==plans[t].bridge))p=&plans[t];
  if(a->device.handle!=b->device.handle||a->device.pci!=b->device.pci||a->device.bdf!=b->device.bdf||
   a->device.identity!=b->device.identity||a->conventional_count!=b->conventional_count)return 0;
  for(unsigned j=0;j<16;j++)if(pi_header_expected(a,j,p,p!=NULL)!=b->header[j])return 0;
  struct pci_conv_result expected=a->conventional;
  if(p&&a->device.bdf==p->endpoint)expected.command_status_raw&=~6U;
  if(memcmp(&expected,&b->conventional,sizeof expected))return 0;
  for(unsigned j=0;j<a->conventional_count;j++){
   const struct rc_read *x=&a->conventional_reads[j],*y=&b->conventional_reads[j];uint32_t value=x->value;
   if(p&&a->device.bdf==p->endpoint&&x->offset==4)value&=~6U;
   if(x->offset!=y->offset||x->status||y->status||value!=y->value)return 0;
  }
 }
 for(unsigned i=0;i<4;i++){
  const struct pi_cmp *a=&before->cmp[i],*b=NULL;int changed=0;
  for(unsigned j=0;j<4;j++)if(after->cmp[j].index==a->index)b=&after->cmp[j];
  for(unsigned t=0;t<PT_COUNT;t++)if((applied_mask&(1U<<t))&&before->functions[a->index].device.bdf==plans[t].endpoint)changed=1;
  if(!b||a->extended_count!=b->extended_count||b->cya!=(changed?0x30:a->cya)||b->cfg!=(changed?0x8000000aU:a->cfg))return 0;
  struct cap_result expected=a->rebar;
  if(changed)for(unsigned j=0;j<expected.count;j++)if(expected.entries[j].offset==0xbbc){
   expected.entries[j].capability=BT_CFG_CAP_OBSERVED;expected.entries[j].control=(expected.entries[j].control&~0x1f00U)|(16U<<8);
  }
  if(memcmp(&expected,&b->rebar,sizeof expected))return 0;
  for(unsigned j=0;j<a->extended_count;j++){
   const struct rc_read *x=&a->extended_reads[j],*y=&b->extended_reads[j];uint32_t value=x->value;
   if(changed&&x->offset==0xbbc)value=BT_CFG_CAP_OBSERVED;
   if(changed&&x->offset==0xbc0)value=(value&~0x1f00U)|(16U<<8);
   if(x->offset!=y->offset||x->status||y->status||value!=y->value)return 0;
  }
 }
 return 1;
}

#endif
