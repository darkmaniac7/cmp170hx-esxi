#ifndef CMP_HANDOFF_R1_H
#define CMP_HANDOFF_R1_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "inventory_sha256.h"
#define HF_MAX_BOOT 1024U
#define HF_MAX_DP 1024U
#define HF_MAX_FILE_DP 512U
#define HF_MAX_FULL_DP (HF_MAX_DP + HF_MAX_FILE_DP)
#define HF_MAX_OPTIONS 256U
#define HF_MAX_PARTITIONS 128U
#define HF_MAX_LOADER (16U * 1024U * 1024U)
enum hf_status { HF_OK, HF_INVALID, HF_AMBIGUOUS, HF_IO, HF_HASH, HF_STATE, HF_RECOVERY_REQUIRED };
struct hf_boot {
    uint32_t attributes; uint32_t partition_number;
    uint64_t partition_start, partition_size;
    uint8_t guid[16], hd[42], file_dp[HF_MAX_FILE_DP], options[HF_MAX_OPTIONS];
    size_t file_dp_size, options_size;
};
struct hf_partition {
    uintptr_t handle; const uint8_t *device_path; size_t device_path_size;
    unsigned existing, simple_fs, block_io, media_present, logical_partition;
    uint32_t block_size, media_id; uint64_t last_block;
};

static inline uint16_t hf_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }
static inline uint32_t hf_u32(const uint8_t *p) { return (uint32_t)hf_u16(p) | (uint32_t)hf_u16(p+2)<<16; }
static inline uint64_t hf_u64(const uint8_t *p) { return (uint64_t)hf_u32(p) | (uint64_t)hf_u32(p+4)<<32; }
static inline int hf_nonzero(const uint8_t *p, size_t n) {
    uint8_t v=0; for(size_t i=0;i<n;i++) v|=p[i]; return v!=0;
}
static inline int hf_hd_valid(const uint8_t *p) {
    return p[0]==4 && p[1]==1 && hf_u16(p+2)==42 && hf_u32(p+4)!=0 &&
        hf_u64(p+8)!=0 && hf_u64(p+16)!=0 &&
        hf_u64(p+8)<=UINT64_MAX-(hf_u64(p+16)-1) &&
        hf_nonzero(p+24,16) && p[40]==2 && p[41]==2;
}
static inline int hf_end(const uint8_t *p) { return !memcmp(p,"\x7f\xff\x04\0",4); }
/* Deliberately accepts only bounded, absolute ASCII-in-UTF16 paths, no aliases. */
static inline int hf_file_valid(const uint8_t *p, size_t n) {
    if(n<10 || n>HF_MAX_FILE_DP || p[0]!=4 || p[1]!=4 || hf_u16(p+2)!=n-4 ||
       ((n-8)&1) || !hf_end(p+n-4)) return 0;
    size_t end=n-6;
    if(p[4]!='\\' || p[5]!=0 || p[end]!=0 || p[end+1]!=0) return 0;
    size_t segment=6;
    for(size_t i=6;i<end;i+=2) {
        if(p[i+1] || p[i]<0x21 || p[i]>0x7e || p[i]=='/' || p[i]==':') return 0;
        if(p[i]=='\\') {
            if(i==segment || (i-segment==2 && p[segment]=='.') ||
               (i-segment==4 && p[segment]=='.' && p[segment+2]=='.')) return 0;
            segment=i+2;
        }
    }
    return end>segment && !(end-segment==2 && p[segment]=='.') &&
        !(end-segment==4 && p[segment]=='.' && p[segment+2]=='.');
}
static inline enum hf_status hf_parse_boot(const uint8_t *raw, size_t size, struct hf_boot *out) {
    struct hf_boot parsed={0};
    if(out) memset(out,0,sizeof(*out));
    if(!raw || !out || size<6 || size>HF_MAX_BOOT || hf_u32(raw)!=1) return HF_INVALID;
    size_t pos=6, chars=0;
    for(;;) {
        if(pos+2>size || chars++>64) return HF_INVALID;
        unsigned ch=hf_u16(raw+pos);pos+=2;
        if(!ch) break;
        if(ch<0x20 || ch>0x7e) return HF_INVALID;
    }
    size_t dp=hf_u16(raw+4);
    if(dp<52 || dp>size-pos || dp>42+HF_MAX_FILE_DP ||
       !hf_hd_valid(raw+pos) || !hf_file_valid(raw+pos+42,dp-42)) return HF_INVALID;
    size_t optional=size-pos-dp;
    if(optional>HF_MAX_OPTIONS) return HF_INVALID;
    parsed.attributes=1;parsed.partition_number=hf_u32(raw+pos+4);
    parsed.partition_start=hf_u64(raw+pos+8);parsed.partition_size=hf_u64(raw+pos+16);
    memcpy(parsed.hd,raw+pos,42);memcpy(parsed.guid,raw+pos+24,16);
    parsed.file_dp_size=dp-42;memcpy(parsed.file_dp,raw+pos+42,parsed.file_dp_size);
    parsed.options_size=optional;memcpy(parsed.options,raw+pos+dp,optional);
    *out=parsed;return HF_OK;
}
/* A fully expanded partition path must have a hardware/ACPI/messaging prefix,
 * exactly one terminal GPT HD node, and one End Entire. Never expand short paths. */
static inline int hf_partition_path(const uint8_t *path, size_t size, size_t *hd_offset) {
    if(!path || !hd_offset || size<50 || size>HF_MAX_DP) return 0;
    size_t pos=0,nodes=0;
    while(pos<size) {
        if(size-pos<4 || ++nodes>64) return 0;
        size_t n=hf_u16(path+pos+2);
        if(n<4 || n>size-pos) return 0;
        if(path[pos]==4) {
            if(!pos || n!=42 || size-pos!=46 || !hf_hd_valid(path+pos) || !hf_end(path+pos+42)) return 0;
            *hd_offset=pos;return 1;
        }
        if(path[pos]<1 || path[pos]>3) return 0;
        pos+=n;
    }
    return 0;
}
static inline enum hf_status hf_match_partition(const struct hf_boot *boot,
    const struct hf_partition *partitions, size_t count, size_t *selected) {
    if(selected) *selected=SIZE_MAX;
    if(!boot || !partitions || !selected || !count || count>HF_MAX_PARTITIONS || !hf_hd_valid(boot->hd)) return HF_INVALID;
    size_t matches=0,index=0;
    for(size_t i=0;i<count;i++) {
        const struct hf_partition *p=&partitions[i];size_t hd=0;
        if(!p->handle || p->existing!=1 || p->simple_fs!=1 || p->block_io!=1 || p->media_present!=1 ||
           p->logical_partition!=1 || p->block_size<512 || p->block_size>4096 ||
           (p->block_size&(p->block_size-1)) || !hf_partition_path(p->device_path,p->device_path_size,&hd) ||
           p->last_block!=hf_u64(p->device_path+hd+16)-1) return HF_INVALID;
        for(size_t j=0;j<i;j++)
            if(p->handle==partitions[j].handle ||
               (p->device_path_size==partitions[j].device_path_size &&
                !memcmp(p->device_path,partitions[j].device_path,p->device_path_size))) return HF_AMBIGUOUS;
        if(!memcmp(p->device_path+hd,boot->hd,42)) { index=i; ++matches; }
    }
    if(matches!=1) return matches?HF_AMBIGUOUS:HF_INVALID;
    *selected=index;return HF_OK;
}

enum hf_phase { HF_EMPTY, HF_PREPARING, HF_PREPARED, HF_MUTATING, HF_TRANSACTION_OK,
    HF_STARTING, HF_REJECTED, HF_COLD_RECOVERY, HF_MUTATION_CHECKING, HF_FINAL_CHECKING };
enum hf_check_stage { HF_CHECK_PRE_LOAD, HF_CHECK_PREPARED, HF_CHECK_BEFORE_MUTATION, HF_CHECK_BEFORE_START };
struct hf_pins {
    uint8_t boot_sha256[32], loader_sha256[32], host_uuid[16], boot_instance[16];
    const uint8_t *partition_path; size_t partition_path_size, loader_size;
    uint32_t block_size;
    unsigned partition_binding_verified;
};
struct hf_binding {
    uint8_t host_uuid[16],boot_instance[16],boot_sha256[32],loader_sha256[32],options_sha256[32];
    uintptr_t parent,device;uint32_t block_size,media_id;uint64_t last_block;
    uint8_t partition_path[HF_MAX_DP],file_dp[HF_MAX_FILE_DP],full_image_path[HF_MAX_FULL_DP];
    size_t partition_path_size,file_dp_size,full_image_path_size,loader_size;
};
struct hf_child {
    uintptr_t parent,device;
    const uint8_t *file_dp,*options; size_t file_dp_size,options_size;
};
struct hf_callbacks {
    void *context;
    uint64_t (*check_current)(void *,enum hf_check_stage,const struct hf_binding *);
    uint64_t (*read_loader)(void *,const struct hf_binding *,uint8_t *,size_t,size_t *);
    uint64_t (*load)(void *,unsigned,uintptr_t,const uint8_t *,size_t,const uint8_t *,size_t,uintptr_t *);
    uint64_t (*inspect_child)(void *,uintptr_t,struct hf_child *);
    uint64_t (*set_ram_options)(void *,uintptr_t,const uint8_t *,size_t);
    uint64_t (*start)(void *,uintptr_t);
};
struct hf_request {
    const uint8_t *boot_raw;size_t boot_raw_size;uint32_t boot_variable_attributes;
    const struct hf_pins *pins;uint8_t host_uuid[16],boot_instance[16];
    const struct hf_partition *partitions;size_t partition_count;uintptr_t parent;
    uint8_t *loader_buffer;size_t loader_buffer_capacity;
};
struct hf_session {
    enum hf_phase phase;enum hf_status status;unsigned mutation_started,start_called;
    uintptr_t child;uint64_t last_callback_status,start_return_status;
    struct hf_binding binding;struct hf_callbacks callbacks;
    uint8_t options[HF_MAX_OPTIONS];size_t options_size;
};

static inline enum hf_status hf_fail(struct hf_session *s,enum hf_status status) {
    if(!s) return HF_INVALID;
    if(s->mutation_started || s->start_called || s->phase==HF_COLD_RECOVERY) {
        s->phase=HF_COLD_RECOVERY;s->status=HF_RECOVERY_REQUIRED;
    } else { s->phase=HF_REJECTED;s->status=status; }
    return s->status;
}
static inline int hf_callback_ok(struct hf_session *s,uint64_t status,enum hf_phase expected) {
    s->last_callback_status=status;
    if(status || s->phase!=expected) { hf_fail(s,HF_IO);return 0; }
    return 1;
}
static inline int hf_check(struct hf_session *s,enum hf_check_stage stage,enum hf_phase phase) {
    return hf_callback_ok(s,s->callbacks.check_current(s->callbacks.context,stage,&s->binding),phase);
}
static inline int hf_child_valid(struct hf_session *s,enum hf_phase phase,unsigned options_required) {
    struct hf_child child={0};
    uint8_t digest[32];
    if(s->options_size>HF_MAX_OPTIONS) { hf_fail(s,HF_INVALID);return 0; }
    inv_sha256(s->options,s->options_size,digest);
    if(memcmp(digest,s->binding.options_sha256,32)) { hf_fail(s,HF_HASH);return 0; }
    if(!hf_callback_ok(s,s->callbacks.inspect_child(s->callbacks.context,s->child,&child),phase)) return 0;
    if(child.parent!=s->binding.parent || child.device!=s->binding.device || !child.file_dp ||
       child.file_dp_size!=s->binding.file_dp_size ||
       memcmp(child.file_dp,s->binding.file_dp,child.file_dp_size) ||
       (options_required && (child.options!=s->options || child.options_size!=s->options_size ||
                             memcmp(child.options,s->options,s->options_size)))) {
        hf_fail(s,HF_INVALID);return 0;
    }
    return 1;
}
/* The session must start zeroed and remain alive/unmoved through StartImage.
 * All callback functions are trusted adapter boundaries, never firmware bindings here. */
static inline enum hf_status hf_prepare(struct hf_session *s,const struct hf_request *r,const struct hf_callbacks *c) {
    if(!s) return HF_INVALID;
    if(s->phase!=HF_EMPTY || s->mutation_started || s->start_called) return hf_fail(s,HF_STATE);
    s->phase=HF_PREPARING;
    if(!r || !c || !r->pins || !c->check_current || !c->read_loader || !c->load ||
       !c->inspect_child || !c->set_ram_options || !c->start || !r->parent ||
       !r->loader_buffer || r->boot_variable_attributes!=7) return hf_fail(s,HF_INVALID);
    const struct hf_pins *pins=r->pins;
    size_t hd=0;
    if(!hf_nonzero(pins->loader_sha256,32) || !hf_nonzero(pins->boot_sha256,32) ||
       !hf_nonzero(pins->host_uuid,16) || !hf_nonzero(pins->boot_instance,16) ||
       memcmp(pins->host_uuid,r->host_uuid,16) || memcmp(pins->boot_instance,r->boot_instance,16) ||
       !pins->loader_size || pins->loader_size>HF_MAX_LOADER || pins->loader_size>r->loader_buffer_capacity ||
       pins->partition_binding_verified!=1 ||
       !hf_partition_path(pins->partition_path,pins->partition_path_size,&hd)) return hf_fail(s,HF_INVALID);
    struct hf_boot boot;enum hf_status rc=hf_parse_boot(r->boot_raw,r->boot_raw_size,&boot);
    if(rc!=HF_OK) return hf_fail(s,rc);
    uint8_t digest[32];inv_sha256(r->boot_raw,r->boot_raw_size,digest);
    if(memcmp(digest,pins->boot_sha256,32)) return hf_fail(s,HF_HASH);
    size_t selected=SIZE_MAX;rc=hf_match_partition(&boot,r->partitions,r->partition_count,&selected);
    if(rc!=HF_OK) return hf_fail(s,rc);
    const struct hf_partition *p=&r->partitions[selected];
    if(p->device_path_size!=pins->partition_path_size ||
       memcmp(p->device_path,pins->partition_path,p->device_path_size) ||
       p->block_size!=pins->block_size || r->parent==p->handle) return hf_fail(s,HF_INVALID);
    struct hf_binding *b=&s->binding;
    memcpy(b->host_uuid,pins->host_uuid,16);memcpy(b->boot_instance,pins->boot_instance,16);
    memcpy(b->boot_sha256,pins->boot_sha256,32);memcpy(b->loader_sha256,pins->loader_sha256,32);
    b->parent=r->parent;b->device=p->handle;b->block_size=p->block_size;b->media_id=p->media_id;b->last_block=p->last_block;
    b->partition_path_size=p->device_path_size;memcpy(b->partition_path,p->device_path,p->device_path_size);
    b->file_dp_size=boot.file_dp_size;memcpy(b->file_dp,boot.file_dp,boot.file_dp_size);
    /* Replace partition End Entire with the complete original file node + End. */
    b->full_image_path_size=p->device_path_size-4+boot.file_dp_size;
    if(b->full_image_path_size>sizeof(b->full_image_path)) return hf_fail(s,HF_INVALID);
    memcpy(b->full_image_path,p->device_path,p->device_path_size-4);
    memcpy(b->full_image_path+p->device_path_size-4,boot.file_dp,boot.file_dp_size);
    b->loader_size=pins->loader_size;s->options_size=boot.options_size;
    memcpy(s->options,boot.options,boot.options_size);
    inv_sha256(s->options,s->options_size,b->options_sha256);s->callbacks=*c;
    if(!hf_check(s,HF_CHECK_PRE_LOAD,HF_PREPARING)) return s->status;
    size_t actual=0;
    if(!hf_callback_ok(s,s->callbacks.read_loader(s->callbacks.context,b,r->loader_buffer,b->loader_size,&actual),HF_PREPARING)) return s->status;
    if(actual!=b->loader_size) return hf_fail(s,HF_IO);
    inv_sha256(r->loader_buffer,actual,digest);
    if(memcmp(digest,b->loader_sha256,32)) return hf_fail(s,HF_HASH);
    /* SourceBuffer and full path both come from validated copies. No BootPolicy search. */
    uint64_t loaded=s->callbacks.load(s->callbacks.context,0,b->parent,b->full_image_path,b->full_image_path_size,
                                     r->loader_buffer,actual,&s->child);
    if(!hf_callback_ok(s,loaded,HF_PREPARING)) return s->status;
    if(!s->child || s->child==b->parent || s->child==b->device) return hf_fail(s,HF_INVALID);
    if(!hf_child_valid(s,HF_PREPARING,0)) return s->status;
    if(!hf_callback_ok(s,s->callbacks.set_ram_options(s->callbacks.context,s->child,s->options,s->options_size),HF_PREPARING)) return s->status;
    if(!hf_child_valid(s,HF_PREPARING,1) || !hf_check(s,HF_CHECK_PREPARED,HF_PREPARING)) return s->status;
    s->phase=HF_PREPARED;s->status=HF_OK;return HF_OK;
}
/* Caller must invoke this BEFORE its first possible PCI write, and only after
 * separately authorizing that transaction. This engine never performs the write. */
static inline enum hf_status hf_begin_mutation(struct hf_session *s,unsigned execute) {
    if(!s) return HF_INVALID;
    if(s->phase!=HF_PREPARED || execute!=1) return hf_fail(s,HF_STATE);
    s->phase=HF_MUTATION_CHECKING;
    if(!hf_check(s,HF_CHECK_BEFORE_MUTATION,HF_MUTATION_CHECKING) || !hf_child_valid(s,HF_MUTATION_CHECKING,1)) return s->status;
    s->mutation_started=1;s->phase=HF_MUTATING;s->status=HF_OK;return HF_OK;
}
/* These are explicit, trusted caller results, not proof of actual hardware state. */
static inline enum hf_status hf_transaction_result(struct hf_session *s,unsigned success,unsigned final_checks) {
    if(!s) return HF_INVALID;
    if(s->phase!=HF_MUTATING || success!=1 || final_checks!=1) return hf_fail(s,HF_STATE);
    s->phase=HF_TRANSACTION_OK;s->status=HF_OK;return HF_OK;
}
/* A successful OS transfer is nonreturning. Every returned status, even EFI_SUCCESS,
 * is terminal recovery-required because the external PCI transaction has begun. */
static inline enum hf_status hf_start(struct hf_session *s,unsigned execute) {
    if(!s) return HF_INVALID;
    if(s->phase!=HF_TRANSACTION_OK || execute!=1 || !s->mutation_started || s->start_called) return hf_fail(s,HF_STATE);
    s->phase=HF_FINAL_CHECKING;
    if(!hf_check(s,HF_CHECK_BEFORE_START,HF_FINAL_CHECKING) || !hf_child_valid(s,HF_FINAL_CHECKING,1)) return s->status;
    s->start_called=1;s->phase=HF_STARTING;
    s->start_return_status=s->callbacks.start(s->callbacks.context,s->child);
    return hf_fail(s,HF_RECOVERY_REQUIRED);
}
#endif
