#define main snapshot_test_main
#include "test_pci_snapshot.c"
#undef main
#include "../src/resource_snapshot.h"
static struct rs_snapshot resources,resources_after;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL roots[RC_ROOTS];
static uint8_t root_lists[RC_ROOTS][48],query_list[8192];
static size_t query_bytes;
static uint8_t table[36];
static struct rc_acpi acpi;
static unsigned queries,root_queries,map_queries,unsupported_query,bad_root_count,duplicate_root;
static unsigned bad_map_version,bad_map_stride,bad_map_size;
static EFI_STATUS EFIAPI attributes(EFI_PCI_IO_PROTOCOL *p,UINT8 query,UINT64 *supports,VOID **out){
 unsigned i=(unsigned)(p-pci);assert(i<PI_FUNCTIONS);assert(query<(((config[i][3]>>16)&127)==1?2:6));
 if(fail())return EFI_DEVICE_ERROR;
 
 *supports=0;queries++;if(unsupported_query&&queries==1){*out=NULL;return EFI_UNSUPPORTED;}
 *out=malloc(query_bytes);assert(*out);memcpy(*out,query_list,query_bytes);return 0;
}
static EFI_STATUS EFIAPI root_config(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *p,VOID **out){
 unsigned i=(unsigned)(p-roots);assert(i<RC_ROOTS);if(fail())return EFI_DEVICE_ERROR;
 *out=root_lists[i];root_queries++;return 0;
}
static EFI_STATUS EFIAPI enum_roots(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out){
 EFI_GUID expected=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;assert(type==ByProtocol&&!key&&!memcmp(guid,&expected,sizeof expected));
 if(fail())return EFI_DEVICE_ERROR;
 *n=bad_root_count?3:RC_ROOTS;*out=malloc(RC_ROOTS*sizeof **out);assert(*out);
 for(unsigned i=0;i<RC_ROOTS;i++)(*out)[i]=(VOID *)(uintptr_t)(1000+i);
 if(duplicate_root)(*out)[1]=(*out)[0];
 return 0;
}
static EFI_STATUS EFIAPI handle_root(EFI_HANDLE h,EFI_GUID *guid,VOID **out){
 EFI_GUID expected=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;assert(!memcmp(guid,&expected,sizeof expected));
 if(fail())return EFI_DEVICE_ERROR;
 unsigned i=(unsigned)(uintptr_t)h-1000;assert(i<RC_ROOTS);*out=&roots[i];return 0;
}
static EFI_STATUS EFIAPI memory_map(UINTN *size,EFI_MEMORY_DESCRIPTOR *out,UINTN *key,UINTN *stride,UINT32 *version){
 if(fail())return EFI_DEVICE_ERROR;
 assert(*size==RC_MAP_MAX);memset(out,0,sizeof *out);
 out->Type=EfiConventionalMemory;out->PhysicalStart=0x100000;out->NumberOfPages=0x100;
 *size=bad_map_size?sizeof *out+1:sizeof *out;*key=4;*stride=bad_map_stride?4:sizeof *out;*version=bad_map_version?2:1;map_queries++;return 0;
}
static void resource_setup(void){
 setup();assert(pi_capture(&before,identities));memset(&resources,0,sizeof resources);memset(&resources_after,0,sizeof resources_after);memset(roots,0,sizeof roots);
 memset(query_list,0,sizeof query_list);query_list[0]=0x79;query_bytes=2;
 for(unsigned i=0;i<PI_FUNCTIONS;i++)pci[i].GetBarAttributes=attributes;
 for(unsigned i=0;i<RC_ROOTS;i++){roots[i].Configuration=root_config;root_lists[i][0]=0x79;root_lists[i][1]=0;}
 bs.LocateHandleBuffer=enum_roots;bs.HandleProtocol=handle_root;bs.GetMemoryMap=memory_map;
 memset(table,0,sizeof table);acpi.dsdt=(struct rc_blob){table,sizeof table};
 for(unsigned i=0;i<6;i++)acpi.ssdt[i]=(struct rc_blob){table,sizeof table};
 queries=root_queries=map_queries=calls=fail_at=unsupported_query=bad_root_count=duplicate_root=0;
 bad_map_size=bad_map_stride=bad_map_version=0;
}
int main(void){
 unsigned checks=0;
#define RCHECK(x) do{checks++;assert(x);}while(0)
 resource_setup();RCHECK(rs_capture(&resources,&before,&acpi));RCHECK(queries==568&&root_queries==4&&map_queries==1);
 unsigned total=calls;RCHECK(resources.view.function_count==112&&resources.view.query_count==568&&resources.view.root_count==4);
 RCHECK(resources.view.target_extcap_bdf==0x100&&resources.view.target_extcap_count>0);
 RCHECK(resources.view.plan_count==PT_COUNT&&resources.view.plans==pair_targets);
 for(unsigned t=0;t<PT_COUNT;t++)RCHECK(resources.view.target_extcaps[t].bdf==pair_targets[t].endpoint&&resources.view.target_extcaps[t].count>0&&resources.view.target_extcaps[t].trace!=NULL);
 for(unsigned t=1;t<PT_COUNT;t++)RCHECK(resources.view.target_extcaps[t].trace!=resources.view.target_extcaps[0].trace);
 for(unsigned i=0;i<RC_FUNCTIONS;i++)RCHECK(resources.view.raw_slot_present[i]==((((before.functions[i].header[3]>>16)&127)==1)?3:63));
 RCHECK(rs_capture(&resources_after,&before,&acpi)&&rs_same_resources(&resources,&resources_after));
 RCHECK(resources.view.acpi.dsdt.data!=table&&!memcmp(resources.view.acpi.dsdt.data,table,36));
 RCHECK(!rs_capture(&resources,&before,&acpi));
 for(unsigned at=1;at<=total;at++){
  resource_setup();fail_at=at;RCHECK(!rs_capture(&resources,&before,&acpi)&&resources.status==RS_FAILED);
  unsigned prior=calls;RCHECK(!rs_capture(&resources,&before,&acpi)&&calls==prior);
 }
 for(unsigned i=0;i<RC_FUNCTIONS;i++){
  resource_setup();before.functions[i].raw_slot_present^=1;RCHECK(!rs_capture(&resources,&before,&acpi));
 }
 resource_setup();unsupported_query=1;RCHECK(rs_capture(&resources,&before,&acpi));
 RCHECK(resources.queries[0].status==EFI_UNSUPPORTED&&resources.queries[0].resources.size==0&&resources.queries[0].resources.data==NULL);
 for(unsigned bad=0;bad<10;bad++){
  resource_setup();
  if(bad==0)bad_root_count=1;
  if(bad==1)duplicate_root=1;
  if(bad==2)bad_map_version=1;
  if(bad==3)bad_map_stride=1;
  if(bad==4)bad_map_size=1;
  if(bad==5){query_bytes=8192;for(unsigned i=0;i<8192;i+=4){query_list[i]=0x80;query_list[i+1]=1;}}
  if(bad==6){query_bytes=4;query_list[0]=0x80;query_list[1]=0xff;query_list[2]=0xff;}
  if(bad==7){query_list[0]=0x78;}
  if(bad==8)acpi.ssdt[4].size=RC_AML_TOTAL_MAX;
  if(bad==9)roots[0].SegmentNumber=1;
  RCHECK(!rs_capture(&resources,&before,&acpi));
 }
 resource_setup();RCHECK(rs_capture(&resources,&before,&acpi));query_list[1]=1;
 RCHECK(rs_capture(&resources_after,&before,&acpi)&&!rs_same_resources(&resources,&resources_after));
 resource_setup();RCHECK(rs_capture(&resources,&before,&acpi));table[0]=1;
 RCHECK(rs_capture(&resources_after,&before,&acpi)&&!rs_same_resources(&resources,&resources_after));
 resource_setup();resources.pci=&before;resources.arena_used=RC_LIST_TOTAL_MAX-1;struct rc_blob blob;
 RCHECK(!rs_list(&resources,query_list,&blob));
 printf("%u raw resource ABI/bounds checks PASS (%u successful EFI calls)\n",checks,total);return 0;
}
