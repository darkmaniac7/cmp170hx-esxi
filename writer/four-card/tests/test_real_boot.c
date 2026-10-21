/* CPU-only GNU-EFI ABI harness; never invokes efi_main or a hardware protocol. */
#include <assert.h>
#include <stdio.h>
#include "../src/handoff_efi.c"
INTN CompareMem(CONST VOID *a,CONST VOID *b,UINTN n){return ba_compare(a,b,n);}
static unsigned boot_reads,current_reads;
static UINT16 mocked_current=4;
static EFI_STATUS mocked_status;
static UINTN mocked_size=2;
static UINT32 mocked_attributes=6;
static EFI_STATUS EFIAPI mock_get(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *size,VOID *data){
    EFI_GUID expected=EFI_GLOBAL_VARIABLE;assert(!ba_compare(guid,&expected,sizeof expected));
    if(name[4]=='0'){
        static CHAR16 exact[]=L"Boot0003";assert(!ba_compare(name,exact,sizeof exact));boot_reads++;
        assert(*size>=sizeof boot_fixture);ba_copy(data,boot_fixture,sizeof boot_fixture);
        *size=sizeof boot_fixture;*attrs=7;return EFI_SUCCESS;
    }
    static CHAR16 exact[]=L"BootCurrent";assert(!ba_compare(name,exact,sizeof exact));current_reads++;
    assert(*size==2);((UINT8 *)data)[0]=mocked_current&255;((UINT8 *)data)[1]=mocked_current>>8;
    *size=mocked_size;*attrs=mocked_attributes;return mocked_status;
}
int main(void){
    EFI_RUNTIME_SERVICES runtime={0};EFI_SYSTEM_TABLE system={0};runtime.GetVariable=mock_get;system.RuntimeServices=&runtime;st=&system;
    UINT8 raw[1024]={0};UINTN n=sizeof raw;UINT32 attrs=0;
    assert(read_boot(raw,&n,&attrs)==0);assert(boot_reads==1&&n==120&&attrs==7&&!ba_compare(raw,boot_fixture,120));
    assert(current_boot_check()==1);assert(current_boot.value==4&&current_reads==1);
    assert(current_boot_check()==1);mocked_current=3;assert(!current_boot_check());mocked_current=4;
    mocked_status=EFI_NOT_FOUND;assert(!current_boot_check());mocked_status=0;
    mocked_size=1;assert(!current_boot_check());mocked_size=2;
    mocked_attributes=7;assert(!current_boot_check());
    assert(boot_reads==1&&current_reads==6);
    puts("7 real GetVariable ABI scenarios PASS; no entrypoint or hardware call");return 0;
}
