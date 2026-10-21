#ifndef CMP_WRITER_EFI_TABLES_H
#define CMP_WRITER_EFI_TABLES_H
#include <efi.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static int we_table(const EFI_SYSTEM_TABLE *system,const EFI_GUID *guid,
 int(*readable)(void*,uintptr_t,size_t),void *context,size_t required,void **out){
 if(out)*out=NULL;
 if(!system||!guid||!readable||!required||!out||!system->NumberOfTableEntries||
    system->NumberOfTableEntries>1024||!system->ConfigurationTable||
    readable(context,(uintptr_t)system->ConfigurationTable,
      system->NumberOfTableEntries*sizeof(EFI_CONFIGURATION_TABLE))!=1)return 0;
 void *found=NULL;unsigned matches=0;
 for(UINTN i=0;i<system->NumberOfTableEntries;i++){
  const EFI_CONFIGURATION_TABLE *t=system->ConfigurationTable+i;
  if(!memcmp(&t->VendorGuid,guid,sizeof *guid)){found=t->VendorTable;matches++;}
 }
 if(matches!=1||!found||readable(context,(uintptr_t)found,required)!=1)return 0;
 *out=found;return 1;
}
#endif
