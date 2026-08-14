"""
Полнота детекции в "опасной зоне" — то, за что отвечает устройство.

mAP считается по всем размеченным объектам, включая столб в конце улицы
размером в десяток пикселей. Боевой main.cpp предупреждает только об объектах
ближе MAX_DANGER_DIST=7 м, а дистанция берётся из высоты бокса:
    distance = FOCAL_LENGTH * real_height / box_height_px   (кадр 640x480)
Поэтому здесь и разметка, и предсказания фильтруются тем же правилом: остаются
только объекты, которые система обязана назвать вслух.
"""
import json
import sys

import cv2
import numpy as np
import onnxruntime as ort

import bn_eval as B

FOCAL = 450.0
FRAME_H = 480.0
MAX_DIST = 7.0
REAL_H = {0: 1.70, 1: 1.50, 2: 0.15, 3: 3.00, 4: 0.70,
          5: 0.80, 6: 0.60, 7: 0.60, 8: 0.05, 9: 0.05}

MODEL, LIST, IMG_ROOT, OUT = sys.argv[1:5]
names = [l.strip() for l in open(LIST) if l.strip().startswith("images/")]
sess = ort.InferenceSession(MODEL, providers=["CPUExecutionProvider"])
inp = sess.get_inputs()[0].name


def near_mask(boxes, classes, img_h):
    """True для объектов, которые по высоте бокса оказываются ближе 7 м."""
    if len(boxes) == 0:
        return np.zeros(0, bool)
    h_px = (boxes[:, 3] - boxes[:, 1]) / img_h * FRAME_H
    real = np.array([REAL_H.get(int(c), 1.0) for c in classes])
    with np.errstate(divide="ignore"):
        dist = FOCAL * real / np.maximum(h_px, 1e-6)
    return dist < MAX_DIST


preds, gts = [], []
for k, rel in enumerate(names):
    img = cv2.imread(f"{IMG_ROOT}/{rel}")
    h, w = img.shape[:2]
    rgb, back = B.preprocess(img, "squash")
    x = (rgb.astype(np.float32) / 255.0).transpose(2, 0, 1)[None]
    heads = sess.run(None, {inp: x})
    boxes, scores, classes = B.decode_heads(heads, conf_thresh=0.05)
    if len(boxes):
        keep = B.nms(boxes, scores, classes)
        boxes, scores, classes = back(boxes[keep]), scores[keep], classes[keep]
        m = near_mask(boxes, classes, h)
        boxes, scores, classes = boxes[m], scores[m], classes[m]
    preds.append((boxes, scores, classes))

    gb, gc = B.load_gt(f"{IMG_ROOT}/{rel.replace('images/', 'labels/', 1).rsplit('.', 1)[0]}.txt", w, h)
    if len(gb):
        m = near_mask(gb, gc, h)
        gb, gc = gb[m], gc[m]
    gts.append((gb, gc))
    if (k + 1) % 100 == 0:
        print(f"  {k + 1}/{len(names)}", flush=True)

res = {"model": MODEL, "images": len(names), "zone": f"< {MAX_DIST} m",
       "at_conf_0.30": B.precision_recall_at(preds, gts, 0.30),
       "at_conf_0.10": B.precision_recall_at(preds, gts, 0.10)}
json.dump(res, open(OUT, "w"), ensure_ascii=False, indent=1)
print("saved", OUT)
