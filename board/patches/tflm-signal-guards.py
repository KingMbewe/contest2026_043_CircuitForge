#!/usr/bin/env python3
"""Build the TFLM signal library with -fno-threadsafe-statics.

WHY: every kernel in signal/micro/kernels has a function-local static.  Without
this flag g++ emits a real __cxa_guard_acquire around each one, and on this
target that call NEVER RETURNS -- the same silent deadlock that produced the
solid blue screen when the app itself was first built without the flag.

The failure looks like this and gives you nothing else to go on:

    nsh> velapaw kws
    kws: init (frontend 32768 B + classifier 32768 B arenas)
    <hangs forever, no crash dump, no further output>

because MicroInterpreter::AllocateTensors() runs every kernel's Init(), and the
first guarded static in Window/Rfft/FilterBank/PCAN/... blocks there.

Confirm before and after with:

    find apps/mlearning/tflite-micro/tflite-micro/signal -name '*.o' \\
      | while read f; do xtensa-esp32s3-elf-nm "$f" 2>/dev/null \\
          | grep -q cxa_guard && echo "GUARD: $f"; done

Before the fix that lists ~28 objects; after a rebuild it must list none.

This is a SECOND patch, applied after tflm-signal.py.  A fresh checkout needs
only tflm-signal.py, which now adds the flag itself -- this script exists for
trees where tflm-signal.py was applied before the flag was folded in, and it
refuses to run if the flag is already there.

The flag is not header-visible: it changes only the codegen of function-local
statics, emits no guard variable, and alters no type layout.  Objects built
with and without it link together correctly, so only signal/ is rebuilt.  (That
is the opposite of TF_LITE_STATIC_MEMORY, which DOES change TfLiteTensor's
layout and must never differ between the app and the library.)

Writes <file>.new then os.replace().  Nothing is truncated in place.
"""

import os
import sys

HOME = os.path.expanduser("~")
TFLM = os.path.join(HOME, "openvela", "apps", "mlearning", "tflite-micro")
TARGET = os.path.join(TFLM, "Makefile")
SIGNAL = os.path.join(TFLM, "tflite-micro", "signal")

# Guaranteed present exactly once, and guaranteed to come after every flag
# assignment.  Anchoring here rather than on a specific CXXFLAGS line, because
# the vendored Makefile is not the same revision in every checkout.
ANCHOR = "include $(APPDIR)/Application.mk"

FLAG = "-fno-threadsafe-statics"

ADDITION = """\
# CRITICAL -- signal/micro/kernels each hold a function-local static.  Without
# this, g++ emits __cxa_guard_acquire around them, and that call never returns
# on this target: AllocateTensors() hangs inside the first frontend kernel with
# no crash dump.  Same flag, same reason, as apps/examples/velapaw/Makefile.
# Not header-visible -- codegen only -- so mixing it with objects built without
# it is safe.
CXXFLAGS += -fno-threadsafe-statics

"""


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

    # Ordering matters: this patch is meaningless if signal/ is not compiled.
    if "signal/micro/kernels" not in src:
        print("REFUSE: signal sources absent -- run tflm-signal.py first")
        return 1

    if FLAG in src:
        print("REFUSE: %s already present -- patch looks applied" % FLAG)
        return 1

    n = src.count(ANCHOR)
    if n != 1:
        print("REFUSE: anchor %r matched %d times (need exactly 1)"
              % (ANCHOR, n))
        return 1

    src = src.replace(ANCHOR, ADDITION + ANCHOR, 1)

    if len(src) <= orig_len:
        print("REFUSE: file did not grow (%d -> %d)" % (orig_len, len(src)))
        return 1

    tmp = TARGET + ".new"
    with open(tmp, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(src)
    os.replace(tmp, TARGET)
    print("WROTE %s (%d -> %d bytes)" % (TARGET, orig_len, len(src)))

    # A flag change does not invalidate a .o by timestamp, so without this the
    # rebuild is a no-op and the "fix" silently does nothing.  Only signal/ is
    # cleaned; the flag is codegen-only, so the rest of the library is fine as
    # it stands.
    if not os.path.isdir(SIGNAL):
        print("REFUSE: %s does not exist" % SIGNAL)
        return 1

    stale = []
    for root, _dirs, files in os.walk(SIGNAL):
        for f in files:
            if f.endswith(".o"):
                stale.append(os.path.join(root, f))

    for f in stale:
        os.remove(f)

    print("removed %d stale signal/ object(s) to force a rebuild" % len(stale))
    if not stale:
        print("NOTE: no objects found -- if the library was already built, "
              "the rebuild will be a no-op and the hang will persist")

    print("OK -- signal kernels will be rebuilt without guard variables")
    return 0


sys.exit(main())
