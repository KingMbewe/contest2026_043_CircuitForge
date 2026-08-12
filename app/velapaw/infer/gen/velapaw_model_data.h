/****************************************************************************
 * apps/examples/velapaw/infer/gen/velapaw_model_data.h
 *
 * Declaration for the identity model blob.
 *
 * NOTE: the real 1.4 MB MobileNetV3 model is NOT embedded here.  It is
 * written to raw flash at offset 0x600000 and read into PSRAM at run time by
 * velapaw_load_model_from_flash() in infer/backend_tflm.cc -- the DROM0 MMU
 * window bus-stalls past ~256 KB, so it cannot be mapped as rodata.
 *
 * This header exists because backend_tflm.cc includes it; nothing references
 * the array itself.  See velapaw_model_data.cc.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPAW_INFER_GEN_VELAPAW_MODEL_DATA_H
#define __APPS_EXAMPLES_VELAPAW_INFER_GEN_VELAPAW_MODEL_DATA_H

#ifdef __cplusplus
extern "C"
{
#endif

extern const unsigned char g_velapaw_model_data[];
extern const unsigned int  g_velapaw_model_data_len;

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_VELAPAW_INFER_GEN_VELAPAW_MODEL_DATA_H */
