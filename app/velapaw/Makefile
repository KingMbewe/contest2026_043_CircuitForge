############################################################################
# VelaPaw - contest 2026 team 043 - Makefile
#
# This is the build used on the real ESP32-S3 board (apps/examples/velapaw).
# CMakeLists.txt mirrors it for the CMake/emulator path; keep the two in sync.
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = velapaw
PRIORITY  = SCHED_PRIORITY_DEFAULT
# LVGL needs a large stack -- and the UI task ALSO runs a TFLM Invoke inline on
# the Enroll "Capture" button (recognition gets its own 32K worker thread, but
# capture does not). 32768 has to cover LVGL *plus* inference; that is too tight
# and wedged the board on capture. Keep this well clear of both.
#
# Reduced 131072 -> 49152 on 2026-08-26 for the ai_agent merge: every task and
# pthread stack is carved from the XTENSA_IMEM separate heap
# (CONFIG_XTENSA_IMEM_REGION_SIZE, now 0x48000 = 294912 B), and 131072 here left
# too little for ai_agent's own task + 6 pthreads -- pthread_create returned
# ENOMEM (r=12). Must stay in step with VELAPAW_AUTOSTART_STACK in
# boards/xtensa/esp32s3/esp32s3-devkit/src/esp32s3_appinit.c.
STACKSIZE = 49152
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_043_VELAPAW)

VELAPAW_DIR  = $(APPDIR)/examples/velapaw
VELAPAW_TFLM = $(APPDIR)/mlearning/tflite-micro/tflite-micro

# Module headers (hal/, infer/, identity/, store/) resolve from the app root;
# backend_tflm.cc includes the generated model header flat, so gen/ too.
COMMON_INC  = -I$(VELAPAW_DIR)
COMMON_INC += -I$(VELAPAW_DIR)/infer/gen
COMMON_INC += -I$(VELAPAW_DIR)/assets
COMMON_INC += -I$(VELAPAW_DIR)/voice/gen

# TFLite-Micro + its third-party headers (mirrors apps/mlearning/tflite-micro).
TFLM_INC  = -I$(VELAPAW_TFLM)
TFLM_INC += -I$(VELAPAW_TFLM)/third_party/flatbuffers/include
TFLM_INC += -I$(APPDIR)/math/gemmlowp/gemmlowp
TFLM_INC += -I$(APPDIR)/math/kissfft/kissfft
TFLM_INC += -I$(APPDIR)/math/ruy/ruy

CFLAGS   += $(COMMON_INC) -DVELAPAW_HAVE_BCS

CXXFLAGS += $(COMMON_INC) $(TFLM_INC)
CXXFLAGS += -DVELAPAW_HAVE_BCS

# These MUST be exactly the flags apps/mlearning/tflite-micro built the library
# with -- they are header-visible, and the app and the library share headers.
#
# DO NOT ADD -DTF_LITE_STATIC_MEMORY HERE. The library does not define it, and
# it CHANGES THE LAYOUT of TfLiteTensor in core/c/common.h. Define it on only
# one side and the app reads the library's tensors at the wrong field offsets:
# AllocateTensors still succeeds (all inside the library), then the first Invoke
# writes through a garbage pointer and WEDGES THE SoC -- dead console, no crash
# dump, any thread, any tab. Cost an evening. (CMakeLists.txt does define it,
# because the CMake tflite_micro target compiles the library with it too --
# that's why copying flags from CMake into this Makefile is wrong.)
#
# The debug/strip trio is an if/else in the library's Makefile: it has EITHER
# SHOW_MEMORY_USE+USE_CTIME (CONFIG_TFLITEMICRO_DEBUG=y) OR STRIP_ERROR_STRINGS.
# Ours is the strip branch -- that's why MicroPrintf() is absent at link time.
CXXFLAGS += -DTFLITE_EMULATE_FLOAT
CXXFLAGS += -DTF_LITE_DISABLE_X86_NEON
CXXFLAGS += -DTF_LITE_USE_GLOBAL_CMATH_FUNCTIONS
CXXFLAGS += -DTF_LITE_STRIP_ERROR_STRINGS
# ESP-NN kernels (the library auto-enables these when apps/mlearning/esp-nn is
# present); keep the app symmetric with it.
CXXFLAGS += -DESP_NN=1 -DCONFIG_NN_OPTIMIZED -mlongcalls
CXXFLAGS += -include $(APPDIR)/mlearning/tflite-micro/float_shim.h
CXXFLAGS += -fpermissive
# CRITICAL: without this the first C++ function-local static in tflm_init
# (the MicroMutableOpResolver) deadlocks in __cxa_guard_acquire -> silent hang,
# LVGL starved -> solid blue screen. Do not remove.
CXXFLAGS += -fno-threadsafe-statics

MAINSRC = velapaw_main.c

CSRCS   = ui/ui_lvgl.c ui/ui_i18n.c \
          assets/velapaw_assets.c \
          hal/camera_ov5640.c hal/feeder_servo.c \
          infer/infer.c infer/infer_stub.c \
          identity/identity.c store/store_stub.c

# Chinese (CJK) UI font — generated subset blobs, only compiled when enabled.
ifeq ($(CONFIG_VELAPAW_FONT_CJK),y)
CSRCS  += ui/velapaw_font_cjk_14.c ui/velapaw_font_cjk_16.c ui/velapaw_font_cjk_20.c
endif

# Real on-device INT8 inference. The 1.4MB model is NOT embedded as rodata (the
# DROM0 MMU window can't map it) -- it is flashed to 0x600000 and read at run
# time; velapaw_model_data.cc is pre-generated and committed under infer/gen.
# Application.mk's C++ pattern rule keys off CXXEXT (default .cxx); our sources
# are .cc, so without this they "don't match the target pattern" and g++ is
# invoked with no input file.
CXXEXT  = .cc
CXXSRCS = infer/backend_tflm.cc infer/gen/velapaw_model_data.cc

# Keyword spotting (Phase 1.5). Both micro_speech models ARE embedded as
# rodata -- 8,772 + 18,800 B fits the DROM0 window that the 1.4 MB pet model
# does not.
#
# The nine frontend ops (Window/Rfft/FilterBank*/PCAN/...) live in
# tflite-micro/signal/, which the vendored library Makefile does not compile.
# They are added there, NOT here, by board/patches/tflm-signal.py -- compiling
# library sources with app flags is exactly the header-visible mismatch the
# TF_LITE_STATIC_MEMORY comment above warns about.
ifeq ($(CONFIG_VELAPAW_VOICE),y)
CSRCS   += voice/mic.c
CXXSRCS += voice/kws.cc
CXXSRCS += voice/gen/micro_speech_quantized_model_data.cc
CXXSRCS += voice/gen/audio_preprocessor_int8_model_data.cc

# Test vectors: ~99 KB of rodata, build-time only. Leave the self-test off in
# the release build.
ifeq ($(CONFIG_VELAPAW_KWS_SELFTEST),y)
CXXSRCS += voice/gen/yes_30ms_audio_data.cc
CXXSRCS += voice/gen/no_30ms_audio_data.cc
CXXSRCS += voice/gen/yes_1000ms_audio_data.cc
CXXSRCS += voice/gen/no_1000ms_audio_data.cc
CXXSRCS += voice/gen/silence_1000ms_audio_data.cc
endif
endif

include $(APPDIR)/Application.mk
