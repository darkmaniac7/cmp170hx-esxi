#define main baseline_snapshot_main
#include "test_pci_snapshot.c"
#undef main
#include "../../fixtures/native_extended.h"
#include "../../fixtures/second_target_extended.h"
static struct bt_plan plans[2];
static void pair_setup(void){
 setup();
 for(unsigned i=0;i<PI_FUNCTIONS;i++){
  if(identities[i].bdf==0x100)for(unsigned j=0;j<sizeof native_pairs/sizeof *native_pairs;j++)config[i][native_pairs[j].offset/4]=native_pairs[j].before;
  if(identities[i].bdf==0x200)for(unsigned j=0;j<sizeof second_target_trace/sizeof *second_target_trace;j++)config[i][second_target_trace[j].offset/4]=second_target_trace[j].value;
 }
 assert(pi_capture(&before,identities));make_plan(&plans[0]);
 plans[1]=plans[0];struct bt_plan *p=&plans[1];p->target_index=1;p->endpoint=0x200;p->bridge=0xb;
 p->bar1_base=pair_targets[1].bar1;p->bar3_base=pair_targets[1].bar3;p->bridge_base=p->reservation_base=p->bar1_base;p->bridge_limit=p->bar3_base+BT_BAR3_SIZE-1;
 memcpy(p->endpoint_before,before.functions[before.endpoint_indices[1]].header,64);
 memcpy(p->bridge_before,before.functions[before.bridge_indices[1]].header,64);
 for(unsigned i=0;i<4;i++)if(before.cmp[i].index==before.endpoint_indices[1]){p->rebar_before=before.cmp[i].rebar;p->cya_before=before.cmp[i].cya;p->cfg_before=before.cmp[i].cfg;}
}
static void delta(unsigned mask){
 for(unsigned t=0;t<2;t++)if(mask&(1U<<t)){
  const struct bt_plan *p=&plans[t];
  for(unsigned i=0;i<PI_FUNCTIONS;i++)if(identities[i].bdf==p->endpoint||identities[i].bdf==p->bridge)
   for(unsigned j=0;j<16;j++)config[i][j]=pi_header_expected(&before.functions[i],j,p,1);
  unsigned i=before.endpoint_indices[t];config[i][0xbbc/4]=BT_CFG_CAP_OBSERVED;
  config[i][0xbc0/4]=(config[i][0xbc0/4]&~0x1f00U)|(16U<<8);config[i][0x724/4]=0x30;config[i][0xdcc/4]=0x8000000a;
 }
 assert(pi_capture(&after,identities));
}
int main(void){unsigned checks=0;
#define P(x) do{checks++;assert(x);}while(0)
 for(unsigned mask=0;mask<=3;mask++){
  pair_setup();delta(mask);P(pi_same_pair(&before,&after,plans,mask)==(mask!=2));
  if(mask==2)continue;
  for(unsigned i=0;i<PI_FUNCTIONS;i++){
   for(unsigned j=0;j<16;j++){after.functions[i].header[j]^=1;P(!pi_same_pair(&before,&after,plans,mask));after.functions[i].header[j]^=1;}
   for(unsigned j=0;j<after.functions[i].conventional_count;j++){
    struct rc_read *r=&after.functions[i].conventional_reads[j];
    r->value^=1;P(!pi_same_pair(&before,&after,plans,mask));r->value^=1;
    r->offset^=4;P(!pi_same_pair(&before,&after,plans,mask));r->offset^=4;
    r->status=1;P(!pi_same_pair(&before,&after,plans,mask));r->status=0;
   }
   unsigned char *conv=(unsigned char *)&after.functions[i].conventional;
   for(unsigned j=0;j<sizeof after.functions[i].conventional;j+=sizeof(uint32_t)){
    uint32_t old,changed;memcpy(&old,conv+j,sizeof old);changed=old^1;
    memcpy(conv+j,&changed,sizeof changed);P(!pi_same_pair(&before,&after,plans,mask));memcpy(conv+j,&old,sizeof old);
   }
   for(unsigned j=0;j<16;j++){after.functions[i].header_present^=(uint16_t)(1U<<j);P(!pi_same_pair(&before,&after,plans,mask));after.functions[i].header_present^=(uint16_t)(1U<<j);}

   after.functions[i].raw_slot_present^=1;P(!pi_same_pair(&before,&after,plans,mask));after.functions[i].raw_slot_present^=1;
  }
  for(unsigned i=0;i<4;i++){
   after.cmp[i].cya^=1;P(!pi_same_pair(&before,&after,plans,mask));after.cmp[i].cya^=1;
   after.cmp[i].cfg^=1;P(!pi_same_pair(&before,&after,plans,mask));after.cmp[i].cfg^=1;
   for(unsigned j=0;j<after.cmp[i].extended_count;j++){
    struct rc_read *r=&after.cmp[i].extended_reads[j];
    r->value^=1;P(!pi_same_pair(&before,&after,plans,mask));r->value^=1;
    r->offset^=4;P(!pi_same_pair(&before,&after,plans,mask));r->offset^=4;
    r->status=1;P(!pi_same_pair(&before,&after,plans,mask));r->status=0;
   }
   for(unsigned j=0;j<after.cmp[i].rebar.count;j++){
    struct rebar_entry *r=&after.cmp[i].rebar.entries[j];
    r->offset^=4;P(!pi_same_pair(&before,&after,plans,mask));r->offset^=4;
    r->capability^=1;P(!pi_same_pair(&before,&after,plans,mask));r->capability^=1;
    r->control^=1;P(!pi_same_pair(&before,&after,plans,mask));r->control^=1;
   }
  }
 }
 pair_setup();delta(3);plans[1].target_index=0;P(!pi_same_pair(&before,&after,plans,3));
 printf("%u cumulative pair delta checks PASS (masks0/1/3, mask2 refused)\n",checks);return 0;
}
