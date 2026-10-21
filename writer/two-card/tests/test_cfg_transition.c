/* Synthetic CFG-selected-size transitions; no physical observations implied. */
#define main original_transaction_tests_main
#include "test_bar_transaction.c"
#undef main
static unsigned transition,transition_enabled,after_cap,inject;
static int transition_write(void *ctx,uint32_t bdf,uint16_t off,unsigned width,uint32_t value){
 int n=write_cfg(ctx,bdf,off,width,value);
 if(transition_enabled&&bdf==((struct fake *)ctx)->endpoint&&off==0xdcc){
  struct fake *f=ctx;f->ep[0xbc0/4]=transition;f->ep[0xbbc/4]=after_cap;
  if(inject==7)return (int)width-1;
 }
 return n;
}
static int transition_read(void *ctx,uint32_t bdf,uint16_t off,unsigned width,uint32_t *value){
 int n=read_cfg(ctx,bdf,off,width,value);struct fake *f=ctx;
 if(bdf==((struct fake *)ctx)->endpoint&&f->ep[0xdcc/4]==0x8000000aU){
  if(inject==1&&off==0xdcc)*value^=1;
  if(inject==2&&off==0xdcc)return (int)width-1;
  if(inject==3&&off==0xbbc)*value=UINT32_MAX;
  if(inject==4&&off==0xbbc)return (int)width-1;
  if(inject==5&&off==0xbc0)*value=UINT32_MAX;
  if(inject==6&&off==0xbc0)return (int)width-1;
 }
 return n;
}
static void reset(struct fake *f,struct bt_plan *p,struct bt_state *s){
 fixture(f,p);memset(s,0,sizeof *s);transition_enabled=1;transition=0x3001;after_cap=BT_CFG_CAP_OBSERVED;inject=0;
}
int main(void){
 unsigned checks=0;for(target_index=0;target_index<2;target_index++){struct fake f;struct bt_plan p;struct bt_state s;
#define CHECK(x) do{checks++;assert(x);}while(0)
 struct bt_io io={&f,transition_read,transition_write,owned};
 for(unsigned size=6;size<=16;size++){
  reset(&f,&p,&s);transition=0x2001|(size<<8);
  CHECK(bt_apply(&p,&io,&s)==BT_APPLIED);CHECK(writes(&f)==13&&f.ep[0xbc0/4]==0x3001);
  CHECK(s.ctrl_before==0x2601&&s.ctrl_after_valid&&s.ctrl_after==transition&&s.cap_after_valid&&s.cap_after==after_cap);
  CHECK(!s.failure.reason&&s.rebar_wanted==0x3001&&f.br[1]==7&&!(f.ep[1]&6));
 }
 for(unsigned bit=0;bit<32;bit++)if(!(0x1f00U&(1U<<bit))){
  reset(&f,&p,&s);transition^=1U<<bit;
  CHECK(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&writes(&f)==4);
  CHECK(s.failure.reason==BT_FAILURE_POST_CTRL&&s.failure.bdf==p.endpoint&&s.failure.offset==0xbc0&&s.failure.width==4);
  CHECK(s.failure.expected_valid&&s.failure.expected==0x2601&&s.failure.mask==~0x1f00U&&s.failure.observed_valid&&s.failure.observed==transition);
 }
 for(unsigned size=0;size<32;size++){
  if(size>=6&&size<=16)continue;
  reset(&f,&p,&s);transition=0x2001|(size<<8);
  CHECK(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&writes(&f)==4&&s.failure.reason==BT_FAILURE_POST_SIZE);
 }
 reset(&f,&p,&s);after_cap=1U<<10;
 CHECK(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&writes(&f)==4&&s.failure.reason==BT_FAILURE_POST_CAP);
 CHECK(s.failure.expected==BT_CFG_CAP_OBSERVED&&s.failure.mask==UINT32_MAX&&s.failure.observed==after_cap&&!s.ctrl_after_valid);
 reset(&f,&p,&s);transition=0x3101;
 CHECK(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&writes(&f)==4&&s.failure.reason==BT_FAILURE_POST_SIZE);
 for(unsigned bit=0;bit<32;bit++){
  reset(&f,&p,&s);after_cap=BT_CFG_CAP_OBSERVED^(1U<<bit);
  CHECK(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&writes(&f)==4&&s.failure.reason==BT_FAILURE_POST_CAP);
  CHECK(s.cap_after_valid&&s.cap_after==after_cap&&s.failure.expected==BT_CFG_CAP_OBSERVED&&s.failure.mask==UINT32_MAX);
 }
 for(unsigned bad=1;bad<=7;bad++){
  reset(&f,&p,&s);inject=bad;
  CHECK(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&writes(&f)==4);
  CHECK(s.failure.bdf==p.endpoint&&s.failure.width==4&&s.failure.offset==(bad<=2||bad==7?0xdcc:bad<=4?0xbbc:0xbc0));
  CHECK(s.failure.reason==(bad==1?BT_FAILURE_EQUAL:bad==3||bad==5?BT_FAILURE_ALL_ONES:bad==7?BT_FAILURE_WRITE_RESULT:BT_FAILURE_READ_RESULT));
  CHECK(s.failure.observed_valid==(bad==1||bad==3||bad==5));
  if(bad==1)CHECK(s.failure.expected_valid&&s.failure.expected==0x8000000aU&&s.failure.observed==0x8000000bU);
  if(bad==3||bad==5)CHECK(s.failure.observed==UINT32_MAX);
  struct bt_diagnostic first=s.failure;unsigned before=f.n;
  CHECK(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&f.n==before);
  bt_failure(&s,BT_FAILURE_INVARIANT,0,0,0);CHECK(!memcmp(&first,&s.failure,sizeof first));
 }
 }printf("%u CFG transition and first-failure diagnostic checks PASS\n",checks);return 0;
}
