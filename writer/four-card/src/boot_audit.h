#ifndef BOOT_AUDIT_R1_H
#define BOOT_AUDIT_R1_H
#include "handoff.h"
#define BA_MAX_HANDLES 128U
#define BA_MAX_INFO 1024U
#define BA_CHUNK 65536U
#define BA_UNSUPPORTED UINT64_C(0x8000000000000003)
#define BA_NOT_FOUND UINT64_C(0x800000000000000e)
struct ba_device {
    uintptr_t handle;
    uint64_t block_status,path_status,sfs_status,partition_status;
    uint32_t media_id,block_size,io_align;
    uint64_t last_block;
    unsigned media_present,logical_partition,removable,read_only;
    uint8_t path[HF_MAX_DP],partition_info[144];size_t path_size;
};
struct ba_callbacks {
    void *context;
    uint64_t (*current)(void *);
    uint64_t (*open)(void *,unsigned,uintptr_t *);
    uint64_t (*info)(void *,uintptr_t,uint8_t *,size_t *);
    uint64_t (*read)(void *,uintptr_t,uint8_t *,size_t *);
    uint64_t (*close)(void *,uintptr_t);
};
struct ba_file_result {
    unsigned complete,opened,close_attempted,read_calls;
    uint64_t current_before,open_status,info_before,read_status,eof_status,info_after,current_after,close_status,current_closed;
    uint64_t size,bytes_read;uint8_t digest[32],info_digest[32];
};
/* Entire path is bounded; no short-path expansion or controller discovery. */
static inline int ba_path(const uint8_t *p,size_t n,size_t *hd) {
    if(!p||!hd||n<4||n>HF_MAX_DP)return 0;
    *hd=SIZE_MAX;size_t off=0,nodes=0;
    while(off<n){
        if(n-off<4||++nodes>128)return 0;
        size_t len=hf_u16(p+off+2);
        if(len<4||len>n-off)return 0;
        if(p[off]==0x7f)return len==4&&off+4==n&&hf_end(p+off);
        if(p[off]<1||p[off]>5)return 0;
        if(p[off]==4&&p[off+1]==1){
            if(*hd!=SIZE_MAX||len!=42||off+46!=n)return 0;
            *hd=off;
        }
        off+=len;
    }
    return 0;
}
static inline int ba_unavailable(uint64_t s){return s==BA_UNSUPPORTED||s==BA_NOT_FOUND;}
static inline int ba_partition(const struct ba_device *d,size_t hd) {
    if(ba_unavailable(d->partition_status))return 1;
    if(d->partition_status)return 0;
    const uint8_t *p=d->partition_info;
    if(hf_u32(p)!=0x1000||hf_u32(p+4)>2||p[8]>1)return 0;
    for(unsigned i=9;i<16;i++)if(p[i])return 0;
    if(hf_u32(p+4)==2){
        if(hd==SIZE_MAX||!hf_hd_valid(d->path+hd))return 0;
        const uint8_t *h=d->path+hd;
        uint64_t first=hf_u64(p+48),last=hf_u64(p+56);
        if(memcmp(p+32,h+24,16)||first!=hf_u64(h+8)||last<first||last-first!=hf_u64(h+16)-1)return 0;
        static const uint8_t esp[]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
        if((!memcmp(p+16,esp,16))!=(p[8]==1))return 0;
    }
    return 1;
}
static inline int ba_select(const uint8_t *boot,size_t boot_size,uint32_t attrs,
    const struct ba_device *devices,size_t count,size_t *selected) {
    if(selected)*selected=SIZE_MAX;
    if(!selected||!devices||!count||count>BA_MAX_HANDLES||attrs!=7)return 0;
    struct hf_boot b;
    if(hf_parse_boot(boot,boot_size,&b)!=HF_OK)return 0;
    static const uint8_t expected[]={0xde,0x62,0x65,0x31,0xc2,0x80,0x29,0xd9,0xbc,0xd3,0xbb,0xfc,0x64,0xb3,0x85,0xe6,0x14,0x41,0x56,0x06,0x29,0x45,0xd1,0x84,0xbf,0xbb,0x82,0x3e,0x22,0x09,0x90,0xf0};
    uint8_t digest[32];inv_sha256(boot,boot_size,digest);if(memcmp(digest,expected,32))return 0;
    size_t found=SIZE_MAX,matches=0;
    for(size_t i=0;i<count;i++){
        const struct ba_device *d=devices+i;size_t hd;
        if(!d->handle||d->block_status||d->path_status||!ba_path(d->path,d->path_size,&hd)||
           (d->sfs_status&&!ba_unavailable(d->sfs_status))||!ba_partition(d,hd)||
           d->media_present>1||d->logical_partition>1||d->removable>1||d->read_only>1)return 0;
        for(size_t j=0;j<i;j++)if(d->handle==devices[j].handle||
            (d->path_size==devices[j].path_size&&!memcmp(d->path,devices[j].path,d->path_size)))return 0;
        if(hd!=SIZE_MAX&&!memcmp(d->path+hd,b.hd,42)){
            size_t checked;
            if(d->logical_partition!=1||d->media_present!=1||d->sfs_status||
               !hf_partition_path(d->path,d->path_size,&checked)||
               d->block_size<512||d->block_size>4096||(d->block_size&(d->block_size-1))||
               (d->io_align>1&&(d->io_align&(d->io_align-1)))||d->last_block!=b.partition_size-1||
               (!d->partition_status&&(hf_u32(d->partition_info+4)!=2||d->partition_info[8]!=1)))return 0;
            found=i;matches++;
        }
    }
    if(matches!=1)return 0;
    *selected=found;return 1;
}
static inline int ba_info(const uint8_t *b,size_t n,uint64_t *size){
    if(n<82||n>BA_MAX_INFO||hf_u64(b)!=n||(n&1)||hf_u16(b+n-2)!=0)return 0;
    uint64_t attr=hf_u64(b+72),length=hf_u64(b+8);
    if((attr&~UINT64_C(0x37))||(attr&0x10)||!length||length>HF_MAX_LOADER)return 0;
    for(size_t i=80;i+2<n;i+=2)if(hf_u16(b+i)==0)return 0;
    *size=length;return 1;
}
static inline int ba_hash_file(const struct ba_callbacks *c,unsigned index,uint8_t *buffer,size_t capacity,struct ba_file_result *r) {
    if(!r)return 0;
    memset(r,0,sizeof(*r));
    r->current_before=r->open_status=r->info_before=r->read_status=r->eof_status=r->info_after=
        r->current_after=r->close_status=r->current_closed=UINT64_MAX;
    if(!c||!c->current||!c->open||!c->info||!c->read||!c->close||index>2||!buffer)return 0;
    uintptr_t handle=0;unsigned okay=0;
    uint8_t before[BA_MAX_INFO] __attribute__((aligned(8))),after[BA_MAX_INFO] __attribute__((aligned(8))),hash[32],infohash[32];
    size_t before_n=sizeof before,after_n=sizeof after;uint64_t after_size=0;
    /* EFI_TIME padding may be left untouched by GetInfo. Define input bytes;
     * retain the exact full-byte comparison, including all returned fields. */
    memset(before,0,sizeof before);memset(after,0,sizeof after);
    r->current_before=c->current(c->context);if(r->current_before)return 0;
    r->open_status=c->open(c->context,index,&handle);
    if(r->open_status||!handle)goto cleanup;
    r->opened=1;r->info_before=c->info(c->context,handle,before,&before_n);
    if(r->info_before||!ba_info(before,before_n,&r->size)||r->size>capacity)goto cleanup;
    while(r->bytes_read<r->size){
        size_t requested=(size_t)(r->size-r->bytes_read);if(requested>BA_CHUNK)requested=BA_CHUNK;
        size_t got=requested;r->read_calls++;r->read_status=c->read(c->context,handle,buffer+r->bytes_read,&got);
        if(r->read_status||got!=requested)goto cleanup;
        r->bytes_read+=got;
    }
    uint8_t extra;size_t one=1;r->eof_status=c->read(c->context,handle,&extra,&one);
    if(r->eof_status||one!=0)goto cleanup;
    r->info_after=c->info(c->context,handle,after,&after_n);
    if(r->info_after||!ba_info(after,after_n,&after_size)||before_n!=after_n||memcmp(before,after,before_n))goto cleanup;
    r->current_after=c->current(c->context);if(r->current_after)goto cleanup;
    inv_sha256(buffer,(size_t)r->size,hash);inv_sha256(before,before_n,infohash);okay=1;
cleanup:
    if(handle){r->close_attempted=1;r->close_status=c->close(c->context,handle);if(r->close_status)okay=0;}
    if(okay){r->current_closed=c->current(c->context);if(r->current_closed)okay=0;}
    if(okay){memcpy(r->digest,hash,32);memcpy(r->info_digest,infohash,32);r->complete=1;}
    return r->complete;
}
#endif
