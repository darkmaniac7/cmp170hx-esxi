#define main original_access_tests_main
#include "test_pci_access.c"
#undef main
static int automatic_cfg(void *ctx,uint32_t bdf,uint16_t off,unsigned width,uint32_t value){
 int n=pa_write(ctx,bdf,off,width,value);
 if(n==(int)width&&bdf==BT_ENDPOINT&&off==0xdcc)config[0][0xbc0/4]=0x3001;
 return n;
}
int main(void){unsigned checks=0;uint32_t value=0;
#define CHECK_D(x) do{checks++;assert(x);}while(0)
 for(unsigned fail=1;fail<=3;fail++)for(unsigned ones=0;ones<2;ones++){
  if(fail==1&&ones)continue;
  setup();fail_at=fail;fail_allones=ones;
  CHECK_D(pa_read(&c,BT_ENDPOINT,0xbc0,4,&value)==0);
  CHECK_D(c.failure.reason==(fail==1?PA_FAILURE_LOCATION:fail==2?PA_FAILURE_IDENTITY:ones?PA_FAILURE_ALL_ONES:PA_FAILURE_READ));
  CHECK_D(c.failure.status_valid&&c.failure.status==(ones?0:EFI_DEVICE_ERROR));
  CHECK_D(c.failure.bdf==BT_ENDPOINT&&c.failure.offset==(fail<3?0:0xbc0));
  CHECK_D(c.failure.observed_valid==ones);
  if(ones)CHECK_D(c.failure.observed==UINT32_MAX);
  struct pa_diagnostic original=c.failure;
  (void)pa_read(&c,BT_ENDPOINT,0,4,&value);CHECK_D(!memcmp(&original,&c.failure,sizeof original));
 }
 setup();CHECK_D(pa_write(&c,BT_ENDPOINT,4,2,1)==2);unsigned at=calls;
 setup();fail_at=at;CHECK_D(pa_write(&c,BT_ENDPOINT,4,2,1)==0&&writes==1);
 CHECK_D(c.failure.reason==PA_FAILURE_WRITE&&c.failure.status_valid&&c.failure.status==EFI_DEVICE_ERROR);
 CHECK_D(c.failure.expected_valid&&c.failure.expected==1&&!c.failure.observed_valid&&c.failure.bdf==BT_ENDPOINT&&c.failure.offset==4&&c.failure.width==2);
 struct pa_diagnostic original=c.failure;
 CHECK_D(!pa_write(&c,BT_ENDPOINT,0x724,4,0x30)&&writes==1&&!memcmp(&original,&c.failure,sizeof original));
 setup();c.next_write=4;
 CHECK_D(!pa_write(&c,BT_ENDPOINT,0xbc0,4,0x1001)&&writes==0&&c.failure.reason==PA_FAILURE_WRITE_GUARD);
 CHECK_D(c.failure.expected_valid&&c.failure.expected==0x1001&&!c.failure.status_valid);
 setup();struct bt_state state={0};struct bt_io io={&c,pa_read,automatic_cfg,pa_owned};
 CHECK_D(bt_apply(&c.plan,&io,&state)==BT_APPLIED&&writes==13&&c.next_write==13);
 CHECK_D(state.ctrl_before==0x2601&&state.ctrl_after==0x3001&&state.rebar_wanted==0x3001&&!c.failure.reason);
 CHECK_D(!pa_expected_write(&c,BT_ENDPOINT,0xbc0,4,0x3001));
 printf("%u PCI_IO exact-status/first-failure/whitelist checks PASS\n",checks);return 0;}
