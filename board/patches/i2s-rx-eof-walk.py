#!/usr/bin/env python3
"""VelaPaw patch #31: fix the RX EOF descriptor walk in i2s_rx_schedule().

This is the other half of patch #10.

The upstream driver carries the same "find the last descriptor" walk in three
places -- i2s_tx_schedule(), i2s_rx_schedule() and i2s_rx_worker().  The
hardware sets suc_eof on the LAST descriptor of a chain, so the walk has to
advance until it finds the descriptor CARRYING EOF.  i2s_tx_schedule() does
that correctly.  Patch #10 fixed i2s_rx_worker(), which had been exiting on
d[0] and reporting nbytes == 0.  i2s_rx_schedule() was missed, and still
advances only while the NEXT link has EOF:

    while (bfdesc->next != NULL && (bfdesc->next->ctrl & ..._EOF))

On a multi-descriptor transfer that stops on d[0], compares it against an
inlink pointing at d[n], and falls through the `if (bfdesc == inlink)` below
-- which has NO else.  The container is left in rx.act, and i2s_rxdma_start()
returns early on a non-empty rx.act ("already active"), so RX never restarts.
Every later buffer strands in rx.pend.

Observed signature (VelaPaw builds 66/68/70/71, intermittent across boots):

    mic fail rec 1/19   play 1/1
    mic fail got 0/32000  phase -1
    mic stage 34 (cl-stop-rec)   <- AUDIOIOC_STOP hangs

The stop hangs because es8311's worker thread cannot terminate while it still
holds the stranded buffers, so the join inside AUDIOIOC_STOP never returns and
g_mic_busy is never cleared -- one stall disables the microphone until the
next cold power cycle.

Two changes:

  1. The walk is corrected to match i2s_tx_schedule() and patch #10.
  2. The missing else is supplied.  Upstream flagged this exact case in
     i2s_tx_schedule() ("REVISIT: what to do if we miss syncronization...")
     and never answered it.  On RX the answer matters: doing nothing costs
     the entire stream until a power cycle, while completing the head
     container anyway costs at worst one buffer of wrong length.

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
EXPECT_MAX = 125000

# (substring, required count)
MARKERS = [
    ("VelaPaw patch #10", 1),
    ("i2s_rx_schedule", 4),          # prototype, definition, 2 call sites
    ("VelaPaw patch #31", 0),        # not already applied
    ("g_i2s_rx_desync", 0),
]

OLD_WALK = """      /* Find the last descriptor of the current buffer container */

      bfdesc = bfcontainer->dma_link;

      while (bfdesc->next != NULL &&
             (bfdesc->next->ctrl & ESP32S3_DMA_CTRL_EOF))
        {
          bfdesc = bfdesc->next;
        }

      if (bfdesc == inlink)
        {
          sq_remfirst(&priv->rx.act);

          /* Report the result of the transfer */

          bfcontainer->result = OK;

          /* Add the completed buffer container to the tail of the rx.done
           * queue
           */

          sq_addlast((sq_entry_t *)bfcontainer, &priv->rx.done);

          /* Check if the DMA is IDLE */

          if (sq_empty(&priv->rx.act))
            {
              /* Then start the next DMA. */

              i2s_rxdma_start(priv);
            }
        }
"""

NEW_WALK = """      /* Find the descriptor that CARRIES EOF.
       *
       * VelaPaw patch #31: this walk used to advance only while the NEXT
       * link had EOF, which is the same mistake patch #10 fixed over in
       * i2s_rx_worker().  The hardware sets suc_eof on the LAST descriptor
       * of a chain, so on a multi-descriptor transfer the old form stopped
       * on d[0], compared it against an inlink pointing at d[n], and fell
       * through the match test below -- which had no else.  The container
       * stayed in rx.act, i2s_rxdma_start() bails on a non-empty rx.act,
       * and RX never restarted: one buffer delivered, every later one
       * stranded in rx.pend.  i2s_tx_schedule() already walks correctly;
       * this brings RX into line with it.
       *
       * The NULL guard replaces TX's DEBUGASSERT deliberately.  This runs
       * in interrupt context, where a fault is unrecoverable and cannot
       * even be reported -- syslog from an ISR faults on this part.
       */

      bfdesc = bfcontainer->dma_link;

      while (!(bfdesc->ctrl & ESP32S3_DMA_CTRL_EOF) && bfdesc->next != NULL)
        {
          bfdesc = bfdesc->next;
        }

      /* Answering the REVISIT left in i2s_tx_schedule(): if the descriptor
       * that raised the interrupt is not the expected one, completing the
       * head container anyway costs at worst a single buffer of wrong
       * length, while doing nothing costs the whole RX stream until the
       * next power cycle.  Recover instead of wedging.
       *
       * Counted, not logged -- see the ISR note above.  g_i2s_rx_desync is
       * readable from a debugger or a later probe build; if it is still 0
       * after a long capture session then the walk fix alone was enough.
       */

      if (bfdesc != inlink)
        {
          g_i2s_rx_desync++;
        }

      sq_remfirst(&priv->rx.act);

      /* Report the result of the transfer */

      bfcontainer->result = OK;

      /* Add the completed buffer container to the tail of the rx.done
       * queue
       */

      sq_addlast((sq_entry_t *)bfcontainer, &priv->rx.done);

      /* Check if the DMA is IDLE */

      if (sq_empty(&priv->rx.act))
        {
          /* Then start the next DMA. */

          i2s_rxdma_start(priv);
        }
"""

OLD_DEF = """static void i2s_rx_schedule(struct esp32s3_i2s_s *priv,
                            struct esp32s3_dmadesc_s *inlink)
{
"""

NEW_DEF = """/* VelaPaw patch #31: count of RX EOF interrupts whose descriptor did not
 * match the head of rx.act.  Expected to stay 0 once the walk above is
 * correct; a nonzero value means the recovery branch is carrying the stream
 * and the descriptor chain deserves another look.
 */

static uint32_t g_i2s_rx_desync;

static void i2s_rx_schedule(struct esp32s3_i2s_s *priv,
                            struct esp32s3_dmadesc_s *inlink)
{
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

    for name, old in (("rx walk", OLD_WALK), ("rx_schedule def", OLD_DEF)):
        got = src.count(old)
        if got != 1:
            sys.exit("ABORT: %s anchor found %d times, expected 1"
                     % (name, got))

    src = src.replace(OLD_WALK, NEW_WALK, 1)
    src = src.replace(OLD_DEF, NEW_DEF, 1)

    tmp = PATH + ".new"
    with io.open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(src)
    os.replace(tmp, PATH)

    print("patch #31 applied to %s (%d bytes)" % (PATH, len(src)))
    print("rebuild: cd ~/openvela/nuttx && make -j8")


if __name__ == "__main__":
    main()
