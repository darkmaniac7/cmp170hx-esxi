# Makefile -- build the CMP 170HX BAR1-to-64GiB UEFI writer (gnu-efi, x86_64).
#
# This reproduces the plain gnu-efi compile the lab review harness performed.
# Targets:
#   make four-card   (default)  -> build/four-card/BOOTX64.EFI
#   make two-card               -> build/two-card/BOOTX64.EFI
#   make all                    -> both variants
#   make clean                  -> remove build/
#
# GNUEFI must point at a gnu-efi install tree providing:
#   include/efi, include/efi/x86_64,
#   lib/crt0-efi-x86_64.o, lib/elf_x86_64_efi.lds, lib/libefi.a, lib/libgnuefi.a
GNUEFI ?= /usr

# Build-policy defines consumed by <variant>/src/handoff_efi.c.
# In the lab these were emitted into the generated build_pins.h; for a plain
# source build they are explicit build inputs:
#   WRITER_POLICY_BOUND=1  the writer binding has been reviewed/accepted for this build.
#   WRITER_EXECUTE=1       perform the BAR stores on real hardware.
#                          WRITER_EXECUTE=0 means observe-only (no stores); note the
#                          current source #errors unless EXECUTE=1, by design, so an
#                          observe-only build is a deliberate source change, not a flag flip.
# (HANDOFF_SYNTHETIC=0 and PHYSICAL_BINDING_VERIFIED=1 come from src/build_pins.h.)
POLICY := -DWRITER_POLICY_BOUND=1 -DWRITER_EXECUTE=1

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy

# Freestanding UEFI compile flags. -maccumulate-outgoing-args is required by some
# gcc versions when combining the MS ABI with -mno-red-zone.
CFLAGS := -I$(GNUEFI)/include/efi -I$(GNUEFI)/include/efi/x86_64 \
          -DGNU_EFI_USE_MS_ABI \
          -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -Wall \
          -maccumulate-outgoing-args
LDFLAGS := -nostdlib -znocombreloc -T $(GNUEFI)/lib/elf_x86_64_efi.lds -shared -Bsymbolic
SECTIONS := -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc

BUILD := build

.DEFAULT_GOAL := four-card
.PHONY: all four-card two-card clean
all: four-card two-card
four-card: $(BUILD)/four-card/BOOTX64.EFI
two-card:  $(BUILD)/two-card/BOOTX64.EFI

define VARIANT_rules
$(BUILD)/$(1)/BOOTX64.EFI: $(BUILD)/$(1)/writer.so
	$(OBJCOPY) $(SECTIONS) --target=efi-app-x86_64 $$< $$@

$(BUILD)/$(1)/writer.so: $(BUILD)/$(1)/writer.o
	$(LD) $(LDFLAGS) $(GNUEFI)/lib/crt0-efi-x86_64.o $$< -L$(GNUEFI)/lib -lefi -lgnuefi -o $$@

$(BUILD)/$(1)/writer.o: $(1)/src/handoff_efi.c | $(BUILD)/$(1)
	$(CC) $(CFLAGS) -I$(1)/src $(POLICY) -c $$< -o $$@

$(BUILD)/$(1):
	mkdir -p $$@
endef

$(eval $(call VARIANT_rules,four-card))
$(eval $(call VARIANT_rules,two-card))

clean:
	rm -rf $(BUILD)
