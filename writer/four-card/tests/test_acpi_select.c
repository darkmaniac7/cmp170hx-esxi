#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/acpi_select.h"
static unsigned checks;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);exit(1);} }while(0)
static uint8_t bytes[512],pins[7][32];static struct ae_result r;static struct rc_acpi out;
static void fixsum(uint8_t *p){p[9]=0;uint8_t sum=0;for(unsigned i=0;i<36;i++)sum+=p[i];p[9]=(uint8_t)-sum;}
static void setup(void){
 memset(bytes,0,sizeof bytes);memset(&r,0,sizeof r);memset(&out,0,sizeof out);
 r.status=AE_OK;r.table_count=7;r.total_bytes=7*36;
 for(unsigned i=0;i<8;i++){
  uint8_t *p=bytes+i*36;memcpy(p,i?"SSDT":"DSDT",4);p[4]=36;p[8]=2;p[10]=(uint8_t)i;fixsum(p);
  r.tables[i]=(struct ae_table){i?AE_SSDT:AE_DSDT,0x1000+i*0x100,36,i*36,2};
  if(i<7)inv_sha256(p,36,pins[i]);
 }
}
int main(void){
 setup();CHECK(ws_select(&r,bytes,sizeof bytes,pins,&out)==1);CHECK(out.dsdt.data==bytes&&out.dsdt.size==36);
 for(unsigned i=0;i<6;i++)CHECK(out.ssdt[i].data==bytes+(i+1)*36);
 setup();r.table_count=8;r.total_bytes=8*36;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));CHECK(!out.dsdt.data);
 setup();r.tables[6]=r.tables[1];CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));CHECK(!out.dsdt.data);
 setup();r.status=AE_CHECKSUM;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.table_count=AE_MAX_TABLES+1;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.tables[1].workspace_offset=UINT32_MAX;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.tables[1].length=AE_TABLE_LIMIT+1;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.total_bytes=sizeof bytes+1;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();bytes[37]^=1;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();pins[1][0]^=1;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.tables[6].kind=AE_DSDT;memcpy(bytes+6*36,"DSDT",4);fixsum(bytes+6*36);CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.tables[0].kind=AE_SSDT;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.tables[2].workspace_offset=36;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.total_bytes--;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();r.route_authorized=1;CHECK(!ws_select(&r,bytes,sizeof bytes,pins,&out));
 setup();out.dsdt.data=bytes;CHECK(!ws_select(NULL,bytes,sizeof bytes,pins,&out));CHECK(!out.dsdt.data);
 setup();CHECK(!ws_select(&r,NULL,sizeof bytes,pins,&out));CHECK(!ws_select(&r,bytes,sizeof bytes,NULL,&out));
 printf("%u ACPI selection checks PASS\n",checks);return 0;
}
