#ifndef CMP_BOOT_CURRENT_H
#define CMP_BOOT_CURRENT_H
#include <stdint.h>
#include <stddef.h>
struct bc_state {unsigned initialized;uint16_t value;uint32_t attributes;uint64_t last_status;size_t last_size;};
typedef uint64_t (*bc_read_fn)(void *,uint8_t *,size_t *,uint32_t *);
static inline int bc_check(struct bc_state *s,void *context,bc_read_fn read){
    if(!s||!read)return 0;
    uint8_t raw[2]={0};size_t size=sizeof raw;uint32_t attributes=0;
    uint64_t status=read(context,raw,&size,&attributes);
    s->last_status=status;s->last_size=size;
    if(status||size!=sizeof raw||attributes!=6)return 0;
    uint16_t value=(uint16_t)raw[0]|(uint16_t)((uint16_t)raw[1]<<8);
    if(value==UINT16_MAX)return 0;
    if(s->initialized)return s->initialized==1&&s->value==value&&s->attributes==attributes;
    s->value=value;s->attributes=attributes;s->initialized=1;return 1;
}
#endif
