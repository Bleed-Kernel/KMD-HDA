#pragma once

// PCI identifiers
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_SUBCLASS_HDA        0x03
#define PCI_SUBCLASS_AUDIO      0x01    // intel files controllers with the dsp on here
#define PCI_PROGIF_HDA_DSP      0x80
#define PCI_VENDOR_AMD          0x1022
#define PCI_VENDOR_ATI          0x1002
#define PCI_VENDOR_NVIDIA       0x10DE

#define HDA_PCI_ATI_MISC        0x42
#define HDA_PCI_ATI_SNOOP       0x02
#define HDA_PCI_TCSEL           0x44
#define HDA_PCI_INTEL_DEVC      0x78
#define HDA_PCI_NV_OSTRM_COH    0x4C    // and the input streams in the byte above
#define HDA_PCI_NV_TRANSREG     0x4E
#define HDA_NV_STREAM_COH       0x0101
#define HDA_NV_TRANS_COH        0x0F
#define HDA_DEVC_NOSNOOP        (1u << 11)

// global registers
#define HDA_GCAP                0x00
#define HDA_VMIN                0x02
#define HDA_VMAJ                0x03
#define HDA_GCTL                0x08
#define HDA_STATESTS            0x0E
#define HDA_INTCTL              0x20
#define HDA_CORBLBASE           0x40
#define HDA_CORBUBASE           0x44
#define HDA_CORBWP              0x48
#define HDA_CORBRP              0x4A
#define HDA_CORBCTL             0x4C
#define HDA_CORBSIZE            0x4E
#define HDA_RIRBLBASE           0x50
#define HDA_RIRBUBASE           0x54
#define HDA_RIRBWP              0x58
#define HDA_RINTCNT             0x5A
#define HDA_RIRBCTL             0x5C
#define HDA_RIRBSTS             0x5D
#define HDA_RIRBSIZE            0x5E
#define HDA_IC                  0x60
#define HDA_IR                  0x64
#define HDA_ICS                 0x68
#define HDA_DPLBASE             0x70
#define HDA_DPUBASE             0x74
#define HDA_SD_BASE             0x80
#define HDA_SD_SIZE             0x20

#define HDA_GCAP_64OK           (1u << 0)
#define HDA_GCAP_BSS(g)         (((g) >> 3) & 0x1F)
#define HDA_GCAP_ISS(g)         (((g) >> 8) & 0xF)
#define HDA_GCAP_OSS(g)         (((g) >> 12) & 0xF)

#define HDA_GCTL_CRST           (1u << 0)
#define HDA_RP_RST              (1u << 15)
#define HDA_CORBCTL_RUN         (1u << 1)
#define HDA_RIRBCTL_INT         (1u << 0)   // we poll, GIE stays clear so this never reaches the bus
#define HDA_RIRBCTL_RUN         (1u << 1)
#define HDA_RIRBSTS_INT         (1u << 0)
#define HDA_RIRBSTS_OVERRUN     (1u << 2)
#define HDA_RIRB_UNSOL          (1u << 4)   // in the high half of an entry, low bits are the codec
#define HDA_RIRB_CAD(e)         ((e) & 0xF)
#define HDA_DPLBASE_ENABLE      (1u << 0)
#define HDA_ICS_BUSY            (1u << 0)
#define HDA_ICS_VALID           (1u << 1)

// stream descriptor, CTL and STS share a dword and we always touch them that way
#define HDA_SD_CTL              0x00
#define HDA_SD_LPIB             0x04
#define HDA_SD_CBL              0x08
#define HDA_SD_LVI              0x0C
#define HDA_SD_FMT              0x12
#define HDA_SD_BDPL             0x18
#define HDA_SD_BDPU             0x1C

#define HDA_SDCTL_SRST          (1u << 0)
#define HDA_SDCTL_RUN           (1u << 1)
#define HDA_SDCTL_TAG_SHIFT     20
#define HDA_SDCTL_TAG_MASK      (0xFu << HDA_SDCTL_TAG_SHIFT)
#define HDA_SDSTS_MASK          0xFF000000u
#define HDA_SDSTS_CLEAR         (0x1Cu << 24)

// verbs
#define HDA_VERB_GET_PARAM      0xF00
#define HDA_VERB_SET_CONN_SEL   0x701
#define HDA_VERB_GET_CONN_LIST  0xF02
#define HDA_VERB_SET_POWER      0x705
#define HDA_VERB_SET_STREAM     0x706
#define HDA_VERB_SET_PIN_CTL    0x707
#define HDA_VERB_GET_PIN_SENSE  0xF09
#define HDA_VERB_SET_PIN_SENSE  0x709
#define HDA_VERB_SET_EAPD       0x70C
#define HDA_VERB_GET_EAPD       0xF0C
#define HDA_VERB_GET_CONFIG     0xF1C
#define HDA_VERB_GET_PIN_CTL    0xF07
#define HDA_VERB_SET_GPIO_DATA  0x715
#define HDA_VERB_SET_GPIO_MASK  0x716
#define HDA_VERB_SET_GPIO_DIR   0x717
#define HDA_VERB_GET_SUBSYSTEM  0xF20
#define HDA_VERB4_SET_COEF_INDEX 0x5
#define HDA_VERB4_SET_PROC_COEF  0x4
#define HDA_VERB4_SET_FORMAT    0x2
#define HDA_VERB4_SET_AMP       0x3

// parameters
#define HDA_PARAM_VENDOR        0x00
#define HDA_PARAM_NODE_COUNT    0x04
#define HDA_PARAM_FG_TYPE       0x05
#define HDA_PARAM_WIDGET_CAPS   0x09
#define HDA_PARAM_PCM           0x0A
#define HDA_PARAM_PIN_CAPS      0x0C
#define HDA_PARAM_IN_AMP        0x0D
#define HDA_PARAM_CONN_LEN      0x0E
#define HDA_PARAM_GPIO          0x11
#define HDA_PARAM_OUT_AMP       0x12

#define HDA_FG_AUDIO            0x01

// widget caps
#define HDA_WCAP_TYPE(w)        (((w) >> 20) & 0xF)
#define HDA_WCAP_IN_AMP         (1u << 1)
#define HDA_WCAP_OUT_AMP        (1u << 2)
#define HDA_WCAP_AMP_OVERRIDE   (1u << 3)
#define HDA_WCAP_FMT_OVERRIDE   (1u << 4)
#define HDA_WCAP_BAD            (0xFu << 20) // a widget that wouldnt answer, type 0 would read as a dac
#define HDA_WCAP_CONN_LIST      (1u << 8)
#define HDA_WCAP_DIGITAL        (1u << 9)
#define HDA_WCAP_POWER          (1u << 10)

#define HDA_WIDGET_OUT          0x0
#define HDA_WIDGET_MIXER        0x2
#define HDA_WIDGET_SELECTOR     0x3
#define HDA_WIDGET_PIN          0x4

// pins
#define HDA_PINCAP_TRIGGER      (1u << 1)
#define HDA_PINCAP_DETECT       (1u << 2)
#define HDA_PINCAP_OUT          (1u << 4)
#define HDA_PINCAP_HDMI         (1u << 7)
#define HDA_PINCAP_EAPD         (1u << 16)
#define HDA_PINCAP_DP           (1u << 24)
#define HDA_PIN_CTL_OUT         0x40
#define HDA_PIN_CTL_HP          0x80
#define HDA_PIN_VREF_50         0x01
#define HDA_PIN_VREF_80         0x04
#define HDA_PIN_SENSE_PRESENT   (1u << 31)
#define HDA_EAPD_ON             (1u << 1)

#define HDA_CFG_PORT(c)         (((c) >> 30) & 0x3)
#define HDA_CFG_DEVICE(c)       (((c) >> 20) & 0xF)
#define HDA_CFG_LOCATION(c)     (((c) >> 24) & 0x3F)
#define HDA_CFG_COLOUR(c)       (((c) >> 12) & 0xF)
#define HDA_PORT_NONE           1
#define HDA_LOC_REAR            0x01
#define HDA_LOC_FRONT           0x02
#define HDA_DEV_LINE_OUT        0
#define HDA_DEV_SPEAKER         1
#define HDA_DEV_HP_OUT          2

// amps
#define HDA_AMP_OFFSET(a)       ((a) & 0x7F)
#define HDA_AMP_STEPS(a)        (((a) >> 8) & 0x7F)
#define HDA_AMP_STEP_SIZE(a)    (((a) >> 16) & 0x7F)
#define HDA_AMP_MUTE_CAP        (1u << 31)
#define HDA_AMP_SET_OUT         (1u << 15)
#define HDA_AMP_SET_IN          (1u << 14)
#define HDA_AMP_SET_LEFT        (1u << 13)
#define HDA_AMP_SET_RIGHT       (1u << 12)
#define HDA_AMP_SET_INDEX(i)    ((uint32_t)(i) << 8)
#define HDA_AMP_SET_MUTE        (1u << 7)

// stream format
#define HDA_PCM_RATE_MASK       0x7Fu       // the rates we can ask for
#define HDA_FMT_BASE_44K        (1u << 14)
#define HDA_FMT_MULT(m)         (((uint32_t)(m) - 1) << 11)
#define HDA_FMT_DIV(d)          (((uint32_t)(d) - 1) << 8)
#define HDA_FMT_16BIT           (1u << 4)
#define HDA_FMT_STEREO          1u
