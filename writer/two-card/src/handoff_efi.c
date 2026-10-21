/* Separate two-card writer candidate; exact independent build binding required. */
#include "build_pins.h"
#if !defined(HANDOFF_SYNTHETIC) || HANDOFF_SYNTHETIC != 0 || !defined(PHYSICAL_BINDING_VERIFIED) || PHYSICAL_BINDING_VERIFIED != 1
#error A separately verified physical capture binding is required
#endif
#if !defined(WRITER_POLICY_BOUND) || WRITER_POLICY_BOUND != 1 || !defined(WRITER_EXECUTE) || WRITER_EXECUTE != 1
#error An independently reviewed writer binding is required
#endif
#include "../pins/boot_fixture.h"
#define inv_le writer_identity_le
#include "existing_readers.inc"
#undef inv_le
#include "image_services.h"
#include "boot_current.h"
#include "../coordinator/src/coordinator.h"
#include "acpi_current.h"
#include "efi_tables.h"
static struct hf_efi image_context;
static struct hf_session session;
static struct hf_partition partitions[BA_MAX_HANDLES];
static unsigned partition_count;
static struct bc_state current_boot;
static UINT8 invocation_token[16];
static struct pc_pair_session *writer;
static struct ws_context current_acpi;
static unsigned acpi_captures;
static uint64_t boot_current_read(void *unused,uint8_t *raw,size_t *size,uint32_t *attrs){
    (void)unused;EFI_GUID guid=EFI_GLOBAL_VARIABLE;UINTN n=*size;UINT32 a=0;
    EFI_STATUS result=uefi_call_wrapper(st->RuntimeServices->GetVariable,5,L"BootCurrent",&guid,&a,&n,raw);
    *size=n;*attrs=a;return result;
}
static int current_boot_check(void){
    int good=bc_check(&current_boot,NULL,boot_current_read);
    begin("BOOT_CURRENT");field("status",current_boot.last_status);field("bytes",current_boot.last_size);
    field("value",current_boot.value);field("attributes",current_boot.attributes);field("unchanged",good);field("set_variable",0);emit();
    return good;
}
static int readable(void *unused,uintptr_t pointer,size_t n){(void)unused;return ram_readable(pointer,n);}
static EFI_STATUS refresh_map(void){
    if(firmware_map){EFI_STATUS s=uefi_call_wrapper(bs->FreePool,1,firmware_map);if(s)return s;}
    firmware_map=NULL;firmware_map_bytes=0;memory_map();
    return errors||!firmware_map?EFI_COMPROMISED_DATA:EFI_SUCCESS;
}
static uint64_t current_check(void *ctx,enum hf_check_stage stage,const struct hf_binding *binding){
    (void)ctx;
    if(stage==HF_CHECK_BEFORE_MUTATION&&(!writer||writer->status!=PC_RUNNING||
       writer->env.handoff!=&session||session.phase!=HF_MUTATION_CHECKING))return EFI_ACCESS_DENIED;
    if(!binding||CompareMem(binding->host_uuid,pinned_host_uuid,16)||
       CompareMem(binding->boot_instance,invocation_token,16)||
       binding->device!=devices[selected].handle)return EFI_COMPROMISED_DATA;
    if(!current_boot_check())return EFI_MEDIA_CHANGED;
    EFI_STATUS refreshed=refresh_map();if(refreshed)return refreshed;
    UINTN old=matched_uuids;smbios();if(errors||matched_uuids==old)return EFI_COMPROMISED_DATA;
    UINTN n=0;if(!collect(again,&n,FALSE)||n!=device_count)return EFI_MEDIA_CHANGED;
    for(UINTN i=0;i<n;i++){
        UINTN matches=0;for(UINTN j=0;j<n;j++)
            if(again[j].handle==devices[i].handle&&!CompareMem(again+j,devices+i,sizeof devices[i]))matches++;
        if(matches!=1)return EFI_MEDIA_CHANGED;
    }
    uint64_t status=unchanged(NULL);
    begin("CURRENT_CHECK");field("stage",stage);field("status",status);field("pci_mutation",session.mutation_started);emit();
    return status;
}
static uint64_t loader_read(void *ctx,const struct hf_binding *binding,uint8_t *buffer,size_t capacity,size_t *actual){
    (void)ctx;*actual=0;
    if(binding->device!=devices[selected].handle||target_root)return EFI_COMPROMISED_DATA;
    EFI_GUID guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;EFI_FILE_IO_INTERFACE *fs=NULL;
    EFI_STATUS s=uefi_call_wrapper(bs->HandleProtocol,3,(EFI_HANDLE)binding->device,&guid,(VOID **)&fs);
    if(s||!fs||!ram_readable((UINTN)fs,sizeof *fs)||!fs->OpenVolume)return s?s:EFI_COMPROMISED_DATA;
    s=uefi_call_wrapper(fs->OpenVolume,2,fs,&target_root);
    if(s||!target_root||!ram_readable((UINTN)target_root,sizeof *target_root)||!target_root->Open||!target_root->Close)
        return s?s:EFI_COMPROMISED_DATA;
    struct ba_callbacks cb={NULL,unchanged,open_fixed,get_info,read_file,close_file};
    struct ba_file_result r;int valid=ba_hash_file(&cb,0,buffer,capacity,&r);
    s=close_file(NULL,(uintptr_t)target_root);target_root=NULL;
    begin("LOADER_READ");field("complete",valid);field("size",r.size);field("root_close",s);add(" sha256=");bytes_hex(r.digest,32);emit();
    if(!valid||s)return s?s:EFI_COMPROMISED_DATA;
    *actual=r.bytes_read;return EFI_SUCCESS;
}
static uint64_t image_load(void *ctx,unsigned policy,uintptr_t parent,const uint8_t *path,size_t path_n,const uint8_t *source,size_t source_n,uintptr_t *child){
    (void)ctx;uint64_t s=hf_efi_load(&image_context,policy,parent,path,path_n,source,source_n,child);
    if(!s)s=refresh_map();
    begin("LOAD_IMAGE");field("status",s);field("child",*child);field("source_bytes",source_n);field("pci_mutation",0);emit();return s;
}
static uint64_t child_inspect(void *ctx,uintptr_t child,struct hf_child *out){(void)ctx;return hf_efi_inspect(&image_context,child,out);}
static uint64_t child_options(void *ctx,uintptr_t child,const uint8_t *bytes,size_t size){(void)ctx;return hf_efi_options(&image_context,child,bytes,size);}
static uint64_t child_start(void *ctx,uintptr_t child){
    (void)ctx;begin("START_IMAGE");field("attempt",1);field("pci_mutation",session.mutation_started);emit();
    return hf_efi_start(&image_context,child);
}
static int writer_acpi(void *context,struct rc_acpi *out){
    (void)context;acpi_captures++;
    int okay=ws_capture(&current_acpi,st,rc_aml_sha,out);
    begin("WRITER_ACPI");field("attempt",acpi_captures);field("complete",okay);
    field("status",current_acpi.result.status);field("tables",current_acpi.result.table_count);
    field("bytes",current_acpi.result.total_bytes);field("error_address",current_acpi.result.error_address);
    field("error_length",current_acpi.result.error_length);field("map_key",current_acpi.map_key);
    add(" map_sha256=");bytes_hex(current_acpi.ram.memory_map_sha256,32);emit();
    return okay&&errors==0;
}
static void writer_report(const char *stage){
    for(unsigned t=0;t<2;t++){
    begin("WRITER_STATE");field("target_index",t);field("attempted_stores",writer->pair.attempted);field("applied_mask",writer->applied_mask);add(" stage=");add(stage);field("status",writer->status);
    field("hf_phase",session.phase);field("hf_status",session.status);
    field("hf_callback_status",session.last_callback_status);field("write_attempted",writer->access[t].write_attempted);
    field("next_write",writer->access[t].next_write);field("bt_status",writer->transaction[t].status);
    field("final_checks",writer->final_checks);field("hardware_decode_observed",writer->hardware_decode_observed);
    field("atomic_hardware_isolation_observed",writer->atomic_hardware_isolation_observed);emit();
    const struct bt_state *bt=&writer->transaction[t];const struct bt_diagnostic *bd=&bt->failure;
    begin("WRITER_BT_FAILURE");field("target_index",t);field("reason",bd->reason);field("stage",bd->stage);
    field("bdf",bd->bdf);field("offset",bd->offset);field("width",bd->width);
    field("expected_valid",bd->expected_valid);field("expected",bd->expected);field("mask",bd->mask);
    field("observed_valid",bd->observed_valid);field("observed",bd->observed);
    field("callback_result",(uint32_t)bd->callback_result);emit();
    begin("WRITER_REBAR_TRANSITION");field("target_index",t);field("cap_before",bt->cap_before);field("ctrl_before",bt->ctrl_before);
    field("cap_after_valid",bt->cap_after_valid);field("cap_after",bt->cap_after);
    field("ctrl_after_valid",bt->ctrl_after_valid);field("ctrl_after",bt->ctrl_after);
    field("wanted",bt->rebar_wanted);emit();
    const struct pa_diagnostic *pd=&writer->access[t].failure;
    begin("WRITER_PCI_IO_FAILURE");field("target_index",t);field("reason",pd->reason);field("bdf",pd->bdf);
    field("offset",pd->offset);field("width",pd->width);field("expected_valid",pd->expected_valid);
    field("expected",pd->expected);field("observed_valid",pd->observed_valid);field("observed",pd->observed);
    field("efi_status_valid",pd->status_valid);field("efi_status",pd->status);emit();
    }
    for(unsigned i=0;i<5;i++){
        const struct pi_snapshot *p=&writer->pci[i];begin("WRITER_PCI");field("snapshot",i);field("status",p->status);
        field("count",p->count);field("cmp_count",p->cmp_count);field("last_index",p->last_index);
        field("last_offset",p->last_offset);field("last_status",p->last_status);emit();
    }
    for(unsigned i=0;i<2;i++){
        const struct rc_result *c=&writer->coverage[i].coverage;const struct rs_snapshot *r=&writer->resources[i];
        begin("WRITER_COVERAGE");field("snapshot",i);field("status",c->status);field("reason",c->reason);
        field("owner",c->owner);field("index",c->index);field("resource_status",r->status);
        field("last_owner",r->last_owner);field("last_query",r->last_query);field("last_status",r->last_status);emit();
    }
    const struct gr_pair_state *g=&writer->reservation;
    for(unsigned t=0;t<2;t++){
    const struct gr_pair_slot *slot=&g->slot[t];
    begin("WRITER_GCD");field("target_index",t);field("status",g->status);
    field("owned_mask",g->owned_mask);field("first_failure",g->first_failure);
    field("first_status",g->first_status);field("first_slot",g->first_slot);
    field("allocate_attempted",slot->allocate_attempted);field("allocate_verified",slot->allocate_verified);
    field("free_attempted",slot->free_attempted);field("free_verified",slot->free_verified);
    field("pci_attempted",g->pci_attempted);field("returned_base",slot->returned_base);
    field("allocate_status",slot->allocate_status);field("free_status",slot->free_status);
    field("map_status",g->map_status);field("pool_status",g->pool_status);emit();
    }

}
static EFI_STATUS handoff_run(EFI_HANDLE image,EFI_SYSTEM_TABLE *system){
    InitializeLib(image,system);st=system;bs=st->BootServices;
    if(!bs||!bs->GetMemoryMap||!bs->AllocatePool||!bs->FreePool||!bs->LocateHandleBuffer||!bs->HandleProtocol||
       !bs->LoadImage||!bs->StartImage||!st->RuntimeServices||!st->RuntimeServices->GetVariable)return EFI_UNSUPPORTED;
    EFI_GUID sg=EFI_SERIAL_IO_PROTOCOL_GUID;
    EFI_STATUS s=bs->LocateProtocol?uefi_call_wrapper(bs->LocateProtocol,3,&sg,NULL,(VOID **)&serial):EFI_UNSUPPORTED;
    if(s||!serial||!serial->Write||!serial->Mode||!serial->Mode->Timeout||serial->Mode->Timeout>1000000)serial=NULL;
    begin("BEGIN");field("schema",3);field("synthetic",0);field("pci_mutation",0);field("resize_authorized",1);emit();
    memory_map();smbios();
    if(errors||!matched_uuids)return EFI_ACCESS_DENIED;
    if(!current_boot_check())return EFI_ACCESS_DENIED;
    /* Per-invocation token, NOT an authenticated physical boot UUID. The
     * operator's fresh boot/window identity is a separate prerequisite. */
    UINT64 invocation_material[3]={(UINT64)(UINTN)image,(UINT64)(UINTN)system,current_boot.value};
    UINT8 token_digest[32];inv_sha256((UINT8 *)invocation_material,sizeof invocation_material,token_digest);
    CopyMem(invocation_token,token_digest,sizeof invocation_token);
    begin("INVOCATION");field("boot_uuid_proven",0);field("watchdog_unchanged",1);field("boot_current_unchanged",1);emit();
    boot_size=sizeof boot_raw;s=read_boot(boot_raw,&boot_size,&boot_attributes);
    if(s||!collect(devices,&device_count,TRUE)||errors)return EFI_COMPROMISED_DATA;
    if(!ba_select(boot_raw,boot_size,boot_attributes,devices,device_count,&selected))return EFI_COMPROMISED_DATA;
    for(UINTN i=0;i<device_count;i++){
        const struct ba_device *d=devices+i;size_t hd=0;
        if(!d->logical_partition||d->sfs_status)continue;
        if(!hf_partition_path(d->path,d->path_size,&hd))return EFI_COMPROMISED_DATA;
        partitions[partition_count++]=(struct hf_partition){.handle=d->handle,.device_path=d->path,.device_path_size=d->path_size,
            .existing=1,.simple_fs=1,.block_io=1,.media_present=d->media_present,.logical_partition=d->logical_partition,
            .block_size=d->block_size,.media_id=d->media_id,.last_block=d->last_block};
    }
    struct hf_pins pins={.partition_path=pinned_partition_path,.partition_path_size=sizeof pinned_partition_path,
        .loader_size=PINNED_LOADER_BYTES,.block_size=PINNED_BLOCK_SIZE,.partition_binding_verified=1};
    CopyMem(pins.host_uuid,pinned_host_uuid,16);CopyMem(pins.boot_instance,invocation_token,16);
    CopyMem(pins.boot_sha256,pinned_boot_sha256,32);CopyMem(pins.loader_sha256,pinned_loader_sha256,32);
    UINT8 *buffer=NULL;s=uefi_call_wrapper(bs->AllocatePool,3,EfiLoaderData,PINNED_LOADER_BYTES,(VOID **)&buffer);
    if(s||!buffer)return EFI_OUT_OF_RESOURCES;
    struct hf_request request={.boot_raw=boot_raw,.boot_raw_size=boot_size,.boot_variable_attributes=boot_attributes,
        .pins=&pins,.partitions=partitions,.partition_count=partition_count,.parent=(uintptr_t)image,
        .loader_buffer=buffer,.loader_buffer_capacity=PINNED_LOADER_BYTES};
    CopyMem(request.host_uuid,pinned_host_uuid,16);CopyMem(request.boot_instance,invocation_token,16);
    image_context=(struct hf_efi){.bs=bs,.system=st,.parent=image,.readable=readable};
    struct hf_callbacks cb={NULL,current_check,loader_read,image_load,child_inspect,child_options,child_start};
    enum hf_status result=hf_prepare(&session,&request,&cb);
    begin("PREPARE");field("status",result);field("callback_status",session.last_callback_status);field("phase",session.phase);field("pci_mutation",session.mutation_started);emit();
    if(result!=HF_OK)return EFI_COMPROMISED_DATA;
    s=uefi_call_wrapper(bs->AllocatePool,3,EfiLoaderData,sizeof *writer,(VOID **)&writer);
    if(s||!writer)return EFI_OUT_OF_RESOURCES;
    SetMem(writer,sizeof *writer,0);
    if(refresh_map())return EFI_COMPROMISED_DATA;
    const EFI_GUID dxe_guid={0x05ad34ba,0x6f02,0x4214,{0x95,0x2e,0x4d,0xa0,0x39,0x8e,0x2b,0xb9}};
    GR_DXE *dxe=NULL;
    if(!we_table(st,&dxe_guid,readable,NULL,sizeof *dxe,(VOID **)&dxe))return EFI_COMPROMISED_DATA;
    struct pc_environment environment={.bs=bs,.dxe=dxe,.image=image,.handoff=&session,
        .readable=readable,.read_acpi=writer_acpi,.policy=PC_POLICY_TRUSTED_FIRMWARE_R1};
    begin("WRITER_RUN");field("execute",1);field("session_bytes",sizeof *writer);
    field("target_count",2);field("target0",pair_targets[0].endpoint);field("bridge0",pair_targets[0].port);
    field("target1",pair_targets[1].endpoint);field("bridge1",pair_targets[1].port);field("policy",environment.policy);
    field("hardware_decode_observed",0);field("atomic_hardware_isolation_observed",0);emit();
    enum pc_status prepared=pc_pair_run(writer,&environment);writer_report("run_returned");
    if(prepared!=PC_READY_FOR_HANDOFF)return EFI_COMPROMISED_DATA;
    enum pc_status returned=pc_pair_start(writer);writer_report("start_returned");
    begin("WRITER_RETURN");field("result",returned);field("child_status",session.start_return_status);
    field("start_called",session.start_called);field("pci_mutation",session.mutation_started);field("retry",0);emit();
    /* Terminal wrapper state: no automatic retry/fallback/reset. An inherited
     * watchdog or child/firmware action can still reset the platform. */
    for(;;)__asm__ __volatile__("pause");
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *system){
    EFI_STATUS status=handoff_run(image,system);
    begin("HANDOFF_REFUSED");field("status",status);field("start_called",session.start_called);
    field("pci_mutation",session.mutation_started);field("cold_recovery_required",session.phase==HF_COLD_RECOVERY||(writer&&writer->status==PC_COLD_RECOVERY));emit();
    for(;;)__asm__ __volatile__("pause");
}
