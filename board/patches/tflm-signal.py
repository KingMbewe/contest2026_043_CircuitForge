#!/usr/bin/env python3
"""Compile TFLM's signal library into the vendored tflite-micro build.

WHY: the micro_speech audio frontend is a .tflite graph whose ops (Window,
FftAutoScale, Rfft, Energy, FilterBank, FilterBankSquareRoot,
FilterBankSpectralSubtraction, PCAN, FilterBankLog) are implemented in
tflite-micro/signal/, NOT tensorflow/lite/.  The vendored Makefile globs only
tensorflow/lite/..., so MicroMutableOpResolver::AddWindow() and friends link
as undefined references.

Deliberately NOT globbed:
  signal/micro/kernels/xtensa/  -- HiFi-DSP variants of fft_auto_scale_kernel
                                   and filter_bank_square_root; compiling them
                                   alongside the generic ones is a duplicate
                                   symbol.  A plain *.cc wildcard does not
                                   descend into it, which is the point.
  signal/tensorflow_core/       -- host-side TensorFlow custom ops; needs the
                                   full TF framework and cannot cross-compile.

Inserted BEFORE the existing `filter-out %test.cc` line so the signal tests
(delay_test.cc, fft_test.cc, ...) are dropped by the filter already there.

Writes <file>.new then os.replace().  Nothing is truncated in place.
"""

import os
import sys

HOME = os.path.expanduser("~")
TARGET = os.path.join(HOME, "openvela", "apps", "mlearning", "tflite-micro",
                      "Makefile")

ANCHOR = (
    "CXXSRCS += $(wildcard $(TFLM_DIR)/tensorflow/lite/schema/*.cc)\n"
    "CXXSRCS := $(filter-out %test.cc, $(CXXSRCS))\n"
)

ADDITION = (
    "CXXSRCS += $(wildcard $(TFLM_DIR)/tensorflow/lite/schema/*.cc)\n"
    "\n"
    "# TFLM signal library -- Window/FftAutoScale/Rfft/Energy/FilterBank*/PCAN.\n"
    "# Required by the micro_speech audio frontend graph (VelaPaw voice).\n"
    "# NOTE: *.cc does not descend into signal/micro/kernels/xtensa/, which is\n"
    "# intentional -- those are HiFi-DSP duplicates of two generic kernels.\n"
    "# signal/tensorflow_core/ is host TensorFlow and must stay out.\n"
    "CXXSRCS += $(wildcard $(TFLM_DIR)/signal/micro/kernels/*.cc)\n"
    "CXXSRCS += $(wildcard $(TFLM_DIR)/signal/src/*.cc)\n"
    "CXXSRCS += $(wildcard $(TFLM_DIR)/signal/src/kiss_fft_wrappers/*.cc)\n"
    "\n"
    "# CRITICAL -- the signal kernels each hold a function-local static.  Without\n"
    "# this, g++ emits __cxa_guard_acquire around them, and that call NEVER\n"
    "# RETURNS on this target: AllocateTensors() hangs inside the first frontend\n"
    "# kernel, no crash dump, no output past 'kws: init'.  Same flag and same\n"
    "# reason as apps/examples/velapaw/Makefile.  Codegen only, not header-\n"
    "# visible, so objects built with and without it link together fine.\n"
    "CXXFLAGS += -fno-threadsafe-statics\n"
    "\n"
    "CXXSRCS := $(filter-out %test.cc, $(CXXSRCS))\n"
)

MARKER = "signal/micro/kernels"


def main():
    if not os.path.exists(TARGET):
        print("REFUSE: no such file: %s" % TARGET)
        return 1

    with open(TARGET, "r", encoding="utf-8", errors="surrogateescape") as fh:
        src = fh.read()

    orig_len = len(src)
    print("read %s (%d bytes)" % (TARGET, orig_len))

    if orig_len < 1000:
        print("REFUSE: only %d bytes -- that is not the tflite-micro Makefile"
              % orig_len)
        return 1

    if MARKER in src:
        print("REFUSE: signal sources already present -- patch looks applied")
        return 1

    # The signal tree must actually exist, or the wildcards expand to nothing
    # and this "succeeds" while changing exactly nothing.
    sigdir = os.path.join(os.path.dirname(TARGET), "tflite-micro", "signal",
                          "micro", "kernels")
    if not os.path.isdir(sigdir):
        print("REFUSE: %s does not exist" % sigdir)
        return 1

    nkern = len([f for f in os.listdir(sigdir)
                 if f.endswith(".cc") and not f.endswith("_test.cc")])
    print("signal/micro/kernels: %d non-test .cc files" % nkern)
    if nkern < 10:
        print("REFUSE: expected ~20 kernel sources, found %d" % nkern)
        return 1

    n = src.count(ANCHOR)
    if n != 1:
        print("REFUSE: anchor matched %d times (need exactly 1)" % n)
        print("        the vendored Makefile is not the revision expected")
        return 1

    src = src.replace(ANCHOR, ADDITION, 1)

    if len(src) <= orig_len:
        print("REFUSE: file did not grow (%d -> %d)" % (orig_len, len(src)))
        return 1

    tmp = TARGET + ".new"
    with open(tmp, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(src)
    os.replace(tmp, TARGET)
    print("WROTE %s (%d -> %d bytes)" % (TARGET, orig_len, len(src)))
    print("OK -- signal library will now be compiled into libtflm")
    return 0


sys.exit(main())
