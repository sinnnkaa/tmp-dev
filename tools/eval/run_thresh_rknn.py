"""Точность/полнота при боевых порогах уверенности (без mAP — считается быстро)."""
import json
import sys
import time

import cv2
import numpy as np
from rknnlite.api import RKNNLite

import bn_eval as B

MODEL, LIST, IMG_ROOT, MODE, OUT = sys.argv[1:6]
names = [l.strip() for l in open(LIST) if l.strip().startswith("images/val/")]

rknn = RKNNLite()
assert rknn.load_rknn(MODEL) == 0
assert rknn.init_runtime() == 0

preds, gts = [], []
t0 = time.time()
for k, rel in enumerate(names):
    img = cv2.imread(f"{IMG_ROOT}/{rel}")
    h, w = img.shape[:2]
    rgb, back = B.preprocess(img, MODE)
    heads = rknn.inference(inputs=[rgb[None]])
    # Порог 0.05: всё, что ниже, не влияет ни на один из считаемых порогов,
    # зато на порядок ускоряет NMS и подсчёт.
    boxes, scores, classes = B.decode_heads([np.asarray(t).reshape(74, -1) for t in heads], conf_thresh=0.05)
    if len(boxes):
        keep = B.nms(boxes, scores, classes)
        boxes, scores, classes = back(boxes[keep]), scores[keep], classes[keep]
    preds.append((boxes, scores, classes))
    label = rel.replace("images/val/", "labels/val/").rsplit(".", 1)[0] + ".txt"
    gts.append(B.load_gt(f"{IMG_ROOT}/{label}", w, h))
    if (k + 1) % 100 == 0:
        print(f"  {k + 1}/{len(names)} ({time.time() - t0:.0f}s)", flush=True)

res = {
    "model": MODEL,
    "preprocess": MODE,
    "images": len(names),
    "at_conf_0.30": B.precision_recall_at(preds, gts, 0.30),
    "at_conf_0.10": B.precision_recall_at(preds, gts, 0.10),
}
json.dump(res, open(OUT, "w"), ensure_ascii=False, indent=1)
print("saved", OUT)
rknn.release()
