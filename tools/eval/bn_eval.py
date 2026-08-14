"""
Общий код оценки качества детектора для BlindNav.

Один и тот же decode/NMS/метрика используется для трёх вариантов модели —
best.onnx (FP32 на PC), yolo11_final.rknn (FP16 на NPU) и кандидата INT8, —
чтобы числа были сравнимы между собой, а не только каждое с собой.

Порядок каналов в головах повторяет blind_nav/src/decode.cpp:
  0..63  — DFL (4 стороны x reg_max=16), 64..73 — логиты классов.

Препроцессинг умеет два режима:
  squash    — cv::resize в 512x512 без сохранения пропорций (так делает боевой C++);
  letterbox — с сохранением пропорций и серыми полями (так обучалась модель).
Разница между ними и показывает, сколько точности теряется на препроцессинге.
"""
import os
import numpy as np
import cv2

NAMES = ["person", "car", "curb", "pole", "traffic_sign", "traffic_light",
         "trash_can", "bench", "sidewalk", "crosswalk"]
INPUT = 512
STRIDES = (8, 16, 32)
GRIDS = (64, 32, 16)


def preprocess(img, mode="squash"):
    """Возвращает (uint8 RGB 512x512, обратное преобразование в координаты оригинала)."""
    h, w = img.shape[:2]
    if mode == "squash":
        out = cv2.resize(img, (INPUT, INPUT))
        def back(boxes):
            boxes[:, [0, 2]] *= w / INPUT
            boxes[:, [1, 3]] *= h / INPUT
            return boxes
    else:
        r = min(INPUT / h, INPUT / w)
        nh, nw = int(round(h * r)), int(round(w * r))
        resized = cv2.resize(img, (nw, nh))
        out = np.full((INPUT, INPUT, 3), 114, dtype=np.uint8)
        top, left = (INPUT - nh) // 2, (INPUT - nw) // 2
        out[top:top + nh, left:left + nw] = resized
        def back(boxes):
            boxes[:, [0, 2]] = (boxes[:, [0, 2]] - left) / r
            boxes[:, [1, 3]] = (boxes[:, [1, 3]] - top) / r
            return boxes
    return cv2.cvtColor(out, cv2.COLOR_BGR2RGB), back


def _softmax(v, axis):
    e = np.exp(v - v.max(axis, keepdims=True))
    return e / e.sum(axis, keepdims=True)


def decode_heads(heads, conf_thresh=0.001):
    """3 тензора [1,74,G,G] (или [74,G,G]) -> (boxes xyxy в пикселях входа, scores, classes)."""
    all_boxes, all_scores, all_cls = [], [], []
    for t, grid, stride in zip(heads, GRIDS, STRIDES):
        t = np.asarray(t, dtype=np.float32).reshape(74, grid * grid)
        scores = 1.0 / (1.0 + np.exp(-t[64:]))
        cls = scores.argmax(0)
        conf = scores.max(0)
        keep = conf > conf_thresh
        if not keep.any():
            continue
        idx = np.nonzero(keep)[0]
        d = (_softmax(t[:64].reshape(4, 16, -1), 1) * np.arange(16)[None, :, None]).sum(1)[:, idx]
        gy, gx = np.divmod(idx, grid)
        x0 = (gx + 0.5 - d[0]) * stride
        y0 = (gy + 0.5 - d[1]) * stride
        x1 = (gx + 0.5 + d[2]) * stride
        y1 = (gy + 0.5 + d[3]) * stride
        all_boxes.append(np.stack([x0, y0, x1, y1], 1))
        all_scores.append(conf[idx])
        all_cls.append(cls[idx])
    if not all_boxes:
        return np.zeros((0, 4), np.float32), np.zeros(0, np.float32), np.zeros(0, np.int32)
    return np.concatenate(all_boxes), np.concatenate(all_scores), np.concatenate(all_cls)


def iou_matrix(a, b):
    area_a = (a[:, 2] - a[:, 0]).clip(0) * (a[:, 3] - a[:, 1]).clip(0)
    area_b = (b[:, 2] - b[:, 0]).clip(0) * (b[:, 3] - b[:, 1]).clip(0)
    lt = np.maximum(a[:, None, :2], b[None, :, :2])
    rb = np.minimum(a[:, None, 2:], b[None, :, 2:])
    wh = (rb - lt).clip(0)
    inter = wh[..., 0] * wh[..., 1]
    return inter / (area_a[:, None] + area_b[None] - inter + 1e-9)


def nms(boxes, scores, classes, iou_thresh=0.7, max_det=300):
    """Классозависимый NMS, как в ultralytics val (agnostic=False)."""
    keep_all = []
    for c in np.unique(classes):
        idx = np.nonzero(classes == c)[0]
        order = idx[scores[idx].argsort()[::-1]]
        while order.size:
            i = order[0]
            keep_all.append(i)
            if order.size == 1:
                break
            ious = iou_matrix(boxes[i][None], boxes[order[1:]])[0]
            order = order[1:][ious <= iou_thresh]
    keep_all = np.array(keep_all, dtype=int)
    keep_all = keep_all[scores[keep_all].argsort()[::-1]][:max_det]
    return keep_all


def load_gt(label_path, w, h):
    """YOLO-разметка (боксы или полигоны) -> (boxes xyxy в пикселях, classes)."""
    boxes, classes = [], []
    if not os.path.exists(label_path):
        return np.zeros((0, 4), np.float32), np.zeros(0, np.int32)
    for line in open(label_path):
        parts = line.split()
        if len(parts) < 5:
            continue
        try:
            cid = int(float(parts[0]))
            vals = np.array(parts[1:], dtype=np.float32)
        except ValueError:
            # ~1% файлов в train содержит литеральное "/n" вместо перевода
            # строки (пришло из внешнего kaggle-источника) — такую строку
            # пропускаем, а не роняем весь прогон.
            continue
        if len(vals) == 4:
            cx, cy, bw, bh = vals
            x0, y0, x1, y1 = cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2
        else:
            # Полигон сегментации -> описанный прямоугольник (так же делает ultralytics).
            pts = vals.reshape(-1, 2)
            x0, y0 = pts[:, 0].min(), pts[:, 1].min()
            x1, y1 = pts[:, 0].max(), pts[:, 1].max()
        boxes.append([x0 * w, y0 * h, x1 * w, y1 * h])
        classes.append(cid)
    if not boxes:
        return np.zeros((0, 4), np.float32), np.zeros(0, np.int32)
    return np.array(boxes, np.float32), np.array(classes, np.int32)


def precision_recall_at(predictions, ground_truth, conf, iou=0.5, num_classes=len(NAMES)):
    """
    Точность/полнота при конкретном пороге уверенности — то, что реально видит
    пользователь: боевой main.cpp отбрасывает всё ниже CONF_THRESHOLD=0.30,
    тогда как mAP считается от порога 0.001 и этот эффект скрывает.
    """
    out = {}
    for c in range(num_classes):
        n_gt = sum(int((g[1] == c).sum()) for g in ground_truth)
        if n_gt == 0:
            continue
        tp = fp = 0
        for (pb, ps, pc), (gb, gc) in zip(predictions, ground_truth):
            m = (pc == c) & (ps >= conf)
            pred = pb[m]
            order = ps[m].argsort()[::-1]
            gt = gb[gc == c]
            taken = np.zeros(len(gt), bool)
            for i in order:
                if len(gt) == 0:
                    fp += 1
                    continue
                ious = iou_matrix(pred[i][None], gt)[0]
                j = int(ious.argmax())
                if ious[j] >= iou and not taken[j]:
                    taken[j] = True
                    tp += 1
                else:
                    fp += 1
        out[NAMES[c]] = {
            "precision": round(tp / max(tp + fp, 1), 4),
            "recall": round(tp / n_gt, 4),
            "tp": tp, "fp": fp, "n_gt": n_gt,
        }
    return out


def evaluate(predictions, ground_truth, num_classes=len(NAMES)):
    """
    mAP по правилам COCO (IoU 0.50:0.05:0.95, интерполяция по 101 точке).

    predictions:   список на изображение — (boxes xyxy, scores, classes)
    ground_truth:  список на изображение — (boxes xyxy, classes)
    """
    thresholds = np.arange(0.5, 0.96, 0.05)
    ap = np.zeros((num_classes, len(thresholds)))
    present = np.zeros(num_classes, dtype=bool)

    for c in range(num_classes):
        n_gt = sum(int((g[1] == c).sum()) for g in ground_truth)
        if n_gt == 0:
            continue
        present[c] = True

        rows = []  # (score, img_idx, box)
        for i, (pb, ps, pc) in enumerate(predictions):
            m = pc == c
            for b, s in zip(pb[m], ps[m]):
                rows.append((s, i, b))
        if not rows:
            continue
        rows.sort(key=lambda r: -r[0])

        for ti, thr in enumerate(thresholds):
            matched = {i: np.zeros(int((ground_truth[i][1] == c).sum()), bool) for i in range(len(ground_truth))}
            tp = np.zeros(len(rows))
            fp = np.zeros(len(rows))
            for k, (_, i, box) in enumerate(rows):
                gb, gc = ground_truth[i]
                gb = gb[gc == c]
                if len(gb) == 0:
                    fp[k] = 1
                    continue
                ious = iou_matrix(box[None], gb)[0]
                j = int(ious.argmax())
                if ious[j] >= thr and not matched[i][j]:
                    matched[i][j] = True
                    tp[k] = 1
                else:
                    fp[k] = 1
            ctp, cfp = np.cumsum(tp), np.cumsum(fp)
            recall = ctp / n_gt
            precision = ctp / np.maximum(ctp + cfp, 1e-9)
            # 101-точечная интерполяция COCO
            prec = np.concatenate(([0.0], precision, [0.0]))
            for m in range(len(prec) - 2, -1, -1):
                prec[m] = max(prec[m], prec[m + 1])
            rec = np.concatenate(([0.0], recall, [recall[-1] if len(recall) else 0.0]))
            grid = np.linspace(0, 1, 101)
            ap[c, ti] = np.interp(grid, rec, prec[:len(rec)]).mean()

    per_class = {NAMES[c]: {"AP50": round(float(ap[c, 0]), 4),
                            "AP50_95": round(float(ap[c].mean()), 4)}
                 for c in range(num_classes) if present[c]}
    return {
        "mAP50": round(float(ap[present, 0].mean()), 4),
        "mAP50_95": round(float(ap[present].mean()), 4),
        "classes_evaluated": int(present.sum()),
        "per_class": per_class,
    }
