PY=.venv/Scripts/python.exe
set -e
echo "### TRAIN FAST"
$PY train_embedding.py --variant fast --data data/train --epochs 30 --batch 64 --input-size 96 --embed-dim 128
$PY quantize_export.py --variant fast --repr data/repr --input-size 96 --embed-dim 128
echo "### TRAIN ACCURATE"
$PY train_embedding.py --variant accurate --data data/train --epochs 30 --batch 64 --input-size 96 --embed-dim 128
$PY quantize_export.py --variant accurate --repr data/repr --input-size 96 --embed-dim 128
echo "### EVAL + BENCH"
$PY eval_separability.py --variant fast --data data/val --input-size 96
$PY eval_separability.py --variant accurate --data data/val --input-size 96
$PY benchmark_ab.py --val data/val --input-size 96 --runs 50
echo "### ALL DONE"
