# VelaPaw - contest 2026 team 043
if(CONFIG_LVX_USE_DEMO_CONTEST2026_043_VELAPAW)

  set(VELAPAW_SRCS
      velapaw_main.c
      hal/feeder_mock.c
      infer/infer.c
      infer/infer_stub.c
      identity/identity.c
      store/store_stub.c)

  # Camera source: host-webcam TCP bridge, live V4L2, or embedded photos.
  if(CONFIG_VELAPAW_CAMERA_NET)
    list(APPEND VELAPAW_SRCS hal/camera_net.c)
  elseif(CONFIG_VELAPAW_CAMERA_V4L2)
    list(APPEND VELAPAW_SRCS hal/camera_v4l2.c)
  else()
    list(APPEND VELAPAW_SRCS hal/camera_mock.c)
  endif()

  set(VELAPAW_INCDIRS ${CMAKE_CURRENT_SOURCE_DIR})

  if(CONFIG_VELAPAW_INFER_TFLM)
    # Locate the tflite-micro source tree (apps/mlearning/tflite-micro/tflite-micro).
    # velapaw builds at apps/packages/demos/contest2026_043_velapaw, so up 3 -> apps.
    get_filename_component(
      VELAPAW_TFLM_ROOT
      ${CMAKE_CURRENT_LIST_DIR}/../../../mlearning/tflite-micro/tflite-micro ABSOLUTE)

    if(NOT EXISTS ${VELAPAW_TFLM_ROOT}/tensorflow/lite/micro/micro_interpreter.h)
      message(FATAL_ERROR "VelaPaw: cannot find tflite-micro at ${VELAPAW_TFLM_ROOT}")
    endif()

    set(VELAPAW_GENDIR ${CMAKE_CURRENT_BINARY_DIR}/genfiles)
    # Our trained model, committed into the app tree (host/export -> here).
    set(VELAPAW_MODEL ${CMAKE_CURRENT_LIST_DIR}/infer/model/velapaw.tflite)
    set(VELAPAW_GEN_SCRIPT
        ${VELAPAW_TFLM_ROOT}/tensorflow/lite/micro/tools/generate_cc_arrays.py)
    set(VELAPAW_MODEL_CC ${VELAPAW_GENDIR}/velapaw_model_data.cc)
    set(VELAPAW_MODEL_H ${VELAPAW_GENDIR}/velapaw_model_data.h)

    if(NOT EXISTS ${VELAPAW_MODEL})
      message(FATAL_ERROR "VelaPaw: model not found at ${VELAPAW_MODEL} "
                          "(copy host/export/accurate/accurate.tflite there)")
    endif()

    file(MAKE_DIRECTORY ${VELAPAW_GENDIR})
    add_custom_command(
      OUTPUT ${VELAPAW_MODEL_CC} ${VELAPAW_MODEL_H}
      COMMAND ${Python3_EXECUTABLE} ${VELAPAW_GEN_SCRIPT} ${VELAPAW_GENDIR}
              ${VELAPAW_MODEL}
      DEPENDS ${VELAPAW_MODEL}
      COMMENT "VelaPaw: generating model data from velapaw.tflite")

    # Embedded real pet photos for the emulator demo (host/gen_device_assets.py).
    list(APPEND VELAPAW_SRCS infer/backend_tflm.cc ${VELAPAW_MODEL_CC}
         assets/velapaw_assets.c)
    list(APPEND VELAPAW_INCDIRS ${VELAPAW_GENDIR} ${CMAKE_CURRENT_SOURCE_DIR}/assets)

    # BCS (body condition score) model - second, standalone on-device network.
    set(VELAPAW_BCS_MODEL ${CMAKE_CURRENT_LIST_DIR}/infer/model/bcs.tflite)
    if(EXISTS ${VELAPAW_BCS_MODEL})
      add_custom_command(
        OUTPUT ${VELAPAW_GENDIR}/bcs_model_data.cc
               ${VELAPAW_GENDIR}/bcs_model_data.h
        COMMAND ${Python3_EXECUTABLE} ${VELAPAW_GEN_SCRIPT} ${VELAPAW_GENDIR}
                ${VELAPAW_BCS_MODEL}
        DEPENDS ${VELAPAW_BCS_MODEL}
        COMMENT "VelaPaw: generating BCS model data from bcs.tflite")
      list(APPEND VELAPAW_SRCS ${VELAPAW_GENDIR}/bcs_model_data.cc)
    endif()
  endif()

  set(VELAPAW_STACK 8192)
  if(CONFIG_VELAPAW_UI)
    list(APPEND VELAPAW_SRCS ui/ui_lvgl.c ui/ui_i18n.c)
    if(CONFIG_VELAPAW_FONT_CJK)
      list(APPEND VELAPAW_SRCS ui/velapaw_font_cjk_14.c ui/velapaw_font_cjk_16.c
           ui/velapaw_font_cjk_20.c)
    endif()
    set(VELAPAW_STACK 32768)   # LVGL needs a large stack
  endif()

  nuttx_add_application(
    NAME velapaw
    STACKSIZE ${VELAPAW_STACK}
    SRCS ${VELAPAW_SRCS}
    INCLUDE_DIRECTORIES ${VELAPAW_INCDIRS})

  if(CONFIG_VELAPAW_INFER_TFLM)
    # Link TFLite-Micro and inherit its PUBLIC include dirs (TFLM/gemmlowp/ruy/
    # flatbuffers) per the openVela CMake guide.
    nuttx_add_dependencies(TARGET apps_velapaw DEPENDS tflite_micro)
    target_compile_definitions(apps_velapaw PRIVATE TF_LITE_STATIC_MEMORY)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/infer/model/bcs.tflite)
      target_compile_definitions(apps_velapaw PRIVATE VELAPAW_HAVE_BCS)
    endif()
  endif()

  if(CONFIG_VELAPAW_UI)
    # Link LVGL and inherit its PUBLIC include dirs (lvgl/lvgl.h + nuttx driver).
    nuttx_add_dependencies(TARGET apps_velapaw DEPENDS lvgl)
  endif()

endif()
