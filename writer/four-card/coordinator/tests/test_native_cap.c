#define main original_snapshot_main
#include "test_pci_snapshot.c"
#undef main
#include "../../fixtures/native_extended.h"
static void native_setup(void){
 setup();
 for(unsigned i=0;i<PI_FUNCTIONS;i++)if(identities[i].identity==0x20c210de)
  for(unsigned j=0;j<sizeof(native_pairs)/sizeof(native_pairs[0]);j++)config[i][native_pairs[j].offset/4]=native_pairs[j].before;
}
static void native_target(const struct bt_plan *p){
 changed_config(p);
 for(unsigned j=0;j<sizeof(native_pairs)/sizeof(native_pairs[0]);j++)config[before.endpoint_index][native_pairs[j].offset/4]=native_pairs[j].after;
}
int main(void){struct bt_plan plan;unsigned checks=0;
#define CHECK_N(x) do{checks++;assert(x);}while(0)
 native_setup();CHECK_N(pi_capture(&before,identities));make_plan(&plan);native_target(&plan);
 CHECK_N(pi_capture(&after,identities));CHECK_N(pi_same(&before,&after,&plan,1));
 CHECK_N(config[before.endpoint_index][0xbbc/4]==0x001ffc00U);
 CHECK_N(!pi_same(&before,&after,NULL,0));
 for(unsigned bit=0;bit<32;bit++){
  native_setup();CHECK_N(pi_capture(&before,identities));make_plan(&plan);native_target(&plan);
  config[before.endpoint_index][0xbbc/4]^=1U<<bit;
  CHECK_N(!pi_capture(&after,identities)||!pi_same(&before,&after,&plan,1));
 }
 for(unsigned card=0;card<4;card++)for(unsigned j=0;j<sizeof(native_pairs)/sizeof(native_pairs[0]);j++){
  native_setup();CHECK_N(pi_capture(&before,identities));make_plan(&plan);native_target(&plan);
  unsigned index=before.cmp[card].index,offset=native_pairs[j].offset;
  if(index==before.endpoint_index&&(offset==0xbbc||offset==0xbc0))continue;
  config[index][offset/4]^=1;
  CHECK_N(!pi_capture(&after,identities)||!pi_same(&before,&after,&plan,1));
 }
 printf("%u actual native CAP/all-card extended-preservation checks PASS\n",checks);return 0;}
