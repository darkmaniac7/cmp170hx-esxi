#ifndef CMP_BAR_TRANSACTION_H
#define CMP_BAR_TRANSACTION_H
#include <stdint.h>
#include <stddef.h>
#include "pci_decode.h"
#include "two_card_policy.h"
#define BT_ENDPOINT 0x0100U
#define BT_BRIDGE 0x0009U
#define BT_BAR1_SIZE UINT64_C(0x1000000000)
#define BT_BAR3_SIZE UINT64_C(0x02000000)
#define BT_SPAN (BT_BAR1_SIZE+BT_BAR3_SIZE)
#define BT_MAX_ADDRESS UINT64_C(0xffffffffffff)
/* All four known successful native post-driver captures have this bitmap.
 * Immediate post-CFG timing is unobserved: require it conservatively here. */
#define BT_CFG_CAP_OBSERVED 0x001ffc00U
enum bt_proof {
 BT_LIVE_HOST_BOOT=1U<<0, BT_COMPLETE_INVENTORY=1U<<1,
 BT_EXCLUSIVE_SUBTREE=1U<<2, BT_NO_ACTIVE_DRIVERS=1U<<3,
 BT_NO_PENDING_TRANSACTIONS=1U<<4, BT_ROOT_DECODE_VALID=1U<<5,
 BT_NO_RESOURCE_COLLISIONS=1U<<6, BT_RESERVATION_VERIFIED=1U<<7,
 BT_NO_CONCURRENT_ACTORS=1U<<8
};
#define BT_REQUIRED_PROOFS 0x1ffU
/* Proof bits, all established by a separate live caller: host/current boot,
 * complete inventory, exclusive subtree, no active drivers, no transactions,
 * root decode/zero translation, no collisions, owned GCD reservation,
 * no concurrent firmware/PCI actor. These flags alone are NOT evidence. */
enum bt_status { BT_NEW, BT_REFUSED, BT_COLD_RECOVERY_REQUIRED, BT_APPLIED };
enum bt_failure {BT_FAILURE_NONE,BT_FAILURE_READ_ARGUMENT,BT_FAILURE_READ_RESULT,
 BT_FAILURE_ALL_ONES,BT_FAILURE_EQUAL,BT_FAILURE_WRITE_RESULT,BT_FAILURE_OWNERSHIP,
 BT_FAILURE_POST_CAP,BT_FAILURE_POST_CTRL,BT_FAILURE_POST_SIZE,BT_FAILURE_INVARIANT};
enum bt_stage {BT_STAGE_VALIDATE,BT_STAGE_SNAPSHOT,BT_STAGE_DISABLE,BT_STAGE_CYA,
 BT_STAGE_CFG,BT_STAGE_POST_CFG,BT_STAGE_RESIZE,BT_STAGE_BARS,BT_STAGE_BRIDGE,
 BT_STAGE_FINAL,BT_STAGE_RESTORE};
struct bt_diagnostic {
 uint32_t reason,stage,bdf,offset,width,expected_valid,expected,mask,observed_valid,observed;
 int callback_result;
};
struct bt_plan {
 uint32_t apply,proofs,endpoint,bridge,target_index;
 uint64_t bar1_base,bar1_size,bar3_base,bar3_size,bridge_base,bridge_limit;
 uint64_t root_start,root_end,root_translation,reservation_base,reservation_length,reservation_owner;
 uint16_t pcie_cap,bridge_pcie_cap;
 uint32_t endpoint_before[16],bridge_before[16],cya_before,cfg_before;
 struct cap_result rebar_before;
};
struct bt_io {
 void *context;
 int (*read)(void *,uint32_t,uint16_t,unsigned,uint32_t *);
 int (*write)(void *,uint32_t,uint16_t,unsigned,uint32_t);
 int (*owned)(void *,const struct bt_plan *);
};
struct bt_state {
 enum bt_status status;uint32_t write_attempted,reads,writes,ownership_checks,last_bdf,last_offset,last_width;
 uint32_t endpoint_snapshot[16],bridge_snapshot[16],cya_snapshot,cfg_snapshot;
 uint32_t rebar_wanted,bridge_wanted;
 uint32_t stage,last_observed_valid,last_observed;int last_callback_result;
 uint32_t cap_before,ctrl_before,cap_after,ctrl_after,cap_after_valid,ctrl_after_valid;
 struct bt_diagnostic failure;
};

/* First failure is immutable; no additional device reads for diagnostics. */
static int bt_failure(struct bt_state *s,unsigned reason,unsigned expected_valid,uint32_t expected,uint32_t mask)
{
 if(!s->failure.reason)s->failure=(struct bt_diagnostic){reason,s->stage,s->last_bdf,
  s->last_offset,s->last_width,expected_valid,expected,mask,s->last_observed_valid,
  s->last_observed,s->last_callback_result};
 return 0;
}

/* No EFI binding exists. This engine cannot establish caller proofs itself.
 * Callback failure may still have reached hardware; no rollback is attempted.
 * Reference: successful kernel-pci-source-r2/drivers/pci/cmp170hx-trial.c. */
static int bt_read(const struct bt_io *io,struct bt_state *s,uint32_t bdf,
                   uint16_t off,unsigned width,uint32_t *value)
{
 s->last_bdf=bdf;s->last_offset=off;s->last_width=width;s->reads++;
 s->last_observed_valid=0;s->last_observed=0;s->last_callback_result=0;
 if((width!=2&&width!=4)||(off&(width-1))||off>4096-width)return bt_failure(s,BT_FAILURE_READ_ARGUMENT,0,0,0);
 s->last_callback_result=io->read(io->context,bdf,off,width,value);
 if(s->last_callback_result!=(int)width)return bt_failure(s,BT_FAILURE_READ_RESULT,0,0,0);
 s->last_observed_valid=1;s->last_observed=*value;
 if(width==2?*value>=0xffffU:*value==UINT32_MAX)return bt_failure(s,BT_FAILURE_ALL_ONES,0,0,0);
 return 1;
}
static int bt_equal(const struct bt_io *io,struct bt_state *s,uint32_t bdf,
                    uint16_t off,unsigned width,uint32_t expected)
{uint32_t v;if(!bt_read(io,s,bdf,off,width,&v))return 0;
 return v==expected?1:bt_failure(s,BT_FAILURE_EQUAL,1,expected,width==2?0xffffU:UINT32_MAX);}
static int bt_owned(const struct bt_plan *p,const struct bt_io *io,struct bt_state *s)
{s->ownership_checks++;if(io->owned(io->context,p)==1)return 1;
 s->last_bdf=s->last_offset=s->last_width=s->last_observed_valid=0;
 return bt_failure(s,BT_FAILURE_OWNERSHIP,0,0,0);}
static int bt_write(const struct bt_io *io,struct bt_state *s,uint32_t bdf,
                    uint16_t off,unsigned width,uint32_t value)
{
 s->write_attempted=1;s->status=BT_COLD_RECOVERY_REQUIRED;
 s->last_bdf=bdf;s->last_offset=off;s->last_width=width;s->writes++;
 s->last_observed_valid=0;s->last_callback_result=io->write(io->context,bdf,off,width,value);
 if(s->last_callback_result!=(int)width)return bt_failure(s,BT_FAILURE_WRITE_RESULT,1,value,width==2?0xffffU:UINT32_MAX);
 return bt_equal(io,s,bdf,off,width,value);
}
static int bt_commands(const struct bt_plan *p,const struct bt_io *io,struct bt_state *s,uint32_t ep,uint32_t bridge)
{return bt_equal(io,s,p->endpoint,4,2,ep)&&bt_equal(io,s,p->bridge,4,2,bridge);}
static int bt_write_disabled(const struct bt_plan *p,const struct bt_io *io,struct bt_state *s,uint32_t bdf,
 uint16_t off,uint32_t value,uint32_t ep_command,uint32_t bridge_command)
{return bt_write(io,s,bdf,off,4,value)&&bt_commands(p,io,s,ep_command,bridge_command);}
static int bt_pending_clear(const struct bt_io *io,struct bt_state *s,uint32_t bdf,uint16_t cap)
{uint32_t v;return bt_read(io,s,bdf,(uint16_t)(cap+10),2,&v)&&!(v&0x20U);}
static int bt_pcie(const struct bt_io *io,struct bt_state *s,uint32_t bdf,uint16_t cap,unsigned type)
{
 uint32_t v;
 return bt_read(io,s,bdf,cap,2,&v)&&(v&255U)==0x10U&&
        bt_read(io,s,bdf,(uint16_t)(cap+2),2,&v)&&(v&15U)>=1&&(v&15U)<=2&&
        ((v>>4)&15U)==type&&bt_pending_clear(io,s,bdf,cap);
}
struct bt_cap_context {const struct bt_io *io;struct bt_state *state;uint32_t endpoint;};
static int bt_cap_read(void *context,uint32_t off,uint32_t *value)
{struct bt_cap_context *c=context;return !bt_read(c->io,c->state,c->endpoint,(uint16_t)off,4,value);}
static int bt_same_rebar(const struct cap_result *a,const struct cap_result *b)
{
 if(a->status!=CAP_FOUND||b->status!=CAP_FOUND||a->offset!=b->offset||a->count!=b->count||a->visited!=b->visited)return 0;
 for(uint32_t i=0;i<a->count;i++)if(a->entries[i].offset!=b->entries[i].offset||a->entries[i].capability!=b->entries[i].capability||a->entries[i].control!=b->entries[i].control)return 0;
 return 1;
}
static int bt_plan_valid(const struct bt_plan *p)
{
 if(!p||p->target_index>1)return 0;
 const struct pair_target *t=&pair_targets[p->target_index];
 if(p->apply!=1||p->proofs!=BT_REQUIRED_PROOFS||p->endpoint!=t->endpoint||p->bridge!=t->port||
    p->bar1_base!=t->bar1||p->bar3_base!=t->bar3)return 0;
 if(p->bar1_size!=BT_BAR1_SIZE||p->bar3_size!=BT_BAR3_SIZE||p->bar1_base<UINT64_C(0x100000000)||
    (p->bar1_base&(BT_BAR1_SIZE-1))||p->bar1_base>BT_MAX_ADDRESS-(BT_SPAN-1))return 0;
 if(p->bar3_base!=p->bar1_base+BT_BAR1_SIZE||p->bridge_base!=p->bar1_base||p->bridge_limit!=p->bar1_base+BT_SPAN-1||
    p->reservation_base!=p->bar1_base||p->reservation_length!=BT_SPAN||!p->reservation_owner||
    p->root_translation||p->root_start>p->bar1_base||p->root_end<p->bridge_limit||p->root_end>BT_MAX_ADDRESS)return 0;
 if(p->pcie_cap<0x40||(p->pcie_cap&3)||p->pcie_cap>0xf4||
    p->bridge_pcie_cap<0x40||(p->bridge_pcie_cap&3)||p->bridge_pcie_cap>0xf4)return 0;
 if(p->endpoint_before[0]!=0x20c210deU||p->bridge_before[0]!=0x14831022U||
    ((p->endpoint_before[3]>>16)&0x7f)!=0||((p->bridge_before[3]>>16)&0x7f)!=1||
    (p->bridge_before[2]>>16)!=0x0604U||(p->bridge_before[6]&0xffffffU)!=((t->bus<<16)|(t->bus<<8)))return 0;
 if((p->endpoint_before[1]&0xffffU)==0xffffU||(p->bridge_before[1]&0xffffU)==0xffffU)return 0;
 if((p->endpoint_before[4]&15U)!=0||!(p->endpoint_before[4]&~15U)||
    (p->endpoint_before[5]&15U)!=12||(p->endpoint_before[7]&15U)!=12||
    (p->bridge_before[9]&0x000f000fU)!=0x00010001U)return 0;
 return 1;
}
static enum bt_status bt_apply(const struct bt_plan *p,const struct bt_io *io,struct bt_state *s)
{
 uint32_t value,cap=0,ctrl=0,ep_disabled,br_disabled;int found=0,bar3_found=0;
 if(!s)return BT_REFUSED;
 if(s->status!=BT_NEW)return s->status;
 s->status=BT_REFUSED;
 if(!p||!io||!io->read||!io->write||!io->owned||!bt_plan_valid(p))return s->status;
 if(!bt_owned(p,io,s))goto fail;
 s->stage=BT_STAGE_SNAPSHOT;
 for(unsigned i=0;i<16;i++) {
  if(!bt_read(io,s,p->endpoint,(uint16_t)(i*4),4,&s->endpoint_snapshot[i])||s->endpoint_snapshot[i]!=p->endpoint_before[i])goto fail;
  if(!bt_read(io,s,p->bridge,(uint16_t)(i*4),4,&s->bridge_snapshot[i])||s->bridge_snapshot[i]!=p->bridge_before[i])goto fail;
 }
 if(!bt_pcie(io,s,p->endpoint,p->pcie_cap,0)||!bt_pcie(io,s,p->bridge,p->bridge_pcie_cap,4)||
    !bt_read(io,s,p->endpoint,0x724,4,&s->cya_snapshot)||s->cya_snapshot!=p->cya_before||
    !bt_read(io,s,p->endpoint,0xdcc,4,&s->cfg_snapshot)||s->cfg_snapshot!=p->cfg_before)goto fail;
 struct bt_cap_context cc={io,s,p->endpoint};struct cap_result rebar=decode_rebar(bt_cap_read,&cc);
 if(!bt_same_rebar(&rebar,&p->rebar_before)||!bt_read(io,s,p->endpoint,(uint16_t)rebar.offset,4,&value)||((value>>16)&15U)!=1)goto fail;
 for(uint32_t i=0;i<rebar.count;i++){
  if((rebar.entries[i].control&7U)==3){
   if(((rebar.entries[i].control>>8)&31U)!=5||!(rebar.entries[i].capability&(1U<<9)))goto fail;
   bar3_found=1;
  }
  if((rebar.entries[i].control&7U)!=1)continue;
  if(rebar.entries[i].offset!=0xbbc)goto fail;
  cap=rebar.entries[i].capability;ctrl=rebar.entries[i].control;found=1;
 }
 unsigned old_size=(ctrl>>8)&31U;
 if(!found||!bar3_found||old_size<6||old_size>19||!(cap&(1U<<(old_size+4)))||!bt_owned(p,io,s))goto fail;
 s->cap_before=cap;s->ctrl_before=ctrl;s->stage=BT_STAGE_DISABLE;
 ep_disabled=(p->endpoint_before[1]&0xffffU)&~6U;br_disabled=(p->bridge_before[1]&0xffffU)&~6U;
 if(!bt_write(io,s,p->endpoint,4,2,ep_disabled)||!bt_pending_clear(io,s,p->endpoint,p->pcie_cap)||
    !bt_write(io,s,p->bridge,4,2,br_disabled)||!bt_commands(p,io,s,ep_disabled,br_disabled)||
    !bt_pending_clear(io,s,p->bridge,p->bridge_pcie_cap))goto fail;
 s->stage=BT_STAGE_CYA;
 if(!bt_write_disabled(p,io,s,p->endpoint,0x724,0x30,ep_disabled,br_disabled))goto fail;
 s->stage=BT_STAGE_CFG;
 if(!bt_write_disabled(p,io,s,p->endpoint,0xdcc,0x8000000aU,ep_disabled,br_disabled))goto fail;
 s->stage=BT_STAGE_POST_CFG;
 if(!bt_read(io,s,p->endpoint,0xbbc,4,&cap))goto fail;
 s->cap_after=cap;s->cap_after_valid=1;
 if(cap!=BT_CFG_CAP_OBSERVED){bt_failure(s,BT_FAILURE_POST_CAP,1,BT_CFG_CAP_OBSERVED,UINT32_MAX);goto fail;}
 if(!bt_read(io,s,p->endpoint,0xbc0,4,&ctrl))goto fail;
 s->ctrl_after=ctrl;s->ctrl_after_valid=1;
 /* The proven native quirk rereads CTRL after CFG. Permit selected-size
  * changes only; BAR index/count, bit13 and every other bit must be unchanged. */
 if((ctrl&~0x1f00U)!=(s->ctrl_before&~0x1f00U)){
  bt_failure(s,BT_FAILURE_POST_CTRL,1,s->ctrl_before,~0x1f00U);goto fail;}
 unsigned post_size=(ctrl>>8)&31U;
 if(post_size<6||post_size>19||!(cap&(1U<<(post_size+4)))){
  bt_failure(s,BT_FAILURE_POST_SIZE,0,0,0x1f00U);goto fail;}
 if(!bt_commands(p,io,s,ep_disabled,br_disabled))goto fail;
 s->rebar_wanted=(ctrl&~0x1f00U)|(16U<<8);
 s->stage=BT_STAGE_RESIZE;
 if(!bt_write_disabled(p,io,s,p->endpoint,0xbc0,s->rebar_wanted,ep_disabled,br_disabled))goto fail;
 s->stage=BT_STAGE_BARS;
 if(!bt_write_disabled(p,io,s,p->endpoint,0x14,(uint32_t)p->bar1_base|(p->endpoint_before[5]&15U),ep_disabled,br_disabled)||
    !bt_write_disabled(p,io,s,p->endpoint,0x18,(uint32_t)(p->bar1_base>>32),ep_disabled,br_disabled)||
    !bt_write_disabled(p,io,s,p->endpoint,0x1c,(uint32_t)p->bar3_base|(p->endpoint_before[7]&15U),ep_disabled,br_disabled)||
    !bt_write_disabled(p,io,s,p->endpoint,0x20,(uint32_t)(p->bar3_base>>32),ep_disabled,br_disabled))goto fail;
 s->bridge_wanted=(uint32_t)((p->bridge_base>>16)&0xfff0U)|1U|
                   (((uint32_t)((p->bridge_limit>>16)&0xfff0U)|1U)<<16);
 s->stage=BT_STAGE_BRIDGE;
 if(!bt_write_disabled(p,io,s,p->bridge,0x24,s->bridge_wanted,ep_disabled,br_disabled)||
    !bt_write_disabled(p,io,s,p->bridge,0x28,(uint32_t)(p->bridge_base>>32),ep_disabled,br_disabled)||
    !bt_write_disabled(p,io,s,p->bridge,0x2c,(uint32_t)(p->bridge_limit>>32),ep_disabled,br_disabled))goto fail;
 /* Independently re-read all standard header fields. Only named changes may differ. */
 s->stage=BT_STAGE_FINAL;
 uint32_t ep[16],br[16];
 for(unsigned i=0;i<16;i++) {
  uint32_t ev=p->endpoint_before[i],bv=p->bridge_before[i];
  if(i==1){ev=(ev&0xffff0000U)|ep_disabled;bv=(bv&0xffff0000U)|br_disabled;}
  if(i==5)ev=(uint32_t)p->bar1_base|12U;
  if(i==6)ev=(uint32_t)(p->bar1_base>>32);
  if(i==7)ev=(uint32_t)p->bar3_base|12U;
  if(i==8)ev=(uint32_t)(p->bar3_base>>32);
  if(i==9)bv=s->bridge_wanted;
  if(i==10)bv=(uint32_t)(p->bridge_base>>32);
  if(i==11)bv=(uint32_t)(p->bridge_limit>>32);
  if(!bt_read(io,s,p->endpoint,(uint16_t)(i*4),4,&ep[i])||ep[i]!=ev||
     !bt_read(io,s,p->bridge,(uint16_t)(i*4),4,&br[i])||br[i]!=bv)goto fail;
 }
 uint64_t observed1=((uint64_t)ep[6]<<32)|(ep[5]&~15U),observed3=((uint64_t)ep[8]<<32)|(ep[7]&~15U);
 uint64_t observed_base=((uint64_t)br[10]<<32)|((uint64_t)(br[9]&0xfff0U)<<16);
 uint64_t observed_limit=((uint64_t)br[11]<<32)|((uint64_t)(br[9]&0xfff00000U))|0xfffffU;
 if(observed1!=p->bar1_base||observed3!=observed1+BT_BAR1_SIZE||observed_base!=observed1||observed_limit!=observed3+BT_BAR3_SIZE-1||
    !bt_equal(io,s,p->endpoint,0x724,4,0x30)||!bt_equal(io,s,p->endpoint,0xdcc,4,0x8000000aU)||
    !bt_equal(io,s,p->endpoint,0xbc0,4,s->rebar_wanted)||!bt_owned(p,io,s)||
    !bt_pending_clear(io,s,p->endpoint,p->pcie_cap)||!bt_pending_clear(io,s,p->bridge,p->bridge_pcie_cap))goto fail;
 /* Confirm the resizing operation did not alter other ReBAR controls/layouts. */
 struct cap_result final_rebar=decode_rebar(bt_cap_read,&cc);
 if(final_rebar.status!=CAP_FOUND||final_rebar.offset!=rebar.offset||final_rebar.count!=rebar.count||final_rebar.visited!=rebar.visited||
    !bt_read(io,s,p->endpoint,(uint16_t)final_rebar.offset,4,&value)||((value>>16)&15U)!=1)goto fail;
 for(uint32_t i=0;i<rebar.count;i++){
  uint32_t expected=rebar.entries[i].control;
  if((expected&7U)==1)expected=s->rebar_wanted;
  if(final_rebar.entries[i].offset!=rebar.entries[i].offset||final_rebar.entries[i].control!=expected)goto fail;
  unsigned selected=(expected>>8)&31U;
  if(selected>19||!(final_rebar.entries[i].capability&(1U<<(selected+4))))goto fail;
 }
 /* Restore only parent forwarding after every mapping check. Endpoint stays off. */
 s->stage=BT_STAGE_RESTORE;
 if(!bt_write(io,s,p->bridge,4,2,p->bridge_before[1]&0xffffU)||
    !bt_commands(p,io,s,ep_disabled,p->bridge_before[1]&0xffffU))goto fail;
 s->status=BT_APPLIED;return s->status;
fail:
 if(!s->failure.reason)bt_failure(s,BT_FAILURE_INVARIANT,0,0,0);
 s->status=s->write_attempted?BT_COLD_RECOVERY_REQUIRED:BT_REFUSED;
 return s->status;
}
#endif
