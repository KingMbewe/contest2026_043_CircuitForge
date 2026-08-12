#!/usr/bin/env python3
"""Gate the VelaPaw bring-up probes in esp32s3_i2s.c behind VELAPAW_I2S_PROBES.

Does NOT delete them.  Every one of these probes identified a real bug -- the
RX TDM slot map, the fixed-index start transient, the pin map -- and Phase 2-4
will want them back.  They just cannot stay live once VelaPaw listens
continuously from the UI task: the RX probe fires every 8th buffer, which is
about once a second forever, and syslog at LOG_ERR goes to the console.

Set VELAPAW_I2S_PROBES to 1 and rebuild to get them all back.

Writes <file>.new and os.replace()s it, so a failed patch leaves the original
untouched.  Asserts the prior-patch markers first: this file has been patched
many times and applying to the wrong revision is worse than not applying.
"""

import os
import sys

PATH = os.path.expanduser("~/openvela/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c")

EXPECT_MIN = 100000
EXPECT_MAX = 115000

# (description, must-be-present-exactly-N-times)
MARKERS = [
    ("VelaPaw patch #19", 1),
    ("VelaPaw patch #30", 1),
    ("VP: RXDUMP rxconf=", 1),
    ("VP: mic n=", 1),
    ("VP: i2s pins din=", 1),
    ("#define VELAPAW_I2S_PROBES", 0),   # not already applied
]

GATE_DEF = """/* VelaPaw bring-up probes.
 *
 * 0 = silent (shipping).  1 = the RX DMA state dump, the per-buffer mic
 * meter and the pin map, all via syslog(LOG_ERR).
 *
 * Kept rather than deleted: these found the RX TDM slot map bug and the
 * fixed-index start-of-stream transient.  They cannot be left on because the
 * mic meter fires every 8th RX buffer -- roughly once a second, forever, once
 * VelaPaw listens continuously from the UI task.
 *
 * NOTE all three run in thread context (HPWORK or init).  syslog() from the
 * I2S ISR faults on this board with EXCCAUSE=0x14 -- do not move them.
 */

#define VELAPAW_I2S_PROBES 0

/* I2S DMA RX/TX description number */
"""

GATE_ANCHOR = "/* I2S DMA RX/TX description number */\n"

# Each edit is (anchor_text, replacement_text).  Anchors are matched exactly
# and must be unique.
EDITS = []

EDITS.append((
    "  /* VelaPaw patch #19: one-shot RX DMA state dump, taken the first time\n",
    "#if VELAPAW_I2S_PROBES\n"
    "  /* VelaPaw patch #19: one-shot RX DMA state dump, taken the first time\n",
))

# Close the #if after the RXDUMP block: the block is the last thing in
# i2s_tx_worker, so the close goes just before the function's closing brace.
EDITS.append((
    "                  vpd = vpd->next;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "}\n"
    "#endif /* I2S_HAVE_TX */\n",
    "                  vpd = vpd->next;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "#endif /* VELAPAW_I2S_PROBES */\n"
    "}\n"
    "#endif /* I2S_HAVE_TX */\n",
))

EDITS.append((
    "      /* VelaPaw patch #30: count railed samples and report DC offset, to\n",
    "#if VELAPAW_I2S_PROBES\n"
    "      /* VelaPaw patch #30: count railed samples and report DC offset, to\n",
))

EDITS.append((
    "                   vn ? (int32_t)(vdc / (int64_t)vn) : 0, vrail, vm8);\n"
    "          }\n"
    "      }\n",
    "                   vn ? (int32_t)(vdc / (int64_t)vn) : 0, vrail, vm8);\n"
    "          }\n"
    "      }\n"
    "#endif /* VELAPAW_I2S_PROBES */\n",
))

EDITS.append((
    "  syslog(LOG_ERR, \"VP: i2s pins din=%d dout=%d bclk=%d ws=%d mclk=%d \"\n"
    "                  \"role=%d tx_en=%d rx_en=%d\\n\",\n"
    "         priv->config->din_pin, priv->config->dout_pin,\n"
    "         priv->config->bclk_pin, priv->config->ws_pin,\n"
    "         priv->config->mclk_pin, priv->config->role,\n"
    "         priv->config->tx_en, priv->config->rx_en);\n",
    "#if VELAPAW_I2S_PROBES\n"
    "  syslog(LOG_ERR, \"VP: i2s pins din=%d dout=%d bclk=%d ws=%d mclk=%d \"\n"
    "                  \"role=%d tx_en=%d rx_en=%d\\n\",\n"
    "         priv->config->din_pin, priv->config->dout_pin,\n"
    "         priv->config->bclk_pin, priv->config->ws_pin,\n"
    "         priv->config->mclk_pin, priv->config->role,\n"
    "         priv->config->tx_en, priv->config->rx_en);\n"
    "#endif /* VELAPAW_I2S_PROBES */\n",
))


def fail(msg):
    print("REFUSED: " + msg)
    sys.exit(1)


def main():
    global PATH

    if len(sys.argv) > 1:            # override, used to rehearse on a copy
        PATH = sys.argv[1]

    if not os.path.isfile(PATH):
        fail("no such file: " + PATH)

    with open(PATH, "r", encoding="utf-8", newline="") as f:
        src = f.read()

    n = len(src)
    if not (EXPECT_MIN <= n <= EXPECT_MAX):
        fail("file is %d bytes, expected %d..%d -- wrong revision?"
             % (n, EXPECT_MIN, EXPECT_MAX))

    for text, want in MARKERS:
        got = src.count(text)
        if got != want:
            fail("marker %r appears %d times, expected %d"
                 % (text, got, want))

    if src.count(GATE_ANCHOR) != 1:
        fail("gate anchor is not unique")

    out = src.replace(GATE_ANCHOR, GATE_DEF, 1)

    for i, (anchor, repl) in enumerate(EDITS):
        c = out.count(anchor)
        if c != 1:
            fail("edit %d: anchor appears %d times, expected 1" % (i, c))
        out = out.replace(anchor, repl, 1)

    # Sanity: the gates must balance.
    if out.count("#if VELAPAW_I2S_PROBES") != 3:
        fail("expected 3 #if gates, got %d"
             % out.count("#if VELAPAW_I2S_PROBES"))
    if out.count("#endif /* VELAPAW_I2S_PROBES */") != 3:
        fail("expected 3 #endif gates, got %d"
             % out.count("#endif /* VELAPAW_I2S_PROBES */"))

    tmp = PATH + ".new"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(out)

    os.replace(tmp, PATH)
    print("OK: %s  %d -> %d bytes, 3 probe blocks gated" % (PATH, n, len(out)))


if __name__ == "__main__":
    main()
