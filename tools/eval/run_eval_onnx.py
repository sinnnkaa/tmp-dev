"""Оценка best_3head.onnx (FP32, CPU) на подвыборке val — эталон для сравнения с NPU."""
import json
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort

import bn_eval as B

MODEL = sys.argv[1]
LIST = sys.argv[2]
IMG_ROOT = sys.argv[3]
MODE = sys.argv[4]          # squash | letterbox
OUT = sys.argv[5]

names = [l.strip() for l in open(LIST) if l.strip().startswith("images/")]
sess = ort.InferenceSession(MODEL, providers=["CPUExecutionProvider"])
inp = sess.get_inputs()[0].name

preds, gts = [], []
t0 = time.time()
for k, rel in enumerate(names):
    img = cv2.imread(f"{IMG_ROOT}/{rel}")
    h, w = img.shape[:2]
    rgb, back = B.preprocess(img, MODE)
    x = (rgb.astype(np.float32) / 255.0).transpose(2, 0, 1)[None]
    heads = sess.run(None, {inp: x})
    boxes, scores, classes = B.decode_heads(heads)
    if len(boxes):
        keep = B.nms(boxes, scores, classes)
        boxes, scores, classes = back(boxes[keep]), scores[keep], classes[keep]
    preds.append((boxes, scores, classes))
    label = rel.replace("images/", "labels/", 1).rsplit(".", 1)[0] + ".txt"
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
json.dump(res, open(OUT, "w"), ensure_ascii=False, indent=1)
print(json.dumps({k: v for k, v in res.items() if k != "per_class"}, ensure_ascii=False))
