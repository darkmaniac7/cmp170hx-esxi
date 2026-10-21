#ifndef CMP_WRITER_ACPI_CURRENT_H
#define CMP_WRITER_ACPI_CURRENT_H
#include <efi.h>
#include <efilib.h>
#include "acpi_select.h"
#include "../pins/acpi_ranges.h"
#define WS_MAP_MAX (64U*1024U)
struct ws_context {
 _Alignas(8) uint8_t map[WS_MAP_MAX];UINTN map_bytes,map_key,descriptor_bytes;UINT32 version;
 struct ae_ram_range ranges[AE_RAM_MAX_RANGES];struct ae_ram_adapter ram;
 struct ae_result result;uint8_t workspace[AE_TOTAL_LIMIT];unsigned selected;
};
static int ws_copy(void *context,uint64_t address,uint8_t *out,size_t length){
 if(!out||!ae_ram_contains(context,address,length))return -1;
 const volatile uint8_t *source=(const volatile uint8_t *)(uintptr_t)address;
 for(size_t i=0;i<length;i++)out[i]=source[i];
 return (int)length;
}
static int ws_ram(const struct ws_context *s,uintptr_t address,size_t length){
 if(!s||!s->ram.valid||!address||!length||address>UINTPTR_MAX-(length-1))return 0;
 for(size_t i=0;i<s->ram.count;i++){
  const struct ae_ram_range *r=&s->ram.ranges[i];unsigned t=r->memory_type;
  if(!((t>=1&&t<=7)||t==9||t==10))continue;
  if(address>=r->address&&address-r->address<r->length&&length<=r->length-(address-r->address))return 1;
 }
 return 0;
}
static int ws_capture(struct ws_context *s,EFI_SYSTEM_TABLE *system,const uint8_t pins[7][32],struct rc_acpi *out){
 if(out)memset(out,0,sizeof *out);
 if(s){s->selected=0;s->ram.valid=0;s->result.status=AE_BAD_INPUT;}
 if(!s||!system||!system->BootServices||!system->BootServices->GetMemoryMap||!pins||!out)return 0;
 s->map_bytes=sizeof s->map;s->map_key=0;s->descriptor_bytes=0;s->version=0;
 EFI_STATUS status=uefi_call_wrapper(system->BootServices->GetMemoryMap,5,&s->map_bytes,
   (EFI_MEMORY_DESCRIPTOR *)s->map,&s->map_key,&s->descriptor_bytes,&s->version);
 if(status||!s->map_bytes||s->map_bytes>sizeof s->map||s->descriptor_bytes<40||
    s->descriptor_bytes>256||(s->descriptor_bytes&7)||s->map_bytes%s->descriptor_bytes||
    s->version!=1||s->map_bytes/s->descriptor_bytes>AE_RAM_MAX_RANGES)return 0;
 size_t count=s->map_bytes/s->descriptor_bytes;uint8_t digest[32];
 for(size_t i=0;i<count;i++){
  const EFI_MEMORY_DESCRIPTOR *d=(const void *)(s->map+i*s->descriptor_bytes);
  if(!d->NumberOfPages||d->NumberOfPages>UINT64_MAX/4096||(d->PhysicalStart&4095))return 0;
  s->ranges[i]=(struct ae_ram_range){d->PhysicalStart,d->NumberOfPages*4096,d->Type};
 }
 inv_sha256(s->map,s->map_bytes,digest);
 if(!ae_ram_init(&s->ram,s->ranges,count,digest,ws_copy,&s->ram))return 0;
 if(!system->NumberOfTableEntries||system->NumberOfTableEntries>1024||
    !ws_ram(s,(uintptr_t)system->ConfigurationTable,system->NumberOfTableEntries*sizeof(EFI_CONFIGURATION_TABLE)))return 0;
 const EFI_GUID acpi2={0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0,0x80,0xc7,0x3c,0x88,0x81}};
 unsigned matches=0;uintptr_t rsdp=0;
 for(UINTN i=0;i<system->NumberOfTableEntries;i++){
  const EFI_CONFIGURATION_TABLE *t=system->ConfigurationTable+i;
  if(!memcmp(&t->VendorGuid,&acpi2,sizeof acpi2)){matches++;rsdp=(uintptr_t)t->VendorTable;}
 }
 if(matches!=1||!rsdp)return 0;
 struct ae_reader reader={&s->ram,ae_ram_contains,ae_ram_read};
 if(ae_export(&reader,rsdp,s->workspace,sizeof s->workspace,&s->result)!=AE_OK||
    !ws_select(&s->result,s->workspace,sizeof s->workspace,pins,out))return 0;
 s->selected=1;return 1;
}
#endif
