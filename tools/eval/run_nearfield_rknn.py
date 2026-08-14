"""То же, что run_nearfield.py, но модель считается на NPU платы (FP16 или INT8)."""
import json
import sys

import cv2
import numpy as np
from rknnlite.api import RKNNLite

import bn_eval as B

FOCAL = 450.0
FRAME_H = 480.0
MAX_DIST = 7.0
REAL_H = {0: 1.70, 1: 1.50, 2: 0.15, 3: 3.00, 4: 0.70,
          5: 0.80, 6: 0.60, 7: 0.60, 8: 0.05, 9: 0.05}

MODEL, LIST, IMG_ROOT, OUT = sys.argv[1:5]
names = [l.strip() for l in open(LIST) if l.strip().startswith("images/val/")]

rknn = RKNNLite()
assert rknn.load_rknn(MODEL) == 0
assert rknn.init_runtime() == 0


def near_mask(boxes, classes, img_h):
    if len(boxes) == 0:
        return np.zeros(0, bool)
    h_px = (boxes[:, 3] - boxes[:, 1]) / img_h * FRAME_H
    real = np.array([REAL_H.get(int(c), 1.0) for c in classes])
    return FOCAL * real / np.maximum(h_px, 1e-6) < MAX_DIST


preds, gts = [], []
for k, rel in enumerate(names):
    img = cv2.imread(f"{IMG_ROOT}/{rel}")
    h, w = img.shape[:2]
    rgb, back = B.preprocess(img, "squash")
    heads = rknn.inference(inputs=[rgb[None]])
    boxes, scores, classes = B.decode_heads([np.asarray(t).reshape(74, -1) for t in heads], conf_thresh=0.05)
    if len(boxes):
        keep = B.nms(boxes, scores, classes)
        boxes, scores, classes = back(boxes[keep]), scores[keep], classes[keep]
        m = near_mask(boxes, classes, h)
        boxes, scores, classes = boxes[m], scores[m], classes[m]
    preds.append((boxes, scores, classes))

    gb, gc = B.load_gt(f"{IMG_ROOT}/{rel.replace('images/val/', 'labels/val/').rsplit('.', 1)[0]}.txt", w, h)
    if len(gb):
        m = near_mask(gb, gc, h)
        gb, gc = gb[m], gc[m]
    gts.append((gb, gc))
    if (k + 1) % 100 == 0:
        print(f"  {k + 1}/{len(names)}", flush=True)

json.dump({"model": MODEL, "images": len(names), "zone": f"< {MAX_DIST} m",
           "at_conf_0.30": B.precision_recall_at(preds, gts, 0.30),
           "at_conf_0.10": B.precision_recall_at(preds, gts, 0.10)},
          open(OUT, "w"), ensure_ascii=False, indent=1)
print("saved", OUT)
rknn.release()
