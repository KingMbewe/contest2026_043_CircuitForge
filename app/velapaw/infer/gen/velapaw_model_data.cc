/****************************************************************************
 * apps/examples/velapaw/infer/gen/velapaw_model_data.cc
 *
 * Placeholder for the identity model blob.
 *
 * The 1.4 MB model in infer/model/velapaw.tflite is deliberately NOT embedded
 * as rodata: the DROM0 MMU window bus-stalls past roughly 256 KB.  It is
 * flashed to raw offset 0x600000 and pulled into PSRAM at run time by
 * velapaw_load_model_from_flash() (infer/backend_tflm.cc), and the BCS model
 * the same way from 0x760000.
 *
 * Nothing in the tree references g_velapaw_model_data -- backend_tflm.cc only
 * includes the header.  The proof is the image size: build 41's entire
 * nuttx.bin was 1,018,296 B, which cannot contain a 1,436,344 B array.  So a
 * one-byte definition here is behaviourally identical to whatever the original
 * generated file held, and it lets the app build from a clean checkout.
 *
 * To genuinely embed a model, regenerate with:
 *   xxd -i -n g_velapaw_model_data infer/model/velapaw.tflite
 ****************************************************************************/

#include "velapaw_model_data.h"

extern "C"
{
  const unsigned char g_velapaw_model_data[1] =
  {
    0x00
  };

  const unsigned int g_velapaw_model_data_len = 0;
}
