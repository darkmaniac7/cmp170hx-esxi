#include <efi.h>
#include <efilib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/efi_tables.h"
static unsigned checks;static EFI_CONFIGURATION_TABLE tables[2];static uint8_t data[96];
static int readable(void *ctx,uintptr_t address,size_t n){(void)ctx;return (address==(uintptr_t)tables&&n<=sizeof tables)||(address==(uintptr_t)data&&n<=sizeof data);}
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
 EFI_GUID wanted={0x05ad34ba,0x6f02,0x4214,{0x95,0x2e,0x4d,0xa0,0x39,0x8e,0x2b,0xb9}};
 EFI_SYSTEM_TABLE st={0};void *out=NULL;st.NumberOfTableEntries=1;st.ConfigurationTable=tables;
 tables[0].VendorGuid=wanted;tables[0].VendorTable=data;
 CHECK(we_table(&st,&wanted,readable,NULL,sizeof data,&out)==1&&out==data);
 st.NumberOfTableEntries=0;CHECK(!we_table(&st,&wanted,readable,NULL,80,&out)&&!out);
 st.NumberOfTableEntries=1025;CHECK(!we_table(&st,&wanted,readable,NULL,80,&out));
 st.NumberOfTableEntries=2;tables[1]=tables[0];CHECK(!we_table(&st,&wanted,readable,NULL,80,&out));
 st.NumberOfTableEntries=1;tables[0].VendorGuid.Data1^=1;CHECK(!we_table(&st,&wanted,readable,NULL,80,&out));
 tables[0].VendorGuid=wanted;tables[0].VendorTable=NULL;CHECK(!we_table(&st,&wanted,readable,NULL,80,&out));
 tables[0].VendorTable=(void*)1;CHECK(!we_table(&st,&wanted,readable,NULL,80,&out));
 tables[0].VendorTable=data;CHECK(!we_table(&st,&wanted,readable,NULL,97,&out));
 st.ConfigurationTable=(void*)1;CHECK(!we_table(&st,&wanted,readable,NULL,80,&out));
 CHECK(!we_table(NULL,&wanted,readable,NULL,80,&out));CHECK(!we_table(&st,NULL,readable,NULL,80,&out));
 CHECK(!we_table(&st,&wanted,NULL,NULL,80,&out));CHECK(!we_table(&st,&wanted,readable,NULL,80,NULL));
 printf("%u EFI table binding checks PASS\n",checks);return 0;
}
