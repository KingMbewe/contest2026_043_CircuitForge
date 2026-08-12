#!/usr/bin/env python3
"""Embed a .tflite (or raw PCM test clip) as a C array for rodata linking.

Used for the micro_speech keyword-spotting path.  Both models are small enough
to live in rodata (8,772 + 18,800 B); this is NOT the flash-load path used for
the 1.4 MB pet model, which the DROM0 MMU window cannot map.

Output is byte-for-byte deterministic so the generated .cc can be committed and
diffed.  Regenerate with `make -C host models` or by hand:

    python3 host/gen_model_data.py \
        --input  <tflite> \
        --symbol g_micro_speech_quantized_model_data \
        --outdir app/velapaw/voice/gen

16-byte alignment is required: TFLM maps the flatbuffer in place and unaligned
access to a `double` field on Xtensa is a load fault, not a slow path.
"""

import argparse
import os
import sys

HEADER = """\
/* GENERATED FILE -- DO NOT EDIT.
 * Produced by host/gen_model_data.py from:
 *   {source}
 *   {nbytes} bytes
 * Regenerate rather than hand-editing.
 */

"""


def emit(data, symbol, source, outdir, is_pcm):
    os.makedirs(outdir, exist_ok=True)
    stem = symbol
    if stem.startswith("g_"):
        stem = stem[2:]

    cc_path = os.path.join(outdir, stem + ".cc")
    h_path = os.path.join(outdir, stem + ".h")

    # ---- .cc -------------------------------------------------------------
    lines = [HEADER.format(source=source, nbytes=len(data))]
    lines.append('#include "%s.h"\n\n' % stem)

    if is_pcm:
        # int16 mono PCM: emit as int16_t so the array can be handed straight
        # to the preprocessor input tensor without a cast or byte-swap.
        if len(data) % 2:
            raise SystemExit("REFUSE: PCM input has odd length %d" % len(data))
        count = len(data) // 2
        lines.append("alignas(16) const int16_t %s[] = {\n" % symbol)
        vals = []
        for i in range(count):
            lo = data[2 * i]
            hi = data[2 * i + 1]
            v = lo | (hi << 8)
            if v >= 0x8000:
                v -= 0x10000
            vals.append(v)
        for i in range(0, count, 12):
            chunk = ", ".join("%6d" % v for v in vals[i:i + 12])
            lines.append("    " + chunk + ",\n")
        lines.append("};\n")
        lines.append("const unsigned int %s_size = %d;\n" % (symbol, count))
    else:
        lines.append("alignas(16) const unsigned char %s[] = {\n" % symbol)
        for i in range(0, len(data), 12):
            chunk = ", ".join("0x%02x" % b for b in data[i:i + 12])
            lines.append("    " + chunk + ",\n")
        lines.append("};\n")
        lines.append("const unsigned int %s_size = %d;\n" % (symbol, len(data)))

    with open(cc_path, "w", newline="\n") as fh:
        fh.write("".join(lines))

    # ---- .h --------------------------------------------------------------
    guard = stem.upper() + "_H_"
    ctype = "int16_t" if is_pcm else "unsigned char"
    hdr = [HEADER.format(source=source, nbytes=len(data))]
    hdr.append("#ifndef %s\n#define %s\n\n" % (guard, guard))
    hdr.append("#include <stdint.h>\n\n")
    # No alignas here.  Alignment is a property of the object and belongs on
    # the definition in the .cc (which has it).  On a declaration, `extern
    # alignas(16) const T x[];` puts a standard attribute in the middle of the
    # decl-specifiers, which g++ rejects outright:
    #   "standard attributes in middle of decl-specifiers"
    # This matches how TFLM's own generator emits person_detect_model_data.h.
    hdr.append("extern const %s %s[];\n" % (ctype, symbol))
    hdr.append("extern const unsigned int %s_size;\n\n" % symbol)
    hdr.append("#endif  /* %s */\n" % guard)

    with open(h_path, "w", newline="\n") as fh:
        fh.write("".join(hdr))

    unit = "samples" if is_pcm else "bytes"
    n = len(data) // 2 if is_pcm else len(data)
    print("  %-46s %7d %s" % (os.path.basename(cc_path), n, unit))


def wav_payload(data):
    """Return the raw PCM payload of a 16-bit mono RIFF/WAVE file."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit("REFUSE: not a RIFF/WAVE file")
    pos = 12
    fmt_ok = False
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csz = int.from_bytes(data[pos + 4:pos + 8], "little")
        body = data[pos + 8:pos + 8 + csz]
        if cid == b"fmt ":
            channels = int.from_bytes(body[2:4], "little")
            rate = int.from_bytes(body[4:8], "little")
            bits = int.from_bytes(body[14:16], "little")
            if channels != 1 or rate != 16000 or bits != 16:
                raise SystemExit(
                    "REFUSE: need 16 kHz mono 16-bit, got %d ch / %d Hz / %d bit"
                    % (channels, rate, bits))
            fmt_ok = True
        elif cid == b"data":
            if not fmt_ok:
                raise SystemExit("REFUSE: data chunk before fmt chunk")
            return body
        pos += 8 + csz + (csz & 1)
    raise SystemExit("REFUSE: no data chunk found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--wav", action="store_true",
                    help="input is a 16 kHz mono WAV; emit int16 PCM samples")
    args = ap.parse_args()

    if not os.path.exists(args.input):
        print("REFUSE: no such file: %s" % args.input)
        return 1

    with open(args.input, "rb") as fh:
        data = fh.read()

    if not data:
        print("REFUSE: %s is empty" % args.input)
        return 1

    if args.wav:
        data = wav_payload(data)
    elif data[4:8] != b"TFL3":
        # tflite flatbuffers carry the "TFL3" file identifier at offset 4.
        print("REFUSE: %s has no TFL3 identifier -- not a .tflite"
              % args.input)
        return 1

    emit(data, args.symbol, args.input, args.outdir, args.wav)
    return 0


sys.exit(main())
