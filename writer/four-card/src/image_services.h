#ifndef CMP_IMAGE_SERVICES_H
#define CMP_IMAGE_SERVICES_H
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <stdint.h>
#include "../pins/handoff.h"

struct hf_efi {
    EFI_BOOT_SERVICES *bs;
    EFI_SYSTEM_TABLE *system;
    EFI_HANDLE parent, child;
    int (*readable)(void *, uintptr_t, size_t);
    void *read_context;
    unsigned load_attempted, loaded, options_assigned, start_attempted;
    const uint8_t *options;
    size_t options_size;
};
static int hf_efi_range(struct hf_efi *c,const void *p,size_t n) {
    return c&&c->readable&&p&&n&&(uintptr_t)p<=UINTPTR_MAX-n&&
           c->readable(c->read_context,(uintptr_t)p,n);
}
/* Read at most one bounded path, requiring End Entire rather than an instance. */
static size_t hf_efi_path_size(struct hf_efi *c,const uint8_t *path,size_t maximum) {
    size_t offset=0;
    for(unsigned count=0;count<128&&offset+4<=maximum;count++) {
        if(!path||(uintptr_t)path>UINTPTR_MAX-offset)return 0;
        const uint8_t *node=(const uint8_t *)((uintptr_t)path+offset);
        if(!hf_efi_range(c,node,4))return 0;
        size_t n=(size_t)node[2]|((size_t)node[3]<<8);
        if(n<4||n>maximum-offset||!hf_efi_range(c,node,n))return 0;
        offset+=n;
        if(node[0]==0x7f)return node[1]==0xff&&n==4?offset:0;
    }
    return 0;
}
static uint64_t hf_efi_load(void *ctx,unsigned policy,uintptr_t parent,
    const uint8_t *path,size_t path_size,const uint8_t *source,size_t size,uintptr_t *child) {
    struct hf_efi *c=ctx;
    if(!c||!child||c->load_attempted||!c->bs||!c->bs->LoadImage||!c->parent||
       parent!=(uintptr_t)c->parent||policy||!path_size||path_size>HF_MAX_FULL_DP||
       hf_efi_path_size(c,path,path_size)!=path_size||!size||size>HF_MAX_LOADER||
       !hf_efi_range(c,source,size))return EFI_INVALID_PARAMETER;
    c->load_attempted=1;*child=0;
    EFI_HANDLE result=NULL;
    EFI_STATUS s=uefi_call_wrapper(c->bs->LoadImage,6,FALSE,c->parent,
                                  (EFI_DEVICE_PATH *)path,(VOID *)source,size,&result);
    c->child=result;*child=(uintptr_t)result;
    /* SECURITY_VIOLATION may return a child. Retain it, but never start it. */
    if(s)return s;
    if(!result||result==c->parent)return EFI_COMPROMISED_DATA;
    c->loaded=1;return EFI_SUCCESS;
}
static uint64_t hf_efi_loaded(struct hf_efi *c,uintptr_t child,EFI_LOADED_IMAGE **out) {
    if(!c||!out||!child||!c->loaded||c->start_attempted||child!=(uintptr_t)c->child||
       !c->bs||!c->bs->HandleProtocol)return EFI_INVALID_PARAMETER;
    EFI_GUID guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;VOID *raw=NULL;
    EFI_STATUS s=uefi_call_wrapper(c->bs->HandleProtocol,3,c->child,&guid,&raw);
    if(s)return s;
    if(!hf_efi_range(c,raw,sizeof(EFI_LOADED_IMAGE)))return EFI_COMPROMISED_DATA;
    EFI_LOADED_IMAGE *li=raw;
    if(li->Revision!=EFI_LOADED_IMAGE_PROTOCOL_REVISION||li->SystemTable!=c->system||
       li->ParentHandle!=c->parent||!li->DeviceHandle||li->DeviceHandle==c->child||
       !li->ImageBase||!li->ImageSize||li->ImageSize>HF_MAX_LOADER||
       li->ImageCodeType!=EfiLoaderCode||li->ImageDataType!=EfiLoaderData||
       li->LoadOptionsSize>HF_MAX_OPTIONS||
       (li->LoadOptionsSize&&!hf_efi_range(c,li->LoadOptions,li->LoadOptionsSize)))return EFI_COMPROMISED_DATA;
    *out=li;return EFI_SUCCESS;
}
static uint64_t hf_efi_inspect(void *ctx,uintptr_t child,struct hf_child *out) {
    struct hf_efi *c=ctx;EFI_LOADED_IMAGE *li=NULL;
    if(!out)return EFI_INVALID_PARAMETER;
    uint64_t s=hf_efi_loaded(c,child,&li);if(s)return s;
    size_t n=hf_efi_path_size(c,(const uint8_t *)li->FilePath,HF_MAX_FILE_DP);
    if(!n)return EFI_COMPROMISED_DATA;
    *out=(struct hf_child){.parent=(uintptr_t)li->ParentHandle,.device=(uintptr_t)li->DeviceHandle,
       .file_dp=(const uint8_t *)li->FilePath,.file_dp_size=n,
       .options=li->LoadOptions,.options_size=li->LoadOptionsSize};
    return EFI_SUCCESS;
}
static uint64_t hf_efi_options(void *ctx,uintptr_t child,const uint8_t *options,size_t size) {
    struct hf_efi *c=ctx;EFI_LOADED_IMAGE *li=NULL;
    if(!c||c->options_assigned||size>HF_MAX_OPTIONS||
       (size&&!hf_efi_range(c,options,size)))return EFI_INVALID_PARAMETER;
    struct hf_child info;uint64_t s=hf_efi_inspect(c,child,&info);if(s)return s;
    s=hf_efi_loaded(c,child,&li);if(s)return s;
    c->options_assigned=1;c->options=options;c->options_size=size;
    /* This is the caller-owned child's RAM protocol, never a firmware variable. */
    li->LoadOptions=(VOID *)options;li->LoadOptionsSize=(UINT32)size;
    return EFI_SUCCESS;
}
static uint64_t hf_efi_start(void *ctx,uintptr_t child) {
    struct hf_efi *c=ctx;
    if(!c||!c->options_assigned||!c->bs||!c->bs->StartImage)return EFI_INVALID_PARAMETER;
    struct hf_child info;uint64_t s=hf_efi_inspect(c,child,&info);if(s)return s;
    if(info.options!=c->options||info.options_size!=c->options_size)return EFI_COMPROMISED_DATA;
    c->start_attempted=1;
    UINTN ignored_size=0;
    /* ExitData is optional. Retain status only; no returned buffer to dereference. */
    return uefi_call_wrapper(c->bs->StartImage,3,c->child,&ignored_size,NULL);
}
#endif
