"""
Точность/полнота по классам на сетке порогов, только в зоне предупреждений.

Нужен, чтобы выбрать CONF_THRESHOLD отдельно для каждого класса. Общий порог
0.30 — компромисс, но цена ошибки у классов разная: ложное «человек справа»
дороже ложного «столб справа», а пропущенный столб дешевле пропущенного
человека. Развёртка показывает, где у какого класса выгодная точка.

    python run_thresh_sweep_rknn.py <model.rknn> <val_list.txt> <img_root> <out.json>

Инференс гоняется один раз на кадр с порогом 0.05, дальше пороги применяются
к уже посчитанным предсказаниям — поэтому вся сетка стоит столько же, сколько
один прогон.
"""
import json
import sys
import time

import cv2
import numpy as np
from rknnlite.api import RKNNLite

import bn_eval as B

# Совпадает с blind_nav/src/main.cpp и threat_logic.cpp.
FOCAL = 450.0
FRAME_H = 480.0
MAX_DIST = 7.0
REAL_H = {0: 1.70, 1: 1.50, 2: 0.15, 3: 3.00, 4: 0.70,
          5: 0.80, 6: 0.60, 7: 0.60, 8: 0.05, 9: 0.05}

GRID = [0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.50]

MODEL, LIST, IMG_ROOT, OUT = sys.argv[1:5]
names = [l.strip() for l in open(LIST) if l.strip().startswith("images/val/")]

rknn = RKNNLite()
assert rknn.load_rknn(MODEL) == 0
assert rknn.init_runtime() == 0


def near_mask(boxes, classes, img_h):
    """Те же боксы, что устройство считает попадающими в зону предупреждения."""
    if len(boxes) == 0:
        return np.zeros(0, bool)
    h_px = (boxes[:, 3] - boxes[:, 1]) / img_h * FRAME_H
    real = np.array([REAL_H.get(int(c), 1.0) for c in classes])
    return FOCAL * real / np.maximum(h_px, 1e-6) < MAX_DIST


preds, gts = [], []
t0 = time.time()
for k, rel in enumerate(names):
    img = cv2.imread(f"{IMG_ROOT}/{rel}")
    if img is None:
        continue
    h, w = img.shape[:2]
    rgb, back = B.preprocess(img, "squash")
    heads = rknn.inference(inputs=[rgb[None]])
    boxes, scores, classes = B.decode_heads(
        [np.asarray(t).reshape(74, -1) for t in heads], conf_thresh=min(GRID))
    if len(boxes):
        keep = B.nms(boxes, scores, classes)
        boxes, scores, classes = back(boxes[keep]), scores[keep], classes[keep]
        m = near_mask(boxes, classes, h)
        boxes, scores, classes = boxes[m], scores[m], classes[m]
    preds.append((boxes, scores, classes))

    gb, gc = B.load_gt(
        f"{IMG_ROOT}/{rel.replace('images/val/', 'labels/val/').rsplit('.', 1)[0]}.txt", w, h)
    if len(gb):
        m = near_mask(gb, gc, h)
        gb, gc = gb[m], gc[m]
    gts.append((gb, gc))
    if (k + 1) % 100 == 0:
        print(f"  {k + 1}/{len(names)} ({time.time() - t0:.0f}s)", flush=True)

sweep = {f"{c:.2f}": B.precision_recall_at(preds, gts, c) for c in GRID}
json.dump({"model": MODEL, "images": len(preds), "zone": f"< {MAX_DIST} m",
           "sweep": sweep}, open(OUT, "w"), ensure_ascii=False, indent=1)

# Таблица в консоль: по строке на класс, по столбцу на порог. Внизу число
# размеченных объектов зоны — по классам с единицами доверять цифрам нельзя.
print(f"\n{'класс':<14}" + "".join(f"{c:>13.2f}" for c in GRID) + f"{'n_gt':>8}")
for i, cls in enumerate(B.NAMES):
    row = [sweep[f"{c:.2f}"].get(cls) for c in GRID]
    if not any(r and r.get("n_gt") for r in row):
        continue
    n_gt = next(r["n_gt"] for r in row if r)
    cells = "".join(f"{r['precision']:>6.2f}/{r['recall']:<6.2f}" if r else f"{'—':>13}"
                    for r in row)
    print(f"{cls:<14}{cells}{n_gt:>8}")
print("\nв ячейке точность/полнота; n_gt — размеченных объектов в зоне < 7 м")
rknn.release()
