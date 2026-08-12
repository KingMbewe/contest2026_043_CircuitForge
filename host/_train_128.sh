PY=.venv/Scripts/python.exe
set -e
OUT=export128; SZ=128
echo "### FAST 128"
$PY train_embedding.py --variant fast --data data/train --epochs 30 --batch 64 --input-size $SZ --embed-dim 128 --out $OUT
$PY quantize_export.py --variant fast --repr data/repr --input-size $SZ --embed-dim 128 --out $OUT
echo "### ACCURATE 128"
$PY train_embedding.py --variant accurate --data data/train --epochs 30 --batch 64 --input-size $SZ --embed-dim 128 --out $OUT
$PY quantize_export.py --variant accurate --repr data/repr --input-size $SZ --embed-dim 128 --out $OUT
echo "### EVAL + BENCH 128"
$PY eval_separability.py --variant fast --data data/val --input-size $SZ --out $OUT
$PY eval_separability.py --variant accurate --data data/val --input-size $SZ --out $OUT
$PY benchmark_ab.py --val data/val --input-size $SZ --runs 50 --out $OUT --report ../docs/BENCHMARK_AB_128.md
echo "### ALL DONE 128"
