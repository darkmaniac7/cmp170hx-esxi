#ifndef EFIINV_FIRMWARE_READONLY_H
#define EFIINV_FIRMWARE_READONLY_H
/* Included after inventory emit helpers. All calls below read firmware state. */
static EFI_MEMORY_DESCRIPTOR *firmware_map;
static UINTN firmware_map_bytes, firmware_descriptor_bytes, firmware_map_key;
static UINT32 firmware_descriptor_version;

static void bytes_hex(const UINT8 *data,UINTN bytes)
{
    static const char digits[]="0123456789abcdef";
    for(UINTN i=0;i<bytes;i++) { char s[3]={digits[data[i]>>4],digits[data[i]&15],0};add(s); }
}
static void memory_map(void)
{
    UINTN size=0,key=0,ds=0;UINT32 version=0;
    EFI_STATUS status=uefi_call_wrapper(bs->GetMemoryMap,5,&size,NULL,&key,&ds,&version);
    if(status!=EFI_BUFFER_TOO_SMALL||ds<sizeof(EFI_MEMORY_DESCRIPTOR)||ds>256||size>4*1024*1024) {
        failure("memory_map_size",status);return;
    }
    for(UINTN attempt=0;attempt<3;attempt++) {
        UINTN capacity=size+32*ds;
        status=uefi_call_wrapper(bs->AllocatePool,3,EfiLoaderData,capacity,(VOID **)&firmware_map);
        if(EFI_ERROR(status)||!firmware_map) { failure("memory_map_allocate",status);return; }
        SetMem(firmware_map,capacity,0);
        size=capacity;status=uefi_call_wrapper(bs->GetMemoryMap,5,&size,firmware_map,&key,&ds,&version);
        if(status==EFI_BUFFER_TOO_SMALL) {
            uefi_call_wrapper(bs->FreePool,1,firmware_map);firmware_map=NULL;
            if(size>4*1024*1024||ds>256)break;
            continue;
        }
        if(EFI_ERROR(status)||ds<sizeof(EFI_MEMORY_DESCRIPTOR)||ds>256||!ds||size%ds||size>capacity)break;
        firmware_map_bytes=size;firmware_descriptor_bytes=ds;firmware_map_key=key;firmware_descriptor_version=version;
        begin("MEMORY_MAP");field("count",size/ds);field("descriptor_bytes",ds);field("descriptor_version",version);emit();
        for(UINTN off=0;off<size;off+=ds) {
            EFI_MEMORY_DESCRIPTOR *d=(VOID *)((UINT8 *)firmware_map+off);
            begin("MEMORY");field("index",off/ds);field("type",d->Type);field("physical",d->PhysicalStart);field("virtual",d->VirtualStart);field("pages",d->NumberOfPages);field("attributes",d->Attribute);emit();
        }
        return;
    }
    failure("memory_map_read",status);
    if(firmware_map)uefi_call_wrapper(bs->FreePool,1,firmware_map);
    firmware_map=NULL;
}
static BOOLEAN ram_readable(UINT64 address,UINTN length)
{
    if(!firmware_map||!length||address>~(UINT64)0-length)return FALSE;
    for(UINTN off=0;off<firmware_map_bytes;off+=firmware_descriptor_bytes) {
        EFI_MEMORY_DESCRIPTOR *d=(VOID *)((UINT8 *)firmware_map+off);
        /* No dereference of MMIO, port-space, unusable or unclassified memory. */
        if(d->Type!=EfiLoaderCode&&d->Type!=EfiLoaderData&&d->Type!=EfiBootServicesCode&&
           d->Type!=EfiBootServicesData&&d->Type!=EfiRuntimeServicesCode&&d->Type!=EfiRuntimeServicesData&&
           d->Type!=EfiConventionalMemory&&d->Type!=EfiACPIReclaimMemory&&d->Type!=EfiACPIMemoryNVS)continue;
        if(d->NumberOfPages>~(UINT64)0/4096)continue;
        UINT64 bytes=d->NumberOfPages*4096;
        if(address>=d->PhysicalStart&&address-d->PhysicalStart<=bytes&&length<=bytes-(address-d->PhysicalStart))return TRUE;
    }
    return FALSE;
}
static BOOLEAN anchor(const UINT8 *p,const char *text,UINTN n)
{for(UINTN i=0;i<n;i++)if(p[i]!=(UINT8)text[i])return FALSE;return TRUE;}
static BOOLEAN checksum(const UINT8 *p,UINTN n)
{UINT8 sum=0;for(UINTN i=0;i<n;i++)sum+=p[i];return sum==0;}
static void smbios_table(const UINT8 *entry,BOOLEAN version3)
{
    UINTN minimum=version3?24:31;
    if(!ram_readable((UINTN)entry,minimum)) {failure("smbios_entry_memory",EFI_COMPROMISED_DATA);return;}
    UINTN elen=entry[version3?6:5];
    if(!anchor(entry,version3?"_SM3_":"_SM_",version3?5:4)||elen<minimum||elen>64||
       !ram_readable((UINTN)entry,elen)||!checksum(entry,elen)) {failure("smbios_entry",EFI_COMPROMISED_DATA);return;}
    if(!version3&&(!anchor(entry+16,"_DMI_",5)||!checksum(entry+16,15))) {failure("smbios_dmi",EFI_COMPROMISED_DATA);return;}
    UINTN major=entry[version3?7:6],minor=entry[version3?8:7];
    UINT64 address=inv_le(entry+(version3?16:24),version3?8:4);
    UINTN size=inv_le(entry+(version3?12:22),version3?4:2);
    if(!size||size>2*1024*1024||!ram_readable(address,size)) {failure("smbios_table_memory",EFI_COMPROMISED_DATA);return;}
    begin("SMBIOS_TABLE");field("version3",version3);field("major",major);field("minor",minor);field("address",address);field("maximum_bytes",size);emit();
    const UINT8 *p=(VOID *)(UINTN)address;UINTN offset=0,count=0,uuid_count=0;
    while(offset+4<=size&&count++<4096) {
        UINTN len=p[offset+1],type=p[offset];
        if(len<4||len>size-offset)break;
        if(type==1&&len>=25) {
            const UINT8 *u=p+offset+8;UINT8 normalized[16];
            for(UINTN i=0;i<16;i++)normalized[i]=u[i];
            if(major>2||(major==2&&minor>=6)) {
                normalized[0]=u[3];normalized[1]=u[2];normalized[2]=u[1];normalized[3]=u[0];
                normalized[4]=u[5];normalized[5]=u[4];normalized[6]=u[7];normalized[7]=u[6];
            }
            begin("SMBIOS_UUID");field("version3",version3);add(" raw=");bytes_hex(u,16);add(" canonical=");
            for(UINTN i=0;i<16;i++){if(i==4||i==6||i==8||i==10)add("-");bytes_hex(normalized+i,1);}emit();uuid_count++;
        }
        UINTN tail=offset+len;
        while(tail+1<size&&(p[tail]!=0||p[tail+1]!=0))tail++;
        if(tail+1>=size)break;
        offset=tail+2;
        if(type==127){begin("SMBIOS_END");field("version3",version3);field("structures",count);field("uuid_count",uuid_count);emit();if(uuid_count!=1)failure("smbios_uuid_count",EFI_COMPROMISED_DATA);return;}
    }
    failure("smbios_structures",EFI_COMPROMISED_DATA);
}
static void smbios(void)
{
    EFI_GUID old={0xeb9d2d31,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
    EFI_GUID newer={0xf2fd1544,0x9794,0x4a2c,{0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94}};
    UINTN found=0;
    if(st->NumberOfTableEntries>1024||!st->ConfigurationTable){failure("smbios_configuration_bounds",EFI_COMPROMISED_DATA);return;}
    for(UINTN i=0;i<st->NumberOfTableEntries;i++){
        EFI_CONFIGURATION_TABLE *t=&st->ConfigurationTable[i];
        if(!CompareGuid(&t->VendorGuid,&old)){smbios_table(t->VendorTable,FALSE);found++;}
        if(!CompareGuid(&t->VendorGuid,&newer)){smbios_table(t->VendorTable,TRUE);found++;}
    }
    if(!found)failure("smbios_missing",EFI_NOT_FOUND);
}

#endif
