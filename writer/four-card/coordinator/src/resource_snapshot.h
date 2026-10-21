#ifndef CMP_RESOURCE_SNAPSHOT_H
#define CMP_RESOURCE_SNAPSHOT_H
#include "pci_snapshot.h"
enum rs_status {RS_NEW,RS_FAILED,RS_CAPTURED};
struct rs_snapshot {
 enum rs_status status;struct pi_snapshot *pci;
 struct rc_snapshot view;
 struct rc_function functions[RC_FUNCTIONS];struct rc_root roots[RC_ROOTS];
 struct rc_query queries[RC_QUERIES];
 EFI_HANDLE root_handles[RC_ROOTS];EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root_protocols[RC_ROOTS];
 size_t arena_used;uint8_t arena[RC_LIST_TOTAL_MAX];
 size_t aml_used;uint8_t aml[RC_AML_TOTAL_MAX];
 _Alignas(8) uint8_t memory[RC_MAP_MAX];
 EFI_STATUS last_status;uint32_t last_owner,last_query;
};
static int rs_list(struct rs_snapshot *s,const void *raw,struct rc_blob *out){
 const uint8_t *p=raw;size_t offset=0,start=s->arena_used;out->data=NULL;out->size=0;
 for(unsigned count=0;count<128;count++){
  if(!p||(uintptr_t)p>UINTPTR_MAX-offset||!pi_range(s->pci,p+offset,1))return 0;
  uint8_t tag=p[offset];size_t bytes;
  if(tag==0x79)bytes=2;
  else{
   if(!(tag&0x80)||offset>RC_LIST_MAX-3||!pi_range(s->pci,p+offset,3))return 0;
   bytes=3+(size_t)p[offset+1]+((size_t)p[offset+2]<<8);
   if(bytes>259)return 0;
  }
  if(offset>RC_LIST_MAX-bytes||s->arena_used>RC_LIST_TOTAL_MAX-bytes||
     !pi_range(s->pci,p+offset,bytes))return 0;
  memcpy(s->arena+s->arena_used,p+offset,bytes);s->arena_used+=bytes;offset+=bytes;
  if(tag==0x79){out->data=s->arena+start;out->size=offset;return 1;}
 }
 return 0;
}
static int rs_aml(struct rs_snapshot *s,const struct rc_blob *source,struct rc_blob *out){
 if(source->size<36||source->size>RC_AML_TOTAL_MAX||s->aml_used>RC_AML_TOTAL_MAX-source->size||
    !pi_range(s->pci,source->data,source->size))return 0;
 out->data=s->aml+s->aml_used;out->size=source->size;
 memcpy(s->aml+s->aml_used,source->data,source->size);s->aml_used+=source->size;return 1;
}
static int rs_capture(struct rs_snapshot *s,struct pi_snapshot *pci,const struct rc_acpi *acpi){
 if(!s||s->status!=RS_NEW)return 0;
 s->status=RS_FAILED;s->pci=pci;
 if(!pi_pair_complete(pci)||
    !pi_range(pci,pci->bs,sizeof *pci->bs)||!pci->bs->FreePool||!pci->bs->LocateHandleBuffer||
    !pci->bs->HandleProtocol||!pci->bs->GetMemoryMap||!acpi)return 0;
 EFI_BOOT_SERVICES *bs=pci->bs;s->view.functions=s->functions;s->view.function_count=RC_FUNCTIONS;
 s->view.queries=s->queries;s->view.roots=s->roots;
 s->view.plans=pair_targets;s->view.plan_count=PT_COUNT;
 for(unsigned i=0;i<RC_FUNCTIONS;i++){
  struct pi_function *f=&pci->functions[i];struct rc_function *v=&s->functions[i];
  if(!pi_range(pci,f->device.pci,sizeof *f->device.pci)||!f->device.pci->GetBarAttributes)return 0;
  v->bdf=f->device.bdf;memcpy(v->header,f->header,sizeof v->header);
  s->view.raw_slot_present[i]=f->raw_slot_present;
  v->conventional=f->conventional_reads;v->conventional_count=f->conventional_count;
  unsigned type=(f->header[3]>>16)&127,bars=type==0?6:type==1?2:0;if(!bars)return 0;
  for(unsigned j=0;j<bars;j++){
   if(s->view.query_count>=RC_QUERIES)return 0;
   struct rc_query *q=&s->queries[s->view.query_count++];q->owner_index=i;q->query_index=j;
   UINT64 supports=0;VOID *raw=NULL;s->last_owner=i;s->last_query=j;
   s->last_status=uefi_call_wrapper(f->device.pci->GetBarAttributes,4,f->device.pci,(UINT8)j,&supports,&raw);
   q->status=s->last_status;q->supports=supports;int valid=0;
   if(!q->status)valid=rs_list(s,raw,&q->resources);
   else if(q->status==EFI_UNSUPPORTED&&!raw)valid=1;
   if(raw){s->last_status=uefi_call_wrapper(bs->FreePool,1,raw);if(s->last_status)valid=0;}
   if(!valid)return 0;
  }
 }
 if(s->view.query_count!=RC_QUERIES)return 0;
 EFI_GUID guid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;EFI_HANDLE *handles=NULL;UINTN count=0;int okay=0;
 s->last_status=uefi_call_wrapper(bs->LocateHandleBuffer,5,ByProtocol,&guid,NULL,&count,&handles);
 if(s->last_status)return 0;
 if(count!=RC_ROOTS||!pi_range(pci,handles,count*sizeof *handles))goto roots_done;
 for(UINTN i=0;i<count;i++){
  if(!handles[i])goto roots_done;
  for(UINTN j=0;j<i;j++)if(handles[j]==handles[i])goto roots_done;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root=NULL;
  s->last_status=uefi_call_wrapper(bs->HandleProtocol,3,handles[i],&guid,(VOID **)&root);
  if(s->last_status||!pi_range(pci,root,sizeof *root)||!root->Configuration||root->SegmentNumber)goto roots_done;
  for(UINTN j=0;j<i;j++)if(s->root_protocols[j]==root)goto roots_done;
  s->root_handles[i]=handles[i];s->root_protocols[i]=root;
  VOID *raw=NULL;s->last_status=uefi_call_wrapper(root->Configuration,2,root,&raw);
  s->roots[i].segment=root->SegmentNumber;s->roots[i].status=s->last_status;
  if(s->last_status||!rs_list(s,raw,&s->roots[i].resources))goto roots_done;
  s->view.root_count++;
  /* RootBridge.Configuration returns provider-owned memory. Never free it. */
 }
 okay=s->view.root_count==RC_ROOTS;
roots_done:
 if(handles){s->last_status=uefi_call_wrapper(bs->FreePool,1,handles);if(s->last_status)okay=0;}
 if(!okay)return 0;
 if(!rs_aml(s,&acpi->dsdt,&s->view.acpi.dsdt))return 0;
 for(unsigned i=0;i<6;i++)if(!rs_aml(s,&acpi->ssdt[i],&s->view.acpi.ssdt[i]))return 0;
 UINTN bytes=sizeof s->memory,key=0,stride=0;UINT32 version=0;
 s->last_status=uefi_call_wrapper(bs->GetMemoryMap,5,&bytes,(EFI_MEMORY_DESCRIPTOR *)s->memory,&key,&stride,&version);
 s->view.memory=(struct rc_memory){s->last_status,key,(uint32_t)stride,version,{s->memory,bytes}};
 if(s->last_status||!bytes||bytes>sizeof s->memory||stride<sizeof(EFI_MEMORY_DESCRIPTOR)||stride>256||
    (stride&7)||bytes%stride||version!=1)return 0;
 for(unsigned t=0;t<PT_COUNT;t++){
  unsigned found=0;
  for(unsigned i=0;i<4;i++)if(pci->cmp[i].index==pci->endpoint_indices[t]){
   s->view.target_extcaps[t]=(struct rc_target_extcap){pair_targets[t].endpoint,pci->cmp[i].extended_reads,pci->cmp[i].extended_count};found++;
  }
  if(found!=1)return 0;
 }
 s->view.target_extcap_bdf=s->view.target_extcaps[0].bdf;
 s->view.target_extcap=s->view.target_extcaps[0].trace;
 s->view.target_extcap_count=s->view.target_extcaps[0].count;
 s->status=RS_CAPTURED;return 1;
}
static int rs_blob_same(const struct rc_blob *a,const struct rc_blob *b){
 return a->size==b->size&&(!a->size||(!memcmp(a->data,b->data,a->size)));
}
/* Memory-map allocation bookkeeping may change when firmware allocates pool
 * buffers. The separate pure validator checks each fresh map for proposal
 * collisions. Do not mask changes in root/query/AML bytes as bookkeeping. */
static int rs_same_resources(const struct rs_snapshot *a,const struct rs_snapshot *b){
 if(!a||!b||a==b||a->status!=RS_CAPTURED||b->status!=RS_CAPTURED||
    a->view.query_count!=RC_QUERIES||b->view.query_count!=RC_QUERIES||
    a->view.root_count!=RC_ROOTS||b->view.root_count!=RC_ROOTS)return 0;
 for(unsigned i=0;i<RC_QUERIES;i++){
  const struct rc_query *x=&a->queries[i],*y=&b->queries[i];
  if(x->owner_index!=y->owner_index||x->query_index!=y->query_index||x->status!=y->status||x->supports!=y->supports||
     !rs_blob_same(&x->resources,&y->resources))return 0;
 }
 for(unsigned i=0;i<RC_ROOTS;i++){
  unsigned matches=0;
  for(unsigned j=0;j<RC_ROOTS;j++)if(a->root_handles[i]==b->root_handles[j]&&a->root_protocols[i]==b->root_protocols[j]&&
     a->roots[i].segment==b->roots[j].segment&&a->roots[i].status==b->roots[j].status&&rs_blob_same(&a->roots[i].resources,&b->roots[j].resources))matches++;
  if(matches!=1)return 0;
 }
 if(!rs_blob_same(&a->view.acpi.dsdt,&b->view.acpi.dsdt))return 0;
 for(unsigned i=0;i<6;i++)if(!rs_blob_same(&a->view.acpi.ssdt[i],&b->view.acpi.ssdt[i]))return 0;
 return 1;
}
#endif
