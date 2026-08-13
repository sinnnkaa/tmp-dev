import sys
import cv2
import numpy as np
import onnxruntime as ort

MODEL_PATH = "/home/sinitsina/Загрузки/diplom-cpp_backup_2026-08-12/root/diplom-cpp/weights/best.onnx"
CLASS_NAMES_RU = ["Человек", "Машина", "Бордюр", "Столб", "Знак", "Светофор", "Урна", "Скамья", "Тротуар", "Переход"]
CLASS_NAMES_EN = ["Person", "Car", "Curb", "Pole", "Sign", "Traffic Light", "Trash Can", "Bench", "Sidewalk", "Crosswalk"]

# Совпадает с threat_logic.cpp
def get_real_height(class_id):
    table = {0: 1.70, 1: 1.50, 2: 0.15, 3: 3.00, 4: 0.70, 5: 0.80, 6: 0.60, 7: 0.60, 8: 0.05, 9: 0.05}
    return table.get(class_id, 1.00)

FOCAL_LENGTH = 450.0  # откалибровано в main.cpp под конкретную камеру/разрешение платы (640x480) — см. предупреждение в отчёте

def compute_distance(real_height, box_height_px):
    if box_height_px <= 0:
        return float("inf")
    return (FOCAL_LENGTH * real_height) / box_height_px

def iou(a, b):
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    x1, y1 = max(ax1, bx1), max(ay1, by1)
    x2, y2 = min(ax2, bx2), min(ay2, by2)
    w, h = max(0.0, x2 - x1), max(0.0, y2 - y1)
    inter = w * h
    area_a = (ax2 - ax1) * (ay2 - ay1)
    area_b = (bx2 - bx1) * (by2 - by1)
    return inter / (area_a + area_b - inter + 1e-6)

def nms(dets, threshold=0.45):
    dets = sorted(dets, key=lambda d: d["conf"], reverse=True)
    kept = []
    for d in dets:
        if all(not (d["cls"] == k["cls"] and iou(d["box"], k["box"]) > threshold) for k in kept):
            kept.append(d)
    return kept

def preprocess(img_bgr, double_convert):
    resized = cv2.resize(img_bgr, (512, 512))
    rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
    if double_convert:
        # Воспроизводит старый баг: main.cpp конвертировал BGR->RGB, затем
        # infer() конвертировал ещё раз -> суммарно на NPU уходил BGR.
        rgb = cv2.cvtColor(rgb, cv2.COLOR_BGR2RGB)
    blob = rgb.transpose(2, 0, 1)
    blob = np.expand_dims(blob, axis=0).astype(np.float32) / 255.0
    return blob

def run(session, img_bgr, double_convert, conf_thresh=0.25):
    h_orig, w_orig = img_bgr.shape[:2]
    blob = preprocess(img_bgr, double_convert)
    outputs = session.run(None, {session.get_inputs()[0].name: blob})
    output = outputs[0][0]  # [14, 5376]

    boxes = output[:4, :].T
    scores = output[4:, :].T
    class_ids = np.argmax(scores, axis=1)
    confidences = np.max(scores, axis=1)

    mask = confidences > conf_thresh
    boxes, confidences, class_ids = boxes[mask], confidences[mask], class_ids[mask]

    dets = []
    for i in range(len(boxes)):
        cx, cy, w, h = boxes[i]
        x1 = (cx - w / 2) * w_orig / 512
        y1 = (cy - h / 2) * h_orig / 512
        x2 = (cx + w / 2) * w_orig / 512
        y2 = (cy + h / 2) * h_orig / 512
        dets.append({"cls": int(class_ids[i]), "conf": float(confidences[i]), "box": (x1, y1, x2, y2)})

    dets = nms(dets)
    for d in dets:
        x1, y1, x2, y2 = d["box"]
        box_h_px = y2 - y1
        d["dist_m"] = compute_distance(get_real_height(d["cls"]), box_h_px)
    return dets

def draw(img_bgr, dets, out_path):
    canvas = img_bgr.copy()
    for d in dets:
        x1, y1, x2, y2 = [int(v) for v in d["box"]]
        label = f"{CLASS_NAMES_EN[d['cls']]} {d['conf']:.2f} ~{d['dist_m']:.1f}m"
        cv2.rectangle(canvas, (x1, y1), (x2, y2), (0, 255, 0), 2)
        cv2.putText(canvas, label, (x1, max(15, y1 - 8)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
    cv2.imwrite(out_path, canvas)

def summarize(dets, label):
    print(f"  [{label}] объектов после NMS: {len(dets)}")
    for d in sorted(dets, key=lambda d: -d["conf"]):
        print(f"    {CLASS_NAMES_RU[d['cls']]:12s} conf={d['conf']:.3f}  box_h_px={d['box'][3]-d['box'][1]:.0f}  dist~{d['dist_m']:.1f}m")

if __name__ == "__main__":
    session = ort.InferenceSession(MODEL_PATH)

    images = {
        "street1": "/home/sinitsina/Загрузки/diplom-cpp_backup_2026-08-12/root/diplom-cpp/blind_nav/-0GQmYRienNVqEKiQ0Mkyw.jpg",
        "street2_aug": "/home/sinitsina/Загрузки/diplom-cpp_backup_2026-08-12/root/diplom-cpp/blind_nav/_4g6iIzKJPj_8_PTRPIV3Q_aug_0.jpg",
        "video_frame": "/tmp/claude-1000/-home-sinitsina----------diplom-cpp-backup-2026-08-12/cb6829ed-5e25-4884-bb02-94f51fd67b4d/scratchpad/video_frame.jpg",
    }

    out_dir = "/tmp/claude-1000/-home-sinitsina----------diplom-cpp-backup-2026-08-12/cb6829ed-5e25-4884-bb02-94f51fd67b4d/scratchpad"

    for name, path in images.items():
        img = cv2.imread(path)
        if img is None:
            print(f"!! Не удалось загрузить {path}")
            continue
        print(f"\n=== {name} ({img.shape[1]}x{img.shape[0]}) ===")

        dets_correct = run(session, img, double_convert=False)
        dets_buggy = run(session, img, double_convert=True)

        summarize(dets_correct, "ПРАВИЛЬНО (1x BGR->RGB)")
        summarize(dets_buggy, "СТАРЫЙ БАГ (2x BGR->RGB = фактически BGR)")

        draw(img, dets_correct, f"{out_dir}/{name}_correct.jpg")
        draw(img, dets_buggy, f"{out_dir}/{name}_buggy.jpg")
