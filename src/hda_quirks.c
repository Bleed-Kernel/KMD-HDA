#include "hda_priv.h"
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>

// apple powers its amps off gpios and pin vrefs
// all from linux's alc882 LONG LIVE LINUX 
typedef struct {
    uint32_t ssid;
    uint8_t  vref50[2];
    bool     hp_vref;
    uint8_t  gpio;
    bool     coef_eapd;
} hda_quirk_t;

static const hda_quirk_t hda_apple[] = {
    { 0x106B00A0, { 0,    0    }, true,  1, false },    // macbook pro 3,1
    { 0x106B00A1, { 0,    0    }, true,  1, false },    // macbook
    { 0x106B00A4, { 0,    0    }, true,  1, false },    // macbook pro 4,1
    { 0x106B0C00, { 0x18, 0    }, false, 3, false },    // mac pro
    { 0x106B1000, { 0,    0    }, false, 3, false },    // imac 24
    { 0x106B2800, { 0,    0    }, false, 3, false },    // apple tv
    { 0x106B2C00, { 0,    0    }, true,  1, false },    // macbook pro rev3
    { 0x106B3000, { 0,    0    }, true,  1, false },    // imac
    { 0x106B3200, { 0,    0    }, false, 0, true  },    // imac 7,1
    { 0x106B3400, { 0x18, 0    }, true,  1, false },    // macbook air 1,1
    { 0x106B3500, { 0x18, 0x19 }, true,  1, false },    // macbook air 2,1
    { 0x106B3600, { 0,    0    }, true,  1, false },    // macbook 3,1
    { 0x106B3800, { 0,    0    }, true,  1, false },    // macbook pro 4,1
    { 0x106B3E00, { 0,    0    }, false, 3, false },    // imac 24 aluminum
    { 0x106B3F00, { 0x18, 0x1A }, false, 1, false },    // macbook 5,1
    { 0x106B4000, { 0x18, 0x1A }, false, 1, false },    // macbook pro 5,1
    { 0x106B4100, { 0x18, 0x1A }, false, 1, false },    // mac mini 3,1
    { 0x106B4200, { 0,    0    }, true,  3, false },    // mac pro 4,1
    { 0x106B4300, { 0x18, 0x1A }, false, 1, false },    // imac 9,1
    { 0x106B4600, { 0x18, 0x1A }, false, 1, false },    // macbook pro 5,2
    { 0x106B4900, { 0x18, 0x1A }, false, 1, false },    // imac 9,1 aluminum
    { 0x106B4A00, { 0x18, 0    }, true,  1, false },    // macbook 5,2
};

static const uint8_t hda_hp_pins[] = { 0x14, 0x15, 0x19 };

static void hda_pin_vref(hda_t *c, uint8_t nid, uint8_t vref) {
    uint32_t ctl = 0;
    hda_verb(c, nid, HDA_VERB_GET_PIN_CTL, 0, &ctl);
    ctl |= vref;
    hda_verb(c, nid, HDA_VERB_SET_PIN_CTL, (uint8_t)ctl, NULL);
    serial_printf(LOG_INFO "hda: pin 0x%x vref on, ctl 0x%x\n", nid, ctl);

    // headphone switching rewrites pin ctl, it has to keep this
    for (uint32_t i = 0; i < c->route_count; i++) {
        if (c->routes[i].pin == nid)
            c->routes[i].pin_ctl = (uint8_t)ctl;
    }
}

static void hda_gpio_on(hda_t *c, uint8_t mask) {
    hda_verb(c, c->afg, HDA_VERB_SET_GPIO_MASK, mask, NULL);
    hda_verb(c, c->afg, HDA_VERB_SET_GPIO_DIR, mask, NULL);
    hpet_busy_wait_ms(1);
    hda_verb(c, c->afg, HDA_VERB_SET_GPIO_DATA, mask, NULL);
    serial_printf(LOG_INFO "hda: gpio mask 0x%x high\n", mask);
}

void hda_codec_quirks(hda_t *c) {
    uint32_t ssid = 0;
    hda_verb(c, c->afg, HDA_VERB_GET_SUBSYSTEM, 0, &ssid);
    serial_printf(LOG_INFO "hda: codec subsystem %08x\n", ssid);
    if (c->vendor != HDA_REALTEK_VENDOR)
        return;

    for (size_t i = 0; i < sizeof(hda_apple) / sizeof(hda_apple[0]); i++) {
        const hda_quirk_t *q = &hda_apple[i];
        if (q->ssid != ssid)
            continue;

        serial_printf(LOG_INFO "hda: apple quirk %08x\n", ssid);
        for (int v = 0; v < 2; v++) {
            if (q->vref50[v])
                hda_pin_vref(c, q->vref50[v], HDA_PIN_VREF_50);
        }

        if (q->hp_vref) {
            for (size_t p = 0; p < sizeof(hda_hp_pins); p++) {
                uint32_t cfg = 0;
                hda_verb(c, hda_hp_pins[p], HDA_VERB_GET_CONFIG, 0, &cfg);
                if (HDA_CFG_DEVICE(cfg) == HDA_DEV_HP_OUT) {
                    hda_pin_vref(c, hda_hp_pins[p], HDA_PIN_VREF_80);
                    break;
                }
            }
        }

        if (q->gpio)
            hda_gpio_on(c, q->gpio);

        if (q->coef_eapd) {
            hda_verb4(c, HDA_REALTEK_COEF_NID, HDA_VERB4_SET_COEF_INDEX, 0x07);
            hda_verb4(c, HDA_REALTEK_COEF_NID, HDA_VERB4_SET_PROC_COEF, 0x3060);
        }
        return;
    }
}
