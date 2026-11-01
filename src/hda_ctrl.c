#include "hda_priv.h"
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <mm/dma.h>
#include <mm/paging.h>
#include <ansii.h>

static bool hda_wait8(hda_t *c, uint32_t reg, uint8_t mask, bool set, uint32_t ms) {
    uint64_t start = hpet_get_femtoseconds();
    while (!hda_waited(start, ms)) {
        if (((hda_read8(c, reg) & mask) != 0) == set)
            return true;
        asm volatile("pause");
    }
    return false;
}

static bool hda_wait16(hda_t *c, uint32_t reg, uint16_t mask, bool set, uint32_t ms) {
    uint64_t start = hpet_get_femtoseconds();
    while (!hda_waited(start, ms)) {
        if (((hda_read16(c, reg) & mask) != 0) == set)
            return true;
        asm volatile("pause");
    }
    return false;
}

void hda_pci_quirks(hda_t *c) {
    pci_device_t *p = &c->pci;

    // non zero traffic class is static on the output of some codecs
    if (p->vendor_id == PCI_VENDOR_INTEL) {
        uint16_t tcsel = pci_read16(p->bus, p->device, p->function, HDA_PCI_TCSEL);
        pci_write16(p->bus, p->device, p->function, HDA_PCI_TCSEL, tcsel & ~7u);

        // the ring is cached memory, the controller has to snoop it, hence the name
        uint16_t devc = pci_read16(p->bus, p->device, p->function, HDA_PCI_INTEL_DEVC);
        if (devc & HDA_DEVC_NOSNOOP) {
            serial_printf(LOG_INFO "hda: intel devc 0x%x had no snoop set, clearing it\n", devc);
            pci_write16(p->bus, p->device, p->function, HDA_PCI_INTEL_DEVC, devc & ~HDA_DEVC_NOSNOOP);
        }
    }

    // streams have their own coherency bits, the corb and rirb work fine without them
    if (p->vendor_id == PCI_VENDOR_NVIDIA) {
        uint16_t trans = pci_read16(p->bus, p->device, p->function, HDA_PCI_NV_TRANSREG);
        uint16_t coh   = pci_read16(p->bus, p->device, p->function, HDA_PCI_NV_OSTRM_COH);
        pci_write16(p->bus, p->device, p->function, HDA_PCI_NV_TRANSREG, (trans & ~0xFu) | HDA_NV_TRANS_COH);
        pci_write16(p->bus, p->device, p->function, HDA_PCI_NV_OSTRM_COH, coh | HDA_NV_STREAM_COH);
        serial_printf(LOG_INFO "hda: nvidia snoop on, trans 0x%x -> 0x%x, coh 0x%x -> 0x%x\n", trans,
                      pci_read16(p->bus, p->device, p->function, HDA_PCI_NV_TRANSREG), coh,
                      pci_read16(p->bus, p->device, p->function, HDA_PCI_NV_OSTRM_COH));
    }

    // these say they do 64 bit dma and some revisions dont? we can handle it though
    bool ati_sb = p->vendor_id == PCI_VENDOR_ATI && (p->device_id == 0x437B || p->device_id == 0x4383);
    if (ati_sb)
        c->ok64 = false;

    if (ati_sb || p->vendor_id == PCI_VENDOR_AMD) {
        uint16_t misc = pci_read16(p->bus, p->device, p->function, HDA_PCI_ATI_MISC);
        pci_write16(p->bus, p->device, p->function, HDA_PCI_ATI_MISC, (misc & ~7u) | HDA_PCI_ATI_SNOOP);
        serial_printf(LOG_INFO "hda: amd snoop on, misc 0x%x -> 0x%x\n", misc,
                      pci_read16(p->bus, p->device, p->function, HDA_PCI_ATI_MISC));
    }
}

bool hda_ctrl_reset(hda_t *c) {
    // we cannot trust firmware to fully shut it down
    hda_write8(c, HDA_CORBCTL, 0);
    hda_write8(c, HDA_RIRBCTL, 0);
    hda_wait8(c, HDA_CORBCTL, HDA_CORBCTL_RUN, false, HDA_STREAM_WAIT_MS);
    hda_wait8(c, HDA_RIRBCTL, HDA_RIRBCTL_RUN, false, HDA_STREAM_WAIT_MS);

    uint32_t streams = HDA_GCAP_ISS(c->gcap) + HDA_GCAP_OSS(c->gcap) + HDA_GCAP_BSS(c->gcap);
    for (uint32_t i = 0; i < streams; i++) {
        uint32_t reg = HDA_SD_BASE + i * HDA_SD_SIZE + HDA_SD_CTL;
        hda_write32(c, reg, hda_read32(c, reg) & ~(HDA_SDCTL_RUN | HDA_SDSTS_MASK));
    }
    hda_write32(c, HDA_DPLBASE, 0);
    hda_write32(c, HDA_DPUBASE, 0);
    hda_write32(c, HDA_INTCTL, 0);

    hda_write32(c, HDA_GCTL, hda_read32(c, HDA_GCTL) & ~HDA_GCTL_CRST);
    if (!mmio_wait32(c->mmio, HDA_GCTL, HDA_GCTL_CRST, false, HDA_RESET_WAIT_MS)) {
        serial_printf(LOG_ERROR "hda: controller wont enter reset, gctl 0x%x\n", hda_read32(c, HDA_GCTL));
        return false;
    }
    hpet_busy_wait_ms(1);

    hda_write32(c, HDA_GCTL, hda_read32(c, HDA_GCTL) | HDA_GCTL_CRST);
    if (!mmio_wait32(c->mmio, HDA_GCTL, HDA_GCTL_CRST, true, HDA_RESET_WAIT_MS)) {
        serial_printf(LOG_ERROR "hda: controller wont leave reset, gctl 0x%x\n", hda_read32(c, HDA_GCTL));
        return false;
    }

    // codecs get 521us after reset to say theyre there
    hpet_busy_wait_ms(1);
    serial_printf(LOG_INFO "hda: controller out of reset, gctl 0x%x statests 0x%x\n", hda_read32(c, HDA_GCTL),
                  hda_read16(c, HDA_STATESTS));
    return true;
}

bool hda_cmd_alloc(hda_t *c) {
    void *page = dma_alloc(1, &c->cmd_phys, !c->ok64);
    if (!page) {
        serial_printf(LOG_ERROR "hda: OOM for the corb and rirb\n");
        return false;
    }

    // corb is 1K at most, rirb 2K
    c->corb = page;
    c->rirb = (uint64_t *)((uint8_t *)page + 2048);
    return true;
}

void hda_cmd_free(hda_t *c) {
    dma_free(c->cmd_phys, 1);
    c->cmd_phys = 0;
    c->corb     = NULL;
    c->rirb     = NULL;
}

// biggest size the hardware says it can do
static uint32_t hda_ring_size(hda_t *c, uint32_t reg) {
    uint8_t  size    = hda_read8(c, reg);
    uint8_t  choice  = 0;
    uint32_t entries = 2;

    if (size & (1u << 6)) {
        choice  = 2;
        entries = 256;
    } else if (size & (1u << 5)) {
        choice  = 1;
        entries = 16;
    }
    hda_write8(c, reg, (size & ~3u) | choice);
    return entries;
}

static void hda_rings_start(hda_t *c) {
    c->corb_entries = hda_ring_size(c, HDA_CORBSIZE);
    hda_write32(c, HDA_CORBLBASE, (uint32_t)c->cmd_phys);
    hda_write32(c, HDA_CORBUBASE, (uint32_t)((uint64_t)c->cmd_phys >> 32));

    // qemu clears the bit straight away, real ones want it seen set and then cleared
    hda_write16(c, HDA_CORBRP, HDA_RP_RST);
    hda_wait16(c, HDA_CORBRP, HDA_RP_RST, true, 1);
    hda_write16(c, HDA_CORBRP, 0);
    if (!hda_wait16(c, HDA_CORBRP, HDA_RP_RST, false, HDA_STREAM_WAIT_MS))
        serial_printf(LOG_WARN "hda: corb read pointer stuck in reset, 0x%x\n", hda_read16(c, HDA_CORBRP));
    hda_write16(c, HDA_CORBWP, 0);
    c->corb_wp = 0;

    uint64_t rirb_phys = (uint64_t)c->cmd_phys + 2048;
    c->rirb_entries = hda_ring_size(c, HDA_RIRBSIZE);
    hda_write32(c, HDA_RIRBLBASE, (uint32_t)rirb_phys);
    hda_write32(c, HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));
    hda_write16(c, HDA_RIRBWP, HDA_RP_RST);
    hda_write16(c, HDA_RINTCNT, 1);
    c->rirb_rp = 0;

    hda_write8(c, HDA_RIRBSTS, HDA_RIRBSTS_INT | HDA_RIRBSTS_OVERRUN);
    hda_write8(c, HDA_RIRBCTL, HDA_RIRBCTL_RUN | HDA_RIRBCTL_INT);
    hda_write8(c, HDA_CORBCTL, HDA_CORBCTL_RUN);

    serial_printf(LOG_INFO "hda: corb %u entries, rirb %u entries at 0x%llx\n", c->corb_entries, c->rirb_entries,
                  (uint64_t)c->cmd_phys);
}

void hda_cmd_start(hda_t *c) {
    spinlock_init(&c->cmd_lock);
    hda_rings_start(c);
}

void hda_cmd_stop(hda_t *c) {
    hda_write8(c, HDA_CORBCTL, 0);
    hda_write8(c, HDA_RIRBCTL, 0);
    hda_wait8(c, HDA_CORBCTL, HDA_CORBCTL_RUN, false, HDA_STREAM_WAIT_MS);
    hda_wait8(c, HDA_RIRBCTL, HDA_RIRBCTL_RUN, false, HDA_STREAM_WAIT_MS);
}

static bool hda_send_corb(hda_t *c, uint32_t raw, uint32_t *out) {
    c->corb_wp = (c->corb_wp + 1) % c->corb_entries;
    c->corb[c->corb_wp] = raw;
    asm volatile("mfence");
    hda_write16(c, HDA_CORBWP, c->corb_wp);

    bool     got   = false;
    uint64_t start = hpet_get_femtoseconds();
    while (!hda_waited(start, HDA_CMD_WAIT_MS)) {
        uint16_t wp = hda_read16(c, HDA_RIRBWP) & 0xFF;
        if (wp == c->rirb_rp) {
            asm volatile("pause");
            continue;
        }

        c->rirb_rp = wp;
        uint64_t entry = ((volatile uint64_t *)c->rirb)[wp % c->rirb_entries];
        uint32_t ex    = (uint32_t)(entry >> 32);
        if ((ex & HDA_RIRB_UNSOL) || HDA_RIRB_CAD(ex) != c->cad)
            continue;

        if (out)
            *out = (uint32_t)entry;
        got = true;
        break;
    }
    hda_write8(c, HDA_RIRBSTS, HDA_RIRBSTS_INT | HDA_RIRBSTS_OVERRUN);
    return got;
}

static bool hda_send_immediate(hda_t *c, uint32_t raw, uint32_t *out) {
    if (!hda_wait16(c, HDA_ICS, HDA_ICS_BUSY, false, HDA_CMD_WAIT_MS))
        return false;

    hda_write32(c, HDA_IC, raw);
    hda_write16(c, HDA_ICS, HDA_ICS_VALID);
    hda_write16(c, HDA_ICS, HDA_ICS_BUSY);
    if (!hda_wait16(c, HDA_ICS, HDA_ICS_VALID, true, HDA_CMD_WAIT_MS))
        return false;

    if (out)
        *out = hda_read32(c, HDA_IR);
    return true;
}

static bool hda_send_locked(hda_t *c, uint32_t raw, uint32_t *out) {
    if (c->immediate)
        return hda_send_immediate(c, raw, out);
    if (hda_send_corb(c, raw, out))
        return true;

    // the immediate registers only work with the rings stopped
    serial_printf(LOG_WARN "hda: no answer on the corb to 0x%x, corbrp 0x%x rirbwp 0x%x, trying immediate\n", raw,
                  hda_read16(c, HDA_CORBRP), hda_read16(c, HDA_RIRBWP));
    hda_cmd_stop(c);
    if (hda_send_immediate(c, raw, out)) {
        serial_printf(LOG_INFO "hda: immediate commands work, staying on them\n");
        c->immediate = true;
        return true;
    }

    serial_printf(LOG_WARN "hda: immediate wont answer either, ics 0x%x\n", hda_read16(c, HDA_ICS));
    hda_rings_start(c);
    return false;
}

static bool hda_send(hda_t *c, uint32_t raw, uint32_t *out) {
    unsigned long flags = irq_push();
    spinlock_acquire(&c->cmd_lock);
    bool got = false;
    if (c->cmd_fails < HDA_CMD_MAX_FAILS) {
        got = hda_send_locked(c, raw, out);
        c->cmd_fails = got ? 0 : c->cmd_fails + 1;
        if (c->cmd_fails == HDA_CMD_MAX_FAILS)
            serial_printf(LOG_ERROR "hda: codec %u stopped answering, giving up on it\n", c->cad);
    }
    spinlock_release(&c->cmd_lock);
    irq_restore(flags);
    return got;
}

bool hda_verb(hda_t *c, uint8_t nid, uint32_t verb, uint8_t payload, uint32_t *out) {
    return hda_send(c, ((uint32_t)c->cad << 28) | ((uint32_t)nid << 20) | (verb << 8) | payload, out);
}

bool hda_verb4(hda_t *c, uint8_t nid, uint32_t verb, uint16_t payload) {
    return hda_send(c, ((uint32_t)c->cad << 28) | ((uint32_t)nid << 20) | (verb << 16) | payload, NULL);
}
