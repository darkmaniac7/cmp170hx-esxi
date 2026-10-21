/* Adapted qualified existing-handle readers; console/serial only, no file writes. */
#include <efi.h>
#include <efilib.h>
#include <efiser.h>
#include <string.h>
/* The reviewed pure parser uses C memory primitives; EFI has no libc. */
static void *ba_copy(void *d,const void *s,size_t n){UINT8 *a=d;const UINT8 *b=s;for(size_t i=0;i<n;i++)a[i]=b[i];return d;}
static void *ba_fill(void *d,int value,size_t n){UINT8 *a=d;for(size_t i=0;i<n;i++)a[i]=(UINT8)value;return d;}
static int ba_compare(const void *a,const void *b,size_t n){const UINT8 *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]<y[i]?-1:1;return 0;}
#define memcpy ba_copy
#define memset ba_fill
#define memcmp ba_compare
#include "boot_audit.h"
static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;
static EFI_SERIAL_IO_PROTOCOL *serial;
static CHAR8 text[1024];static UINTN used,sequence,errors;
static UINT64 inv_le(const UINT8 *p,unsigned n){UINT64 v=0;for(unsigned i=0;i<n;i++)v|=(UINT64)p[i]<<(8*i);return v;}
static UINTN matched_uuids;
static const char *current_kind;
static void add(const char *s){while(*s&&used+1<sizeof(text))text[used++]=*s++;text[used]=0;}
static void hex(UINT64 n){static const char d[]="0123456789abcdef";add("0x");for(int i=60;i>=0;i-=4){char b[2]={d[(n>>i)&15],0};add(b);}}
static void field(const char *k,UINT64 v){add(" ");add(k);add("=");hex(v);}
static void begin(const char *kind){current_kind=kind;used=0;add("BOOTINV");field("seq",sequence++);add(" kind=");add(kind);}
static void emit(void){
    CHAR16 wide[1024];add("\r\n");for(UINTN i=0;i<used;i++)wide[i]=text[i];wide[used]=0;
    if(st->ConOut&&st->ConOut->OutputString)uefi_call_wrapper(st->ConOut->OutputString,2,st->ConOut,wide);
    if(current_kind[0]=='S'&&current_kind[1]=='M'&&current_kind[2]=='B'&&current_kind[3]=='I'&&current_kind[4]=='O'&&current_kind[5]=='S'&&current_kind[6]=='_'&&current_kind[7]=='U'&&current_kind[8]=='U'&&current_kind[9]=='I'&&current_kind[10]=='D'&&current_kind[11]==0){
        const char suffix[]=" canonical=" HOST_UUID_TEXT "\r\n";
        if(used<sizeof suffix-1||CompareMem(text+used-(sizeof suffix-1),suffix,sizeof suffix-1))errors++;
        else matched_uuids++;
    }
    if(serial){UINTN n=used;EFI_STATUS s=uefi_call_wrapper(serial->Write,3,serial,&n,text);if(EFI_ERROR(s)||n!=used){serial=NULL;errors++;}}
}
static void failure(const char *scope,EFI_STATUS s){errors++;begin("ERROR");add(" scope=");add(scope);field("status",s);emit();}
#include "identity_readonly.h"
static struct ba_device devices[BA_MAX_HANDLES],again[BA_MAX_HANDLES];
static UINTN device_count,selected=~(UINTN)0;
static EFI_FILE_HANDLE target_root;
static UINT8 boot_raw[HF_MAX_BOOT];static UINTN boot_size;static UINT32 boot_attributes;
static EFI_GUID partition_guid={0x8cf2f62c,0xbc9b,0x4821,{0x80,0x8d,0xec,0x9e,0xc4,0x21,0xa1,0xa0}};
#if HANDOFF_SYNTHETIC
static const CHAR16 *file_paths[]={L"\\EFI\\BOOT\\CHILD64.EFI",L"\\EFI\\VMware\\mboot64.efi",L"\\EFI\\VMware\\safeboot64.efi"};
#else
static const CHAR16 *file_paths[]={L"\\EFI\\BOOT\\BOOTX64.EFI",L"\\EFI\\VMware\\mboot64.efi",L"\\EFI\\VMware\\safeboot64.efi"};
#endif
static void raw_records(const char *kind,UINTN owner,const UINT8 *p,UINTN n){
    for(UINTN off=0;off<n;off+=128){UINTN take=n-off;if(take>128)take=128;
        begin(kind);field("owner",owner);field("offset",off);add(" data=");bytes_hex(p+off,take);emit();}
}
static EFI_STATUS read_boot(UINT8 *raw,UINTN *size,UINT32 *attributes){
#if HANDOFF_SYNTHETIC
    if(*size<sizeof synthetic_boot_fixture){*size=sizeof synthetic_boot_fixture;return EFI_BUFFER_TOO_SMALL;}
    CopyMem(raw,synthetic_boot_fixture,sizeof synthetic_boot_fixture);*size=sizeof synthetic_boot_fixture;*attributes=7;return EFI_SUCCESS;
#else
    EFI_GUID guid=EFI_GLOBAL_VARIABLE;
    return uefi_call_wrapper(st->RuntimeServices->GetVariable,5,L"Boot0003",&guid,attributes,size,raw);
#endif
}
static EFI_STATUS copy_path(EFI_HANDLE h,struct ba_device *d){
    EFI_GUID guid=EFI_DEVICE_PATH_PROTOCOL_GUID;EFI_DEVICE_PATH *path=NULL;
    EFI_STATUS s=uefi_call_wrapper(bs->HandleProtocol,3,h,&guid,(VOID **)&path);
    if(EFI_ERROR(s)||!path)return EFI_ERROR(s)?s:EFI_COMPROMISED_DATA;
    UINT8 *p=(VOID *)path;UINTN off=0;
    if((UINTN)p>~(UINTN)0-sizeof d->path)return EFI_COMPROMISED_DATA;
    for(UINTN nodes=0;nodes<128&&off+4<=sizeof d->path;nodes++){
        if(!ram_readable((UINTN)(p+off),4))return EFI_COMPROMISED_DATA;
        UINTN n=inv_le(p+off+2,2);
        if(n<4||n>sizeof d->path-off||!ram_readable((UINTN)(p+off),n))return EFI_COMPROMISED_DATA;
        CopyMem(d->path+off,p+off,n);off+=n;
        if(p[off-n]==0x7f){d->path_size=off;return EFI_SUCCESS;}
    }
    return EFI_COMPROMISED_DATA;
}
static void observe(EFI_HANDLE h,struct ba_device *d){
    SetMem(d,sizeof *d,0);d->handle=(uintptr_t)h;
    EFI_GUID bg=EFI_BLOCK_IO_PROTOCOL_GUID,sg=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_BLOCK_IO *block=NULL;EFI_FILE_IO_INTERFACE *fs=NULL;VOID *pi=NULL;
    d->block_status=uefi_call_wrapper(bs->HandleProtocol,3,h,&bg,(VOID **)&block);
    if(!d->block_status){
        if(!block||!ram_readable((UINTN)block,sizeof *block)||!block->Media||!ram_readable((UINTN)block->Media,32))d->block_status=EFI_COMPROMISED_DATA;
        else {EFI_BLOCK_IO_MEDIA *m=block->Media;d->media_id=m->MediaId;d->media_present=m->MediaPresent;d->logical_partition=m->LogicalPartition;
            d->removable=m->RemovableMedia;d->read_only=m->ReadOnly;d->block_size=m->BlockSize;d->io_align=m->IoAlign;d->last_block=m->LastBlock;}
    }
    d->path_status=copy_path(h,d);
    d->sfs_status=uefi_call_wrapper(bs->HandleProtocol,3,h,&sg,(VOID **)&fs);
    if(!d->sfs_status&&(!fs||!ram_readable((UINTN)fs,sizeof *fs)||fs->Revision!=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION||!fs->OpenVolume))d->sfs_status=EFI_COMPROMISED_DATA;
    d->partition_status=uefi_call_wrapper(bs->HandleProtocol,3,h,&partition_guid,&pi);
    if(!d->partition_status){if(!pi||!ram_readable((UINTN)pi,sizeof d->partition_info))d->partition_status=EFI_COMPROMISED_DATA;
        else CopyMem(d->partition_info,pi,sizeof d->partition_info);}
}
static int sfs_coverage(const struct ba_device *out,UINTN count,BOOLEAN records){
    EFI_GUID guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;EFI_HANDLE *handles=NULL;UINTN n=0;
    EFI_STATUS s=uefi_call_wrapper(bs->LocateHandleBuffer,5,ByProtocol,&guid,NULL,&n,&handles);
    if(records){begin("SFS_ENUM");field("status",s);field("count",n);emit();}
    if(EFI_ERROR(s)||!handles||!n||n>BA_MAX_HANDLES){if(handles)uefi_call_wrapper(bs->FreePool,1,handles);return 0;}
    unsigned okay=1;
    for(UINTN i=0;i<n;i++){
        UINTN found=~(UINTN)0,matches=0;
        for(UINTN j=0;j<count;j++)if((UINTN)handles[i]==out[j].handle&&!out[j].sfs_status){found=j;matches++;}
        for(UINTN j=0;j<i;j++)if(handles[j]==handles[i])okay=0;
        if(matches!=1)okay=0;
        if(records){begin("SFS_HANDLE");field("index",i);field("handle",(UINTN)handles[i]);field("block_index",found);field("matches",matches);emit();
            if(!matches){struct ba_device extra;observe(handles[i],&extra);begin("SFS_UNBOUND");field("index",i);field("block_status",extra.block_status);field("path_status",extra.path_status);field("path_bytes",extra.path_size);emit();
                if(!extra.path_status)raw_records("SFS_UNBOUND_PATH",i,extra.path,extra.path_size);}}
    }
    for(UINTN i=0;i<count;i++)if(!out[i].sfs_status){UINTN matches=0;for(UINTN j=0;j<n;j++)if((UINTN)handles[j]==out[i].handle)matches++;if(matches!=1)okay=0;}
    s=uefi_call_wrapper(bs->FreePool,1,handles);return okay&&!EFI_ERROR(s);
}
static int collect(struct ba_device *out,UINTN *count,BOOLEAN records){
    EFI_GUID guid=EFI_BLOCK_IO_PROTOCOL_GUID;EFI_HANDLE *handles=NULL;UINTN n=0;
    EFI_STATUS s=uefi_call_wrapper(bs->LocateHandleBuffer,5,ByProtocol,&guid,NULL,&n,&handles);
    if(records){begin("BLOCK_ENUM");field("status",s);field("count",n);emit();}
    *count=0;
    if(EFI_ERROR(s)||!handles||!n||n>BA_MAX_HANDLES){if(handles)uefi_call_wrapper(bs->FreePool,1,handles);return 0;}
    for(UINTN i=0;i<n;i++){
        observe(handles[i],out+i);struct ba_device *d=out+i;
        if(records){begin("BLOCK");field("index",i);field("handle",d->handle);field("block_status",d->block_status);field("path_status",d->path_status);field("sfs_status",d->sfs_status);field("partition_status",d->partition_status);
            field("media_id",d->media_id);field("present",d->media_present);field("logical",d->logical_partition);field("removable",d->removable);field("read_only",d->read_only);field("block_size",d->block_size);field("io_align",d->io_align);field("last_block",d->last_block);field("path_bytes",d->path_size);emit();
            if(!d->path_status)raw_records("PARTITION_PATH",i,d->path,d->path_size);
            if(!d->partition_status)raw_records("PARTITION_INFO",i,d->partition_info,sizeof d->partition_info);
        }
    }
    *count=n;s=uefi_call_wrapper(bs->FreePool,1,handles);
    return !EFI_ERROR(s)&&sfs_coverage(out,n,records);
}
static uint64_t unchanged(void *unused){
    (void)unused;
    if(selected>=device_count)return EFI_NOT_FOUND;
    struct ba_device d;observe((EFI_HANDLE)devices[selected].handle,&d);
    if(CompareMem(&d,devices+selected,sizeof d))return EFI_MEDIA_CHANGED;
    UINT8 raw[HF_MAX_BOOT];UINTN n=sizeof raw;UINT32 attrs=0;EFI_STATUS s=read_boot(raw,&n,&attrs);
    if(EFI_ERROR(s))return s;
    return n==boot_size&&attrs==boot_attributes&&!CompareMem(raw,boot_raw,n)?EFI_SUCCESS:EFI_MEDIA_CHANGED;
}
static uint64_t open_fixed(void *unused,unsigned index,uintptr_t *out){
    (void)unused;if(index>=3||!target_root||!target_root->Open)return EFI_INVALID_PARAMETER;
    EFI_FILE_HANDLE f=NULL;EFI_STATUS s=uefi_call_wrapper(target_root->Open,5,target_root,&f,(CHAR16 *)file_paths[index],EFI_FILE_MODE_READ,0);
    *out=(uintptr_t)f;
    if(!EFI_ERROR(s)&&(!f||!ram_readable((UINTN)f,sizeof *f)||!f->Read||!f->Close||!f->GetInfo))return EFI_COMPROMISED_DATA;
    return s;
}
static uint64_t get_info(void *unused,uintptr_t token,uint8_t *buffer,size_t *n){
    (void)unused;EFI_FILE_HANDLE f=(VOID *)token;EFI_GUID g=EFI_FILE_INFO_ID;UINTN size=*n;
    EFI_STATUS s=uefi_call_wrapper(f->GetInfo,4,f,&g,&size,buffer);*n=size;return s;
}
static uint64_t read_file(void *unused,uintptr_t token,uint8_t *buffer,size_t *n){
    (void)unused;EFI_FILE_HANDLE f=(VOID *)token;UINTN size=*n;
    EFI_STATUS s=uefi_call_wrapper(f->Read,3,f,&size,buffer);*n=size;return s;
}
static uint64_t close_file(void *unused,uintptr_t token){
    (void)unused;EFI_FILE_HANDLE f=(VOID *)token;
    return f&&ram_readable((UINTN)f,sizeof *f)&&f->Close?uefi_call_wrapper(f->Close,1,f):EFI_COMPROMISED_DATA;
}
