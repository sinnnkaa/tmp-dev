"""Оценка .rknn на NPU платы — тот же decode/NMS/метрика, что и для ONNX на PC."""
import json
import sys
import time

import cv2
import numpy as np
from rknnlite.api import RKNNLite

import bn_eval as B

MODEL = sys.argv[1]
LIST = sys.argv[2]
IMG_ROOT = sys.argv[3]
MODE = sys.argv[4]          # squash | letterbox
OUT = sys.argv[5]

names = [l.strip() for l in open(LIST) if l.strip().startswith("images/val/")]

rknn = RKNNLite()
assert rknn.load_rknn(MODEL) == 0, "load_rknn failed"
assert rknn.init_runtime() == 0, "init_runtime failed"

preds, gts = [], []
t_infer = 0.0
t0 = time.time()
for k, rel in enumerate(names):
    img = cv2.imread(f"{IMG_ROOT}/{rel}")
    h, w = img.shape[:2]
    rgb, back = B.preprocess(img, MODE)

    ti = time.time()
    heads = rknn.inference(inputs=[rgb[None]])  # рантайм ждёт NHWC с батчем
    t_infer += time.time() - ti
    assert heads is not None, f"inference вернул None на {rel}"

    boxes, scores, classes = B.decode_heads([np.asarray(t).reshape(74, -1) for t in heads])
    if len(boxes):
        keep = B.nms(boxes, scores, classes)
        boxes, scores, classes = back(boxes[keep]), scores[keep], classes[keep]
    preds.append((boxes, scores, classes))
    label = rel.replace("images/val/", "labels/val/").rsplit(".", 1)[0] + ".txt"
    gts.append(B.load_gt(f"{IMG_ROOT}/{label}", w, h))
    if (k + 1) % 100 == 0:
        print(f"  {k + 1}/{len(names)} ({time.time() - t0:.0f}s)", flush=True)

res = B.evaluate(preds, gts)
# Порог боевого main.cpp — сколько объектов система реально называет вслух.
res["at_conf_0.30"] = B.precision_recall_at(preds, gts, 0.30)
res["at_conf_0.10"] = B.precision_recall_at(preds, gts, 0.10)
res["model"] = MODEL
res["preprocess"] = MODE
res["images"] = len(names)
res["infer_ms_avg"] = round(t_infer / max(len(names), 1) * 1000, 2)
json.dump(res, open(OUT, "w"), ensure_ascii=False, indent=1)
print(json.dumps({k: v for k, v in res.items() if k != "per_class"}, ensure_ascii=False))
rknn.release()
