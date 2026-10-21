/* CPU-only early-refusal checks. No entrypoint/firmware operation is invoked. */
#include <assert.h>
#include <stdio.h>
#include "../src/handoff_efi.c"
INTN CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return ba_compare(a,b,n);}
INTN CompareGuid(EFI_GUID*a,EFI_GUID*b){return ba_compare(a,b,sizeof *a);}
VOID CopyMem(VOID*d,CONST VOID*s,UINTN n){ba_copy(d,s,n);}
VOID SetMem(VOID*d,UINTN n,UINT8 v){ba_fill(d,v,n);}
static char records[20][1024];static unsigned record_count;
static EFI_STATUS EFIAPI capture_output(SIMPLE_TEXT_OUTPUT_INTERFACE *unused,CHAR16 *raw){
 (void)unused;assert(record_count<20);unsigned i=0;
 while(raw[i]){assert(i<1023);records[record_count][i]=(char)raw[i];i++;}
 records[record_count++][i]=0;return 0;
}
int main(void){
 static struct pc_pair_session local;unsigned checks=0;
#define CHECK(x) do{checks++;assert(x);}while(0)
 struct hf_binding binding={0};
 CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 writer=&local;CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 local.status=PC_RUNNING;CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 local.env.handoff=&session;CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 session.phase=HF_MUTATION_CHECKING;
 CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_COMPROMISED_DATA);
 CHECK(current_check(NULL,HF_CHECK_BEFORE_START,NULL)==EFI_COMPROMISED_DATA);
 CHECK(current_check(NULL,HF_CHECK_PREPARED,&binding)==EFI_COMPROMISED_DATA);
 CHECK(!session.mutation_started&&!session.start_called&&local.access[0].write_attempted==0&&local.access[1].write_attempted==0);
 /* Diagnostics render retained state only. A console is the only callback;
  * PCI/GCD service pointers remain null, proving no decorative device reads. */
 EFI_SYSTEM_TABLE system={0};SIMPLE_TEXT_OUTPUT_INTERFACE console={0};
 console.OutputString=capture_output;system.ConOut=&console;st=&system;
 local.pair.attempted=19;local.applied_mask=1;
 local.transaction[0].failure.reason=2;local.transaction[1].failure.reason=3;
 writer_report("test");CHECK(record_count==17);
 for(unsigned t=0;t<2;t++){
  const char *index=t?"target_index=0x0000000000000001":"target_index=0x0000000000000000";
  for(unsigned j=0;j<4;j++)CHECK(strstr(records[t*4+j],index)!=NULL);
  CHECK(strstr(records[15+t],index)!=NULL);
  CHECK(strstr(records[t*4],"attempted_stores=0x0000000000000013")!=NULL);
  CHECK(strstr(records[t*4],"applied_mask=0x0000000000000001")!=NULL);
 }
 CHECK(strstr(records[1],"reason=0x0000000000000002")!=NULL);
 CHECK(strstr(records[5],"reason=0x0000000000000003")!=NULL);
 CHECK(strstr(records[12],"snapshot=0x0000000000000004")!=NULL);
 printf("%u writer entrypoint and indexed diagnostic checks PASS; synthetic console only\n",checks);return 0;
}
