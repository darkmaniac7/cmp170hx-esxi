/* CPU-only early-refusal, fail-open decision and console-line checks.
 * No entrypoint/firmware operation is invoked; nr_start is never called here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/handoff_efi.c"
INTN CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return ba_compare(a,b,n);}
INTN CompareGuid(EFI_GUID*a,EFI_GUID*b){return ba_compare(a,b,sizeof *a);}
VOID CopyMem(VOID*d,CONST VOID*s,UINTN n){ba_copy(d,s,n);}
VOID SetMem(VOID*d,UINTN n,UINT8 v){ba_fill(d,v,n);}
static char records[40][1024];static unsigned record_count;
static EFI_STATUS EFIAPI capture_output(SIMPLE_TEXT_OUTPUT_INTERFACE *unused,CHAR16 *raw){
 (void)unused;assert(record_count<40);unsigned i=0;
 while(raw[i]){assert(i<1023);records[record_count][i]=(char)raw[i];i++;}
 records[record_count++][i]=0;return 0;
}
static int reason_is(const char *expected){return !strcmp(fallback_reason(EFI_COMPROMISED_DATA),expected);}
int main(void){
 static struct pc_pair_session local;unsigned checks=0;
#define CHECK(x) do{checks++;assert(x);}while(0)
 /* Fail-open decision table. Nothing here touches PCI, GCD or the loader. */
 CHECK(!fallback_allowed()); /* HF_EMPTY: nothing prepared, terminal */
 session.phase=HF_PREPARED;
 CHECK(fallback_allowed()&&!strcmp(fallback_reason(EFI_OUT_OF_RESOURCES),"writer_pool_unavailable"));
 CHECK(reason_is("pre_writer_refusal"));
 writer=&local; /* allocated, never entered */
 CHECK(fallback_allowed()&&reason_is("writer_not_entered"));
 local.status=PC_RUNNING;CHECK(!fallback_allowed());
 local.status=PC_READY_FOR_HANDOFF;CHECK(!fallback_allowed());
 local.status=PC_REFUSED;CHECK(!fallback_allowed()); /* refused with a foreign/absent session */
 local.env.handoff=&session;CHECK(fallback_allowed()&&reason_is("resource_proof_0_rejected"));
 local.coverage[0].coverage.status=RC_COVERED;CHECK(reason_is("gcd_reserve_refused"));
 local.reservation.status=GR_UNCERTAIN;CHECK(fallback_allowed()&&reason_is("gcd_uncertain_no_store"));
 local.reservation.status=GR_RELEASED;CHECK(reason_is("resource_proof_1_rejected"));
 local.coverage[1].coverage.status=RC_COVERED;CHECK(reason_is("plan_rejected"));
 for(unsigned t=0;t<PT_COUNT;t++)local.access[t].ready=1;
 CHECK(reason_is("pair_bind_rejected"));
 local.pair.bound=1;CHECK(reason_is("snapshot_mismatch"));
 local.matching_snapshots=1;CHECK(reason_is("writer_refused_before_mutation"));
 local.status=PC_COLD_RECOVERY;CHECK(fallback_allowed()); /* GCD-uncertain cold recovery with no store still falls open */
 /* Every possible-store marker and every loader-session transition is terminal. */
 for(unsigned t=0;t<PT_COUNT;t++){
  local.access[t].write_attempted=1;CHECK(!fallback_allowed());local.access[t].write_attempted=0;
  local.transaction[t].write_attempted=1;CHECK(!fallback_allowed());local.transaction[t].write_attempted=0;
 }
 local.pair.attempted=1;CHECK(!fallback_allowed());local.pair.attempted=0;
 local.reservation.pci_attempted=1;CHECK(!fallback_allowed());local.reservation.pci_attempted=0;
 session.mutation_started=1;CHECK(!fallback_allowed());session.mutation_started=0;
 session.start_called=1;CHECK(!fallback_allowed());session.start_called=0;
 for(unsigned ph=0;ph<=HF_FINAL_CHECKING;ph++){session.phase=(enum hf_phase)ph;CHECK(fallback_allowed()==(ph==HF_PREPARED));}
 session.phase=HF_PREPARED;CHECK(fallback_allowed());
 /* Console lines: the branch is unambiguous from one record. */
 EFI_SYSTEM_TABLE system={0};SIMPLE_TEXT_OUTPUT_INTERFACE console={0};
 console.OutputString=capture_output;system.ConOut=&console;st=&system;
 local.pair.attempted=0;fallback_report(EFI_COMPROMISED_DATA);CHECK(record_count==1);
 CHECK(strstr(records[0]," kind=HANDOFF_FALLBACK reason=writer_refused_before_mutation ")!=NULL);
 CHECK(strstr(records[0]," pci_mutation=0x0000000000000000")!=NULL&&strstr(records[0]," resize_applied=0x0000000000000000")!=NULL);
 CHECK(strstr(records[0]," attempted_stores=0x0000000000000000")!=NULL&&strstr(records[0]," cold_recovery_required=0x0000000000000000")!=NULL);
 CHECK(strstr(records[0]," writer_status=0x0000000000000003")!=NULL&&strstr(records[0]," gcd_status=0x0000000000000004")!=NULL);
 local.status=PC_COLD_RECOVERY;local.pair.attempted=13;session.mutation_started=1;refused_report(EFI_COMPROMISED_DATA);CHECK(record_count==2);
 CHECK(strstr(records[1]," kind=HANDOFF_REFUSED ")!=NULL&&strstr(records[1]," pci_mutation=0x0000000000000001")!=NULL);
 CHECK(strstr(records[1]," attempted_stores=0x000000000000000d")!=NULL&&strstr(records[1]," cold_recovery_required=0x0000000000000001")!=NULL);
 CHECK(strstr(records[1],"HANDOFF_FALLBACK")==NULL&&strstr(records[0],"HANDOFF_REFUSED")==NULL);
 session.mutation_started=0;local.pair.attempted=0;record_count=0;
 /* Original current_check guards. */
 memset(&local,0,sizeof local);memset(&session,0,sizeof session);writer=NULL;
 struct hf_binding binding={0};
 CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 writer=&local;CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 local.status=PC_RUNNING;CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 local.env.handoff=&session;CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_ACCESS_DENIED);
 session.phase=HF_MUTATION_CHECKING;
 CHECK(current_check(NULL,HF_CHECK_BEFORE_MUTATION,&binding)==EFI_COMPROMISED_DATA);
 CHECK(current_check(NULL,HF_CHECK_BEFORE_START,NULL)==EFI_COMPROMISED_DATA);
 CHECK(current_check(NULL,HF_CHECK_PREPARED,&binding)==EFI_COMPROMISED_DATA);
 CHECK(!session.mutation_started&&!session.start_called);
 for(unsigned t=0;t<PT_COUNT;t++)CHECK(local.access[t].write_attempted==0);
 /* Diagnostics render retained state only. A console is the only callback;
  * PCI/GCD service pointers remain null, proving no decorative device reads. */
 local.pair.attempted=19;local.applied_mask=1;
 local.transaction[0].failure.reason=2;local.transaction[1].failure.reason=3;local.transaction[3].failure.reason=5;
 writer_report("test");
 const unsigned per_target=4,snapshots=PC_SNAPSHOTS,coverage=2;
 CHECK(record_count==per_target*PT_COUNT+snapshots+coverage+PT_COUNT);
 for(unsigned t=0;t<PT_COUNT;t++){
  char index[48];snprintf(index,sizeof index,"target_index=0x%016x",t);
  for(unsigned j=0;j<per_target;j++)CHECK(strstr(records[t*per_target+j],index)!=NULL);
  CHECK(strstr(records[per_target*PT_COUNT+snapshots+coverage+t],index)!=NULL);
  CHECK(strstr(records[t*per_target],"attempted_stores=0x0000000000000013")!=NULL);
  CHECK(strstr(records[t*per_target],"applied_mask=0x0000000000000001")!=NULL);
 }
 CHECK(strstr(records[1],"reason=0x0000000000000002")!=NULL);
 CHECK(strstr(records[5],"reason=0x0000000000000003")!=NULL);
 CHECK(strstr(records[13],"reason=0x0000000000000005")!=NULL);
 for(unsigned i=0;i<snapshots;i++){char snap[48];snprintf(snap,sizeof snap,"snapshot=0x%016x",i);CHECK(strstr(records[per_target*PT_COUNT+i],snap)!=NULL);}
 printf("%u writer entrypoint, fail-open decision and indexed diagnostic checks PASS; synthetic console only\n",checks);return 0;
}
