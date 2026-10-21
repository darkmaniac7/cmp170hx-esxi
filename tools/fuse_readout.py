import mmap,os,sys,struct
REGS=[(0x820000,0x040,"FUSE_EN_SW_OVERRIDE"),(0x820000,0x084,"DISABLE_SW_OVERRIDE_STATUS"),(0x820000,0x148,"MAGIC_D_gate"),(0x820000,0x228,"OPT_ECC_EN"),(0x820000,0x250,"OPT_DISABLE_GEN3_SPEED"),(0x820000,0x394,"OPT_PCIE_LANE_DISABLE"),(0x820000,0x3f0,"FUSE_FEAT_OVR_DIS"),(0x820000,0x520,"FUSE_PCIE_MAGIC_D"),(0x820000,0x57c,"FUSE_PCIE_GEN23_DIS"),(0x820000,0x580,"FUSE_PCIE_GEN3_DIS"),(0x820000,0x584,"FUSE_DEVID_SW_OVR_DIS"),(0x823000,0x800,"FEAT_OVR_ECC_PLM"),(0x823000,0x804,"FEAT_OVR_PLM"),(0x823000,0x80c,"FEAT_OVR_ECC"),(0x823000,0x810,"FEAT_OVR_ECC_1"),(0x823000,0x814,"FEATURE_READOUT"),(0x823000,0x82c,"FEAT_OVR_ECC_2"),(0x9a0000,0x470,"FBPA_ECC_CTRL")]
for bdf in sys.argv[1:]:
    fd=os.open(f"/sys/bus/pci/devices/0000:{bdf}/resource0",os.O_RDONLY|os.O_SYNC)
    print("==",bdf)
    for page,off,name in REGS:
        m=mmap.mmap(fd,4096,mmap.MAP_SHARED,mmap.PROT_READ,offset=page)
        v=struct.unpack_from("<I",m,off)[0]; m.close()
        extra=""
        if name=="FEATURE_READOUT": extra=f" bit16(ECC_DRAM)={(v>>16)&1}"
        if name=="FUSE_PCIE_MAGIC_D": extra=f" bit25(Gen4dis)={(v>>25)&1}"
        print(f"  {page+off:#08x} {name:28s} = {v:#010x}{extra}")
    os.close(fd)
