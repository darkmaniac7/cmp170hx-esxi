#ifndef CMP_PCI_COORDINATOR_H
#define CMP_PCI_COORDINATOR_H
#include "resource_snapshot.h"
#include "../pins/resource_validate.h"
#define PC_POLICY_TRUSTED_FIRMWARE_R1 1U
enum pc_status {PC_NEW,PC_RUNNING,PC_REFUSED,PC_COLD_RECOVERY,PC_READY_FOR_HANDOFF};
struct pc_environment {
 EFI_BOOT_SERVICES *bs;GR_DXE *dxe;EFI_HANDLE image;
 struct hf_session *handoff;
 int (*readable)(void *,uintptr_t,size_t);void *read_context;
 int (*read_acpi)(void *,struct rc_acpi *);void *acpi_context;
 unsigned policy;
};
struct pc_pair_session {
 enum pc_status status;struct pc_environment env;
 struct pi_identity identities[RC_FUNCTIONS];
 struct pi_snapshot pci[5];struct rs_snapshot resources[2];
 struct rc_pair_result coverage[2];struct gr_pair_state reservation;
 struct pa_context access[2];struct bt_state transaction[2];
 struct pa_pair_state pair;unsigned applied_mask;
 unsigned trusted_firmware_aperture,matching_snapshots,final_checks;
 unsigned hardware_decode_observed,atomic_hardware_isolation_observed;
};
/* This policy trusts the pinned firmware-advertised aperture, stable repeated
 * observations and ordinary synchronous firmware operation. It does NOT prove
 * hardware decode or exclusion of SMM/other autonomous hardware actors. The
 * legacy BT_ROOT_DECODE_VALID/NO_CONCURRENT_ACTORS names are policy gates, not
 * claims of measured atomic isolation. No TPL change/driver disconnection. */
static int pc_handoff_valid(const struct pc_environment *e){
 static const uint8_t host[16]={0x15,0x2e,0xa0,0x5a,0xa2,0x0e,0x43,0xf0,0x8c,0x04,0x9c,0x6b,0,0xcf,0x57,0xa2};
 if(!e||e->policy!=PC_POLICY_TRUSTED_FIRMWARE_R1||!e->bs||!e->dxe||!e->image||
    !e->readable||!e->read_acpi||!e->handoff)return 0;
 const struct hf_session *h=e->handoff;
 return h->phase==HF_PREPARED&&h->status==HF_OK&&!h->mutation_started&&!h->start_called&&
  h->binding.parent==(uintptr_t)e->image&&h->binding.device&&h->child&&
  h->child!=h->binding.parent&&h->child!=h->binding.device&&
  !memcmp(h->binding.host_uuid,host,16)&&hf_nonzero(h->binding.boot_instance,16)&&
  hf_nonzero(h->binding.boot_sha256,32)&&hf_nonzero(h->binding.loader_sha256,32)&&h->binding.loader_size&&
  h->callbacks.check_current&&h->callbacks.inspect_child&&h->callbacks.start;
}
static void pc_pci_init(struct pc_pair_session *s,unsigned i){
 s->pci[i].bs=s->env.bs;s->pci[i].readable=s->env.readable;s->pci[i].read_context=s->env.read_context;
}
static int pc_collect(struct pc_pair_session *s,unsigned i){
 struct rc_acpi acpi={0};if(i>1)return 0;
 pc_pci_init(s,i);
 return pi_capture(&s->pci[i],s->identities)&&
  s->env.read_acpi(s->env.acpi_context,&acpi)==1&&
  rs_capture(&s->resources[i],&s->pci[i],&acpi)&&
  rc_validate_pair(&s->resources[i].view,&s->coverage[i])==RC_COVERED&&
  s->coverage[i].coverage.trusted_firmware_aperture_match==1&&
  s->coverage[i].coverage.standard_bar_collision_excluded==1&&
  !s->coverage[i].atomic_hardware_isolation_observed&&
  !s->coverage[i].coverage.hardware_authorized&&!s->coverage[i].coverage.hardware_decode_observed&&
  s->coverage[i].coverage.proposal_start==GR_BASE&&s->coverage[i].coverage.proposal_end==GR_LIMIT;
}
static uint16_t pc_pcie_offset(const struct pi_function *f,unsigned type){
 if(f->conventional.status!=PCI_CONV_NO_EA||f->conventional_count>PI_CONVENTIONAL_READS||
    (f->conventional_count&1)||f->conventional_count<8)return 0;
 uint16_t result=0;
 for(unsigned i=4;i<f->conventional_count/2;i++){
  const struct rc_read *r=&f->conventional_reads[i];
  if((r->value&255)!=0x10)continue;
  uint32_t flags=r->value>>16;
  if(result||r->status||r->offset<0x40||r->offset>0xf4||(r->offset&3)||
     (flags&15)<1||(flags&15)>2||((flags>>4)&15)!=type)return 0;
  result=(uint16_t)r->offset;
 }
 return result;
}
static int pc_plan(struct pc_pair_session *s,unsigned t){
 struct pi_snapshot *pi=&s->pci[1];struct pa_context *a=&s->access[t];
 const struct pair_target *target=&pair_targets[t];
 struct bt_plan *p=&a->plan;struct rc_result *r=&s->coverage[1].coverage;
 a->endpoint=pi->functions[pi->endpoint_indices[t]].device;a->bridge=pi->functions[pi->bridge_indices[t]].device;
 a->bs=s->env.bs;a->handoff=s->env.handoff;
 a->readable=s->env.readable;a->read_context=s->env.read_context;
 p->apply=1;p->target_index=t;p->endpoint=target->endpoint;p->bridge=target->port;
 p->bar1_base=target->bar1;p->bar1_size=target->bar1_size;p->bar3_base=target->bar3;p->bar3_size=target->bar3_size;
 p->bridge_base=target->bar1;p->bridge_limit=target->bar1+target->reservation_size-1;p->root_start=r->aperture_start;p->root_end=r->aperture_end;
 p->reservation_base=target->bar1;p->reservation_length=target->reservation_size;p->reservation_owner=(uintptr_t)s->env.image;
 p->pcie_cap=pc_pcie_offset(&pi->functions[pi->endpoint_indices[t]],0);
 p->bridge_pcie_cap=pc_pcie_offset(&pi->functions[pi->bridge_indices[t]],4);
 memcpy(p->endpoint_before,pi->functions[pi->endpoint_indices[t]].header,64);
 memcpy(p->bridge_before,pi->functions[pi->bridge_indices[t]].header,64);
 unsigned found=0;
 for(unsigned i=0;i<4;i++)if(pi->cmp[i].index==pi->endpoint_indices[t]){
  p->cya_before=pi->cmp[i].cya;p->cfg_before=pi->cmp[i].cfg;p->rebar_before=pi->cmp[i].rebar;found++;
 }
 if(found!=1||!p->pcie_cap||!p->bridge_pcie_cap||!pa_no_driver(a,&a->endpoint)||!pa_no_driver(a,&a->bridge))return 0;
 struct bt_io io={a,pa_read,pa_write,pa_owned};struct bt_state read_state={0};
 if(!bt_pcie(&io,&read_state,p->endpoint,p->pcie_cap,0)||!bt_pcie(&io,&read_state,p->bridge,p->bridge_pcie_cap,4))return 0;
 /* The remaining gates above derive from current collected data, the owned
  * reservation and prepared same-boot handoff. Only concurrency/root hardware
  * behavior rests on the explicitly named experimental firmware assumption. */
 p->proofs=BT_REQUIRED_PROOFS;
 if(!bt_plan_valid(p))return 0;
 a->ready=1;return 1;
}
static enum pc_status pc_fail(struct pc_pair_session *s){
 if(!s)return PC_REFUSED;
 s->final_checks=0;
 int mutated=s->pair.attempted||s->reservation.pci_attempted||
  s->access[0].write_attempted||s->access[1].write_attempted||
  s->transaction[0].write_attempted||s->transaction[1].write_attempted||
  (s->env.handoff&&s->env.handoff->mutation_started);
 /* Only a fully verified pair can be freed before mutation. An uncertain
  * Allocate/Free/GetMap/FreePool is terminal, with no speculative cleanup. */
 if(!mutated&&s->reservation.status==GR_OWNED)
  gr_pair_release_before_mutation(&s->reservation);
 if(s->env.handoff)hf_fail(s->env.handoff,HF_IO);
 s->status=mutated||s->reservation.status==GR_UNCERTAIN?PC_COLD_RECOVERY:PC_REFUSED;
 return s->status;
}
static int pc_pair_owners(struct pc_pair_session *s){
 for(unsigned t=0;t<2;t++){
  struct pa_context *a=&s->access[t];
  if(!pa_no_driver(a,&a->endpoint)||!pa_no_driver(a,&a->bridge)||!pa_owned(a,&a->plan))return 0;
 }
 return 1;
}
/* All five PCI observations compare cumulatively to snapshot0. Resource
 * proofs are cold-layout proofs, collected only before either transaction;
 * post-write firmware BAR-attribute caches are not treated as live BARs. */
static int pc_pair_snapshot(struct pc_pair_session *s,unsigned i,unsigned mask){
 pc_pci_init(s,i);
 return pi_capture(&s->pci[i],s->identities)&&
  pi_same_pair(&s->pci[0],&s->pci[i],s->pair.plans,mask)&&pc_pair_owners(s);
}
/* One retained session, two plans, one original same-boot handoff. The caller
 * prepares its exact loader before entry and keeps every context alive through
 * StartImage. No old pc_run/nr_start or direct bt_apply completion bypass. */
static enum pc_status pc_pair_run(struct pc_pair_session *s,const struct pc_environment *e){
 if(!s)return PC_REFUSED;
 if(s->status!=PC_NEW)return s->status;
 s->status=PC_RUNNING;
 if(!pc_handoff_valid(e)){s->status=PC_REFUSED;return s->status;}
 s->env=*e;
 for(unsigned i=0;i<RC_FUNCTIONS;i++)s->identities[i]=(struct pi_identity){rc_identities[i].bdf,rc_identities[i].id};
 if(!hf_check(e->handoff,HF_CHECK_PREPARED,HF_PREPARED)||!pc_collect(s,0))return pc_fail(s);
 struct gr_pair_state *g=&s->reservation;
 g->dxe=e->dxe;g->bs=e->bs;g->image=e->image;
 g->readable=e->readable;g->read_context=e->read_context;
 for(unsigned t=0;t<2;t++)g->slot[t].device=s->pci[0].functions[s->pci[0].endpoint_indices[t]].device.handle;
 if(gr_pair_reserve(g)!=GR_OWNED||!pc_collect(s,1)||
    !rs_same_resources(&s->resources[0],&s->resources[1]))return pc_fail(s);
 if(!pc_plan(s,0)||!pc_plan(s,1))return pc_fail(s);
 s->pair.reservation=g;s->pair.handoff=e->handoff;
 if(!pa_pair_bind(&s->pair,s->access)||
    !pi_same_pair(&s->pci[0],&s->pci[1],s->pair.plans,0))return pc_fail(s);
 s->matching_snapshots=1;s->trusted_firmware_aperture=1;
 if(hf_begin_mutation(e->handoff,1)!=HF_OK)return pc_fail(s);
 for(unsigned t=0;t<2;t++){
  if(pa_apply(&s->access[t],&s->transaction[t])!=BT_APPLIED)return pc_fail(s);
  /* Completion comes exclusively from pa_apply after all final readbacks. */
  s->applied_mask=s->pair.completed_mask;
  if(s->applied_mask!=(t?3U:1U)||!pc_pair_snapshot(s,t+2,s->applied_mask))return pc_fail(s);
 }
 if(s->pair.attempted!=26||hf_transaction_result(e->handoff,1,1)!=HF_OK)return pc_fail(s);
 s->final_checks=1;s->status=PC_READY_FOR_HANDOFF;return s->status;
}
/* Immediately before the original hf_start checks: fresh full PCI snapshot,
 * both GCD owners, and complete authority. Any returned child is terminal. */
static enum pc_status pc_pair_start(struct pc_pair_session *s){
 if(!s)return PC_REFUSED;
 if(s->status!=PC_READY_FOR_HANDOFF)return s->status;
 s->status=PC_RUNNING;
 if(!s->final_checks||s->applied_mask!=3||s->pair.completed_mask!=3||
    s->pair.failed||s->pair.attempted!=26||!s->env.handoff||
    s->env.handoff->phase!=HF_TRANSACTION_OK||!s->env.handoff->mutation_started||
    s->env.handoff->start_called)return pc_fail(s);
 for(unsigned t=0;t<2;t++)if(s->transaction[t].status!=BT_APPLIED||
    !s->transaction[t].write_attempted||!s->access[t].write_attempted||
    s->access[t].failed||s->access[t].next_write!=13)return pc_fail(s);
 if(!pc_pair_snapshot(s,4,3))return pc_fail(s);
 (void)hf_start(s->env.handoff,1);
 return pc_fail(s);
}
#endif
