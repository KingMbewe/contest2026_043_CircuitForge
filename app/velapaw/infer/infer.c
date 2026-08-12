/****************************************************************************
 * VelaPaw - inference backend selection
 *
 * Picks the active backend at build time: TFLite-Micro when
 * CONFIG_VELAPAW_INFER_TFLM is set, otherwise the deterministic stub (which
 * keeps the recognition demo working without a trained model).
 ****************************************************************************/

#include <nuttx/config.h>
#include "infer/infer.h"

const struct velapaw_infer_backend *velapaw_infer_active(void)
{
#ifdef CONFIG_VELAPAW_INFER_TFLM
  return velapaw_infer_tflm_backend();
#else
  return velapaw_infer_stub_backend();
#endif
}
