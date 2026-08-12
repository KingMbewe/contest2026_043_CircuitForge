#!/usr/bin/env python3
"""VelaPaw patch #32: align the RX EOF byte count to the DMA descriptor grain.

This finishes the job patch #9 started.

i2s_receive() sizes the RX transfer like this:

    nbytes = apb->nmaxbytes;
    nbytes -= (nbytes % (priv->data_width / 8));
    nbytes = MIN(nbytes, ESP32S3_DMA_BUFLEN_MAX);
    nbytes &= ~UINT32_C(3);              <- patch #9, 4-byte align -> 4092

That value does two different jobs.  It is written to I2S_RX_EOF_NUM, which
tells the I2S peripheral how many bytes to receive before raising EOF, AND it
is handed to esp32s3_dma_setup_link() to build the descriptor chain.  The two
consumers do not agree on granularity: setup_link builds descriptors on a
16-byte grain, so a 4092-byte request becomes a TWO descriptor chain of
4080 + 16 = 4096 bytes, while EOF stays programmed at 4092.

The EOF boundary then falls four bytes inside the second descriptor and never
coincides with a descriptor boundary.  The GDMA closes descriptors only at
boundaries, so the EOF is missed, nothing is ever completed, the input FIFO
backs up and overflows, and RX hangs.

Confirmed on hardware (VelaPaw build 73) with the VELAPAW_I2S_PROBES dump:

    VP: RXDUMP rxconf=0008962c txconf=08089204 eofnum=00000ffc
    VP: RXDUMP raw=000000a0 ...          <- bit5 INFIFO_OVF, bit1 SUC_EOF clear
    VP: RXDUMP eofdes=00000000 dscr=00000000 act=1 pend=1 done=0
    VP: RXDUMP i2sraw=0000000f ...       <- bit0 RX_DONE, bit2 RX_HUNG
    VP: RXDUMP d[0] ctrl=80000ff0 buf=0x3c510688 next=0x3fccc494
    VP: RXDUMP d[1] ctrl=80000010 buf=0x3c511678 next=0

RX_DONE proves the peripheral was clocking and receiving; INFIFO_OVF with
SUC_EOF clear and eofdes == 0 proves not one descriptor ever completed.

Aligning to 16 instead of 4 yields 4080, which fits in a SINGLE descriptor.
EOF and the end of the chain then coincide exactly and the multi-descriptor
EOF case -- the same case patches #10 and #31 were both written for -- stops
arising on this path at all.

Cost: 4080 bytes per delivery instead of 4092, i.e. 2040 int16 and 1020
usable samples after MIC_STRIDE 2 rather than 1023.  A 16000-sample window
needs 16 deliveries either way, and MIC_WANT_BUFS is 18, so no app change is
required.

Writes <file>.new and os.replace()s it, so a failed patch leaves the original
untouched.  Asserts the prior-patch markers first: this file has been patched
many times and applying to the wrong revision is worse than not applying.
"""

import io
import os
import sys

PATH = os.path.expanduser(
    "~/openvela/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c")

EXPECT_MIN = 95000
EXPECT_MAX = 130000

# (substring, required count)
MARKERS = [
    ("VelaPaw patch #9", 1),
    ("VelaPaw patch #31", 2),         # #31 must already be applied (2 blocks)
    ("VelaPaw patch #32", 0),         # not already applied
]

OLD = """      /* VelaPaw patch #9: the alignment above happens BEFORE the clamp, so
       * clamping 8192 to ESP32S3_DMA_BUFLEN_MAX yields 4095 -- an odd byte
       * count that lands in I2S_RX_EOF_NUM and can never be reached by
       * whole 16-bit samples.  Re-align after clamping (-> 4092).
       */

      nbytes &= ~UINT32_C(3);
"""

NEW = """      /* VelaPaw patch #9: the alignment above happens BEFORE the clamp, so
       * clamping 8192 to ESP32S3_DMA_BUFLEN_MAX yields 4095 -- an odd byte
       * count that lands in I2S_RX_EOF_NUM and can never be reached by
       * whole 16-bit samples.  Re-align after clamping.
       *
       * VelaPaw patch #32: align to 16, not 4.
       *
       * This number is used twice and the two users disagree on grain.  It
       * is written to I2S_RX_EOF_NUM, where it means "raise EOF after this
       * many bytes", and it is handed to esp32s3_dma_setup_link(), which
       * builds descriptors on a 16-byte grain.  At 4092 the chain comes out
       * as 4080 + 16 = 4096 across TWO descriptors while EOF stays at 4092,
       * so the EOF boundary lands four bytes inside d[1] and never
       * coincides with a descriptor boundary.  The GDMA closes descriptors
       * only at boundaries, so the EOF is missed entirely: nothing ever
       * completes, the input FIFO overflows (INFIFO_OVF) and RX hangs
       * (I2S RX_HUNG) with the peripheral still happily receiving.
       *
       * 4080 fits in one descriptor, so EOF and the end of the chain
       * coincide and the multi-descriptor EOF case stops arising here at
       * all -- the same case patches #10 and #31 were both written for.
       *
       * Costs 12 bytes per delivery (1020 usable samples after stride
       * instead of 1023).  A 16000-sample window still needs 16
       * deliveries, so nothing downstream changes.
       */

      nbytes &= ~UINT32_C(15);
"""


def main():
    if not os.path.exists(PATH):
        sys.exit("ABORT: %s does not exist" % PATH)

    with io.open(PATH, "r", encoding="utf-8", newline="") as f:
        src = f.read()

    if not EXPECT_MIN <= len(src) <= EXPECT_MAX:
        sys.exit("ABORT: %s is %d bytes, expected %d..%d -- wrong revision?"
                 % (PATH, len(src), EXPECT_MIN, EXPECT_MAX))

    for text, want in MARKERS:
        got = src.count(text)
        if got != want:
            sys.exit("ABORT: marker %r found %d times, expected %d"
                     % (text, got, want))

    if src.count(OLD) != 1:
        sys.exit("ABORT: patch #9 anchor found %d times, expected 1"
                 % src.count(OLD))

    src = src.replace(OLD, NEW, 1)

    tmp = PATH + ".new"
    with io.open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(src)
    os.replace(tmp, PATH)

    print("patch #32 applied to %s (%d bytes)" % (PATH, len(src)))
    print("rebuild: cd ~/openvela/nuttx && make -j8")


if __name__ == "__main__":
    main()
