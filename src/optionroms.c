// Option rom scanning code.
//
// Copyright (C) 2008  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2002  MandrakeSoft S.A.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "bregs.h" // struct bregs
#include "config.h" // CONFIG_*
#include "farptr.h" // FLATPTR_TO_SEG
#include "biosvar.h" // GET_IVT
#include "hw/pci.h" // pci_config_readl
#include "hw/pcidevice.h" // foreachpci
#include "hw/pci_ids.h" // PCI_CLASS_DISPLAY_VGA
#include "hw/pci_regs.h" // PCI_ROM_ADDRESS
#include "malloc.h" // rom_confirm
#include "output.h" // dprintf
#include "romfile.h" // romfile_loadint
#include "stacks.h" // farcall16big
#include "std/optionrom.h" // struct rom_header
#include "std/pnpbios.h" // PNP_SIGNATURE
#include "string.h" // memset
#include "util.h" // get_pnp_offset
#include "tcgbios.h" // tpm_*

static int EnforceChecksum, S3ResumeVga, RunPCIroms;


/****************************************************************
 * Helper functions
 ****************************************************************/

// Execute a given option rom.
static void
__callrom(struct rom_header *rom, u16 offset, u16 bdf)
{
    u16 seg = FLATPTR_TO_SEG(rom);
    dprintf(1, "Running option rom at %04x:%04x\n", seg, offset);

    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ax = bdf;
    br.bx = 0xffff;
    br.dx = 0xffff;
    br.es = SEG_BIOS;
    br.di = get_pnp_offset();
    br.code = SEGOFF(seg, offset);
    start_preempt();
    farcall16big(&br);
    finish_preempt();

//dprintf(1, "after option rom\n");
}

// Execute a given option rom at the standard entry vector.
void
callrom(struct rom_header *rom, u16 bdf)
{
    __callrom(rom, OPTION_ROM_INITVECTOR, bdf);
}

// Execute a BCV option rom registered via add_bcv().
void
call_bcv(u16 seg, u16 ip)
{
    __callrom(MAKE_FLATPTR(seg, 0), ip, 0);
}

// Verify that an option rom looks valid
static int
is_valid_rom(struct rom_header *rom)
{
    dprintf(1, "Checking rom %p (sig %x size %d B)\n"
            , rom, rom->signature, rom->size * 512);
    if (rom->signature != OPTION_ROM_SIGNATURE)
        return 0;
    if (! rom->size)
        return 0;
    u32 len = rom->size * 512;
    u8 sum = checksum(rom, len);
    if (sum != 0) {
        dprintf(1, "Found option rom with bad checksum: loc=%p len=%d sum=%x\n"
                , rom, len, sum);
        if (EnforceChecksum)
            return 0;
    }
    return 1;
}

// Check if a valid option rom has a pnp struct; return it if so.
static struct pnp_data *
get_pnp_rom(struct rom_header *rom)
{
    struct pnp_data *pnp = (void*)((u8*)rom + rom->pnpoffset);
    if (pnp->signature != PNP_SIGNATURE)
        return NULL;
    return pnp;
}

// Check for multiple pnp option rom headers.
static struct pnp_data *
get_pnp_next(struct rom_header *rom, struct pnp_data *pnp)
{
    if (! pnp->nextoffset)
        return NULL;
    pnp = (void*)((u8*)rom + pnp->nextoffset);
    if (pnp->signature != PNP_SIGNATURE)
        return NULL;
    return pnp;
}

// Check if a valid option rom has a pci struct; return it if so.
static struct pci_data *
get_pci_rom(struct rom_header *rom)
{
    struct pci_data *pd = (void*)((u32)rom + rom->pcioffset);
    if (pd->signature != PCI_ROM_SIGNATURE)
        return NULL;
    if (rom->pcioffset & 3)
        dprintf(1, "WARNING! Found unaligned PCI rom (vd=%04x:%04x)\n"
                , pd->vendor, pd->device);
    return pd;
}

// Run rom init code and note rom size.
static int
init_optionrom(struct rom_header *rom, u16 bdf, int isvga)
{
    dprintf(1, "init_optionrom %p\n", rom);

    if (! is_valid_rom(rom)) {
        dprintf(1, "NOT VALID ROM?\n");
        return -1;
    }

    u8 romblocks = rom->size;

dprintf(1, "ROM size %u*512 B\n", romblocks);

    struct rom_header *newrom = rom_reserve(romblocks * 512);
    if (!newrom) {
        warn_noalloc();
        return -1;
    }

    dprintf(1, "ROM memmove test %p %p\n", newrom, rom);

    if (newrom != rom) {
        memmove(newrom, rom, romblocks * 512);
    }

dprintf(1, "newROM size %u*512 B\n", newrom->size);

    tpm_option_rom(newrom, romblocks * 512);

    // dprintf(1, "IO4\n");

    if (isvga || get_pnp_rom(newrom)) {
        dprintf(1, "IO5 !!! callrom (VGA or PNP)\n");

wbinvd();

        // Only init vga and PnP roms here.
        callrom(newrom, bdf);
    }

    dprintf(1, "ROM size after callrom %u*512 B\n", newrom->size);

    //NOTICE should use which original rom size was copied
    //NOTICE adaptec scsi change its own size of shadow copy
    int ret = rom_confirm(romblocks * 512);

    // dprintf(1, "IO7\n");

    return ret;

}

#define RS_PCIROM (1LL<<33)

static void
setRomSource(u64 *sources, struct rom_header *rom, u64 source)
{
    if (sources)
        sources[((u32)rom - BUILD_ROM_START) / OPTION_ROM_ALIGN] = source;
}

static int
getRomPriority(u64 *sources, struct rom_header *rom, int instance)
{
    u64 source = sources[((u32)rom - BUILD_ROM_START) / OPTION_ROM_ALIGN];
    if (!source)
        return -1;
    if (source & RS_PCIROM)
        return bootprio_find_pci_rom((void*)(u32)source, instance);
    struct romfile_s *file = (void*)(u32)source;
    return bootprio_find_named_rom(file->name, instance);
}


/****************************************************************
 * Roms in CBFS
 ****************************************************************/

static struct rom_header *
deploy_romfile(struct romfile_s *file)
{
    u32 size = file->size;
    struct rom_header *rom = rom_reserve(size);
    if (!rom) {
        warn_noalloc();
        return NULL;
    }
    int ret = file->copy(file, rom, size);
    if (ret <= 0)
        return NULL;
    return rom;
}

// Run all roms in a given CBFS directory.
static void
run_file_roms(const char *prefix, int isvga, u64 *sources)
{
    struct romfile_s *file = NULL;
    for (;;) {
        file = romfile_findprefix(prefix, file);
        if (!file)
            break;
        struct rom_header *rom = deploy_romfile(file);
        if (rom) {
            setRomSource(sources, rom, (u32)file);
            init_optionrom(rom, 0, isvga);
        }
    }
}


/****************************************************************
 * PCI roms
 ****************************************************************/

// Verify device is a vga device with legacy address decoding enabled.
int
is_pci_vga(struct pci_device *pci)
{
    if (pci->class != PCI_CLASS_DISPLAY_VGA)
        return 0;
    u16 cmd = pci_config_readw(pci->bdf, PCI_COMMAND);
    if (!(cmd & PCI_COMMAND_IO && cmd & PCI_COMMAND_MEMORY))
        return 0;
    while (pci->parent) {
        pci = pci->parent;
        u32 ctrl = pci_config_readb(pci->bdf, PCI_BRIDGE_CONTROL);
        if (!(ctrl & PCI_BRIDGE_CTL_VGA))
            return 0;
    }
    return 1;
}

// Copy a rom to its permanent location below 1MiB
static struct rom_header *
copy_rom(struct rom_header *rom)
{
    u32 romsize = rom->size * 512;
    struct rom_header *newrom = rom_reserve(romsize);
    if (!newrom) {
        warn_noalloc();
        return NULL;
    }
    dprintf(1, "Copying option rom (size %d) from %p to %p\n"
            , romsize, rom, newrom);

    iomemcpy(newrom, rom, romsize);

    return newrom;
}

// Map the option rom of a given PCI device.
static struct rom_header *
map_pcirom(struct pci_device *pci)
{
    dprintf(1, "Attempting to map option rom on dev %pP\n", pci);

    if ((pci->header_type & 0x7f) != PCI_HEADER_TYPE_NORMAL) {
        dprintf(1, "Skipping non-normal pci device (type=%x)\n"
                , pci->header_type);
        return NULL;
    }

    u16 bdf = pci->bdf;
    u32 orig = pci_config_readl(bdf, PCI_ROM_ADDRESS);
    pci_config_writel(bdf, PCI_ROM_ADDRESS, ~PCI_ROM_ADDRESS_ENABLE);
    u32 sz = pci_config_readl(bdf, PCI_ROM_ADDRESS);

    dprintf(1, "Option rom sizing returned %x %x\n", orig, sz);
    orig &= ~PCI_ROM_ADDRESS_ENABLE;
    if (!sz || sz == 0xffffffff)
        goto fail;

    // TODO machine with less than 16MiB RAM
    // UMC can have PCI space right after RAM
    // if ((orig == sz) || ((u32)(orig + 4*1024*1024) < 20*1024*1024)) {
    if ((orig == sz) || ((u32) orig < 4*1024*1024)) {
        // Don't try to map to a pci addresses at its max, in the last
        // 4MiB of ram, or the first 16MiB of ram.
        dprintf(1, "Preset rom address doesn't look valid\n");
        goto fail;
    }

    // Looks like a rom - enable it.
    pci_config_writel(bdf, PCI_ROM_ADDRESS, orig | PCI_ROM_ADDRESS_ENABLE);

    struct rom_header *rom = (void*)orig;
    for (;;) {
        dprintf(1, "Inspecting possible rom at %p (vd=%04x:%04x bdf=%pP)\n"
                , rom, pci->vendor, pci->device, pci);
        if (rom->signature != OPTION_ROM_SIGNATURE) {
            dprintf(1, "No option rom signature (got %x)\n", rom->signature);
        //ignore signature
            // goto fail;
        }
        struct pci_data *pd = get_pci_rom(rom);
        if (! pd) {
            dprintf(1, "No valid pci signature found\n");
        //ignore signature
            // goto fail;
        }

        if (pd->vendor == pci->vendor && pd->device == pci->device
            && pd->type == PCIROM_CODETYPE_X86)
            // A match
            break;
        dprintf(1, "Didn't match dev/ven (got %04x:%04x) or type (got %d)\n"
                , pd->vendor, pd->device, pd->type);
        if (pd->indicator & 0x80) {
            dprintf(1, "No more images left\n");
            goto fail;
        }
        rom = (void*)((u32)rom + pd->ilen * 512);
    }

    rom = copy_rom(rom);
    pci_config_writel(bdf, PCI_ROM_ADDRESS, orig);
    return rom;
fail:
    // Not valid - restore original and exit.
    pci_config_writel(bdf, PCI_ROM_ADDRESS, orig);
    return NULL;
}

static int boot_irq_captured(void)
{
    return GET_IVT(0x19).segoff != FUNC16(entry_19_official).segoff;
}

static void boot_irq_restore(void)
{
    struct segoff_s seabios;

    seabios = FUNC16(entry_19_official);
    SET_IVT(0x19, seabios);
}

// Attempt to map and initialize the option rom on a given PCI device.
static void
init_pcirom(struct pci_device *pci, int isvga, u64 *sources)
{
    dprintf(1, "Attempting to init PCI bdf %pP (vd %04x:%04x)\n"
            , pci, pci->vendor, pci->device);

    char fname[17];
    snprintf(fname, sizeof(fname), "pci%04x,%04x.rom"
             , pci->vendor, pci->device);
    struct romfile_s *file = romfile_find(fname);
    struct rom_header *rom = NULL;
    if (file) {
        rom = deploy_romfile(file);
dprintf(1, "PCIROM1 %p\n", rom);
        }
    else if (RunPCIroms > 1 || (RunPCIroms == 1 && isvga)) {
        rom = map_pcirom(pci);
dprintf(1, "PCIROM2 %p\n", rom);
        }
    if (! rom) {
dprintf(1, "PCI NO ROM\n");
        // No ROM present.
        return;
        }
    int irq_was_captured = boot_irq_captured();
    struct pnp_data *pnp = get_pnp_rom(rom);
    setRomSource(sources, rom, RS_PCIROM | (u32)pci);

dprintf(1, "PCI bef init\n");
    init_optionrom(rom, pci->bdf, isvga);
    if (boot_irq_captured() && !irq_was_captured &&
        !file && !isvga && pnp) {
        // This PCI rom is misbehaving - recapture the boot irqs
        char *desc = MAKE_FLATPTR(FLATPTR_TO_SEG(rom), pnp->productname);
        dprintf(1, "PnP optionrom \"%s\" (bdf %pP) captured int19, restoring\n",
                desc, pci);
        boot_irq_restore();
    }
}


/****************************************************************
 * Non-VGA option rom init
 ****************************************************************/

void
optionrom_setup(void)
{
    if (! CONFIG_OPTIONROMS)
        return;

	//TODO should first do ISA ROM and then PCI ROM

rom_set_last(0xd0000);

    dprintf(1, "Scan for option roms\n");
    u64 sources[(BUILD_BIOS_ADDR - BUILD_ROM_START) / OPTION_ROM_ALIGN];
    memset(sources, 0, sizeof(sources));
    u32 post_vga = rom_get_last();

dprintf(1, "==== rest of option ROMs @%x\n", post_vga);

    // Find and deploy PCI roms.
    struct pci_device *pci;
    foreachpci(pci) {
        if (pci->class == PCI_CLASS_DISPLAY_VGA ||
            pci->class == PCI_CLASS_DISPLAY_OTHER ||
            pci->have_driver)
            continue;
        init_pcirom(pci, 0, sources);
    }

    // Find and deploy CBFS roms not associated with a device.
    run_file_roms("genroms/", 0, sources);
    rom_reserve(0);

    // All option roms found and deployed - now build BEV/BCV vectors.

dprintf(1, "rom end2 %x\n", rom_get_last());

    u32 pos = post_vga;

    //TODO temp
    // pos = 0xd0000;

    //NOTICE rescan ROM space only until end of detected ROMs
    while (pos < rom_get_last()) {
    // while (pos < 0xe0000) {	//pc2005

dprintf(1, "rom end3 %x, pos %x\n", rom_get_last(), pos);

        struct rom_header *rom = (void*)pos;
        if (! is_valid_rom(rom)) {
            pos += OPTION_ROM_ALIGN;
            continue;
        }
        pos += ALIGN(rom->size * 512, OPTION_ROM_ALIGN);

// pos += ALIGN(pos, 0x10000);

dprintf(1, "pre pnp\n");

        struct pnp_data *pnp = get_pnp_rom(rom);
        if (! pnp) {
            // Legacy rom.
            boot_add_bcv(FLATPTR_TO_SEG(rom), OPTION_ROM_INITVECTOR, 0
                         , getRomPriority(sources, rom, 0));
            continue;
        }

dprintf(1, "post pnp\n");

        // PnP rom - check for BEV and BCV boot capabilities.
        int instance = 0;
        while (pnp) {
            if (pnp->bev)
                boot_add_bev(FLATPTR_TO_SEG(rom), pnp->bev, pnp->productname
                             , getRomPriority(sources, rom, instance++));
            else if (pnp->bcv)
                boot_add_bcv(FLATPTR_TO_SEG(rom), pnp->bcv, pnp->productname
                             , getRomPriority(sources, rom, instance++));
            else
                break;
            pnp = get_pnp_next(rom, pnp);
        }
    }
}


/****************************************************************
 * VGA init
 ****************************************************************/

int ScreenAndDebug;
struct rom_header *VgaROM;

static int try_setup_display_other(void)
{
    struct pci_device *pci;

    dprintf(1, "No VGA found, scan for other PCI display\n");

    foreachpci(pci) {
        if (pci->class != PCI_CLASS_DISPLAY_OTHER)
            continue;
        struct rom_header *rom = map_pcirom(pci);
        if (!rom)
            continue;
        dprintf(1, "Other display found at %pP\n", pci);
        pci_config_maskw(pci->bdf, PCI_COMMAND, 0,
                         PCI_COMMAND_IO | PCI_COMMAND_MEMORY);

        if (init_optionrom(rom, pci->bdf, 1)) {
        //not found
        return -1;
    }

    //found
        return 0;
    }

    return -1;
}

// Call into vga code to turn on console.
void
vgarom_setup(void)
{
    int have_vga = 0;

    if (! CONFIG_OPTIONROMS)
        return;

    dprintf(1, "Scan for VGA option rom\n");

    // Load some config settings that impact VGA.
    EnforceChecksum = romfile_loadint("etc/optionroms-checksum", 0);
    S3ResumeVga = romfile_loadint("etc/s3-resume-vga-init", CONFIG_QEMU);
    RunPCIroms = romfile_loadint("etc/pci-optionrom-exec", 2);
    ScreenAndDebug = romfile_loadint("etc/screen-and-debug", 1);

    // NOTICE expect to have shadow enabled
    // NOTICE expect writeable
    // NOTICE probably no caching

    // Clear option rom memory
    memset((void*)BUILD_ROM_START, 0, rom_get_max() - BUILD_ROM_START);

// dprintf(1, " SSS %x\n", rom_get_max() - BUILD_ROM_START);

    // Find and deploy PCI VGA rom.
    struct pci_device *pci;
    foreachpci(pci) {
        if (!is_pci_vga(pci)) {
            continue;
        }

        dprintf(1, " found PCI VGA\n");
        vgahook_setup(pci);
        init_pcirom(pci, 1, NULL);
        have_vga = 1;
        break;
    }

    if (!have_vga) {
        if (!try_setup_display_other()) {
            have_vga = 1;
        }
    }

    if (!have_vga) {
        // Find and deploy CBFS vga-style roms not associated with a device.
        run_file_roms("vgaroms/", 1, NULL);
        rom_reserve(0);

        if (rom_get_last() != BUILD_ROM_START) {
            // VGA rom found
            VgaROM = (void*)BUILD_ROM_START;

            have_vga = 1;
        }
    }

    if (!have_vga) {
        dprintf(1, " check ISA VGA\n");

#if 1   //UMC
        //set from RAM shadow to ROM
        const u16 bdf = pci_to_bdf(0, 0x10, 0);

        u8 val = pci_config_readb(bdf, 0x54);
        val &= ~2;  //C_0000 - C_7FFF
        val &= ~4;  //C_8000 - C_BFFF
        val &= ~8;  //C_C000 - C_FFFF
        pci_config_writeb(bdf, 0x54, val);

        //TODO on WT-only 486
        wbinvd();
        // asm volatile("invd": : :"memory");
#endif

        struct rom_header *rom = (struct rom_header *) 0xc0000;

        if (rom->size > (0x8000/512)) {
            dprintf(1, "fixme: ISA VGA ROM is >32kiB\n");
        }

        if (! init_optionrom(rom, 0, 1)) {
            dprintf(1, "use ISA VGA ROM, size %u\n", rom->size*512);
            have_vga = 1;
        } else {
#if 0   //UMC
            //set from ROM to RAM shadow
            const u16 bdf = pci_to_bdf(0, 0x10, 0);
            u8 val = pci_config_readb(bdf, 0x54);
            val |= 2;
            pci_config_writeb(bdf, 0x54, val);

            //TODO on WT-only 486
            wbinvd();
            // asm volatile("invd": : :"memory");
#endif
        }
    }
}

void
s3_resume_vga(void)
{
    if (!S3ResumeVga)
        return;
    if (!VgaROM || ! is_valid_rom(VgaROM))
        return;
    callrom(VgaROM, 0);
}
