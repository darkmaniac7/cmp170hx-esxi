#include <efi.h>
#include <efilib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/acpi_current.h"
static unsigned checks,calls;static int fault;static struct ws_context w;
static _Alignas(4096) uint8_t arena[8192];static EFI_BOOT_SERVICES bs;static EFI_SYSTEM_TABLE st;
static uint8_t expected[7][32];
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static EFI_STATUS EFIAPI map(UINTN *size,EFI_MEMORY_DESCRIPTOR *out,UINTN *key,UINTN *stride,UINT32 *version){
 calls++;*key=7;*stride=sizeof *out;*version=1;
 if(fault==1)return EFI_DEVICE_ERROR;
 if(fault==2){*size=WS_MAP_MAX+1;return EFI_BUFFER_TOO_SMALL;}
 if(*size<sizeof *out)return EFI_BUFFER_TOO_SMALL;
 *size=sizeof *out;memset(out,0,sizeof *out);out->Type=EfiACPIReclaimMemory;
 out->PhysicalStart=(UINTN)arena;out->NumberOfPages=2;
 if(fault==3)*stride=39;if(fault==4)*version=2;if(fault==5)out->Type=EfiMemoryMappedIO;
 if(fault==6)out->NumberOfPages=UINT64_MAX;if(fault==7)out->PhysicalStart++;
 if(fault==8){out[1]=out[0];*size=2*sizeof *out;}
 if(fault==9)out->Type=16;
 return EFI_SUCCESS;
}
static void le(uint8_t*p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static void sum(uint8_t*p,unsigned n,unsigned off){p[off]=0;uint8_t s=0;for(unsigned i=0;i<n;i++)s+=p[i];p[off]=(uint8_t)-s;}
static void table(unsigned off,const char*sig,unsigned n){uint8_t*p=arena+off;memcpy(p,sig,4);le(p+4,n,4);p[8]=2;}
static void setup(void){
 memset(&w,0,sizeof w);memset(arena,0,sizeof arena);memset(&bs,0,sizeof bs);memset(&st,0,sizeof st);calls=0;fault=0;
 bs.GetMemoryMap=map;st.BootServices=&bs;st.NumberOfTableEntries=1;st.ConfigurationTable=(void*)(arena+7000);
 st.ConfigurationTable[0].VendorGuid=(EFI_GUID){0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0,0x80,0xc7,0x3c,0x88,0x81}};
 st.ConfigurationTable[0].VendorTable=arena;
 memcpy(arena,"RSD PTR ",8);arena[15]=2;le(arena+20,36,4);le(arena+24,(UINTN)(arena+256),8);sum(arena,20,8);sum(arena,36,32);
 table(256,"XSDT",36+7*8);le(arena+256+36,(UINTN)(arena+512),8);
 table(512,"FACP",148);le(arena+512+140,(UINTN)(arena+768),8);sum(arena+512,148,9);
 table(768,"DSDT",36);sum(arena+768,36,9);inv_sha256(arena+768,36,expected[0]);
 for(unsigned i=0;i<6;i++){unsigned off=1024+i*256;table(off,"SSDT",36);arena[off+10]=(uint8_t)(i+1);sum(arena+off,36,9);inv_sha256(arena+off,36,expected[i+1]);le(arena+256+44+i*8,(UINTN)(arena+off),8);}
 sum(arena+256,36+7*8,9);
}
int main(void){
 struct rc_acpi out;setup();CHECK(ws_capture(&w,&st,expected,&out)==1);CHECK(calls==1&&w.result.status==AE_OK);
 CHECK(out.dsdt.size==36&&out.dsdt.data!=arena+768);CHECK(w.map_key==7&&w.selected==1);
 for(int i=1;i<=9;i++){setup();fault=i;CHECK(!ws_capture(&w,&st,expected,&out));CHECK(!out.dsdt.data&&!w.selected);}
 setup();st.NumberOfTableEntries=0;CHECK(!ws_capture(&w,&st,expected,&out));
 setup();st.NumberOfTableEntries=1025;CHECK(!ws_capture(&w,&st,expected,&out));
 setup();st.ConfigurationTable=(void *)(UINTN)1;CHECK(!ws_capture(&w,&st,expected,&out));
 setup();st.ConfigurationTable[1]=st.ConfigurationTable[0];st.NumberOfTableEntries=2;CHECK(!ws_capture(&w,&st,expected,&out));
 setup();st.ConfigurationTable[0].VendorGuid.Data1^=1;CHECK(!ws_capture(&w,&st,expected,&out));
 setup();st.ConfigurationTable[0].VendorTable=(void *)(arena+8191);CHECK(!ws_capture(&w,&st,expected,&out));
 setup();arena[1024+10]^=1;CHECK(!ws_capture(&w,&st,expected,&out));
 setup();expected[0][0]^=1;CHECK(!ws_capture(&w,&st,expected,&out));
 setup();CHECK(ws_capture(&w,&st,expected,&out));arena[768+10]^=1;CHECK(!ws_capture(&w,&st,expected,&out));CHECK(!out.dsdt.data&&!w.selected);
 setup();CHECK(!ws_capture(&w,NULL,expected,&out));CHECK(!ws_capture(&w,&st,NULL,&out));CHECK(!ws_capture(NULL,&st,expected,&out));
 printf("%u current EFI ACPI checks PASS\n",checks);return 0;
}
