#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "../src/boot_current.h"
static uint64_t status;
static size_t count=2;
static uint32_t attrs=6;
static unsigned value=4,calls;
static uint64_t read_current(void *context,uint8_t *data,size_t *size,uint32_t *attributes){
    (void)context;assert(*size==2);calls++;
    data[0]=(uint8_t)value;data[1]=(uint8_t)(value>>8);*size=count;*attributes=attrs;return status;
}
int main(void){
    struct bc_state s={0};
    assert(bc_check(&s,NULL,read_current)==1);assert(s.value==4&&s.initialized==1&&calls==1);
    assert(bc_check(&s,NULL,read_current)==1);
    value=3;assert(bc_check(&s,NULL,read_current)==0);value=4;
    attrs=7;assert(bc_check(&s,NULL,read_current)==0);attrs=6;
    count=1;assert(bc_check(&s,NULL,read_current)==0);count=3;assert(bc_check(&s,NULL,read_current)==0);count=2;
    status=UINT64_C(0x800000000000000e);assert(bc_check(&s,NULL,read_current)==0);status=0;
    assert(bc_check(NULL,NULL,read_current)==0);assert(bc_check(&s,NULL,NULL)==0);
    s=(struct bc_state){0};value=65535;assert(bc_check(&s,NULL,read_current)==0);assert(s.initialized==0);
    for(unsigned i=0;i<4;i++){s=(struct bc_state){0};value=4;attrs=(unsigned[]){0,2,4,7}[i];assert(!bc_check(&s,NULL,read_current));assert(!s.initialized);}
    s=(struct bc_state){0};value=3;attrs=6;assert(bc_check(&s,NULL,read_current));assert(s.value==3);
    puts("15 BootCurrent scenarios PASS");return 0;
}
