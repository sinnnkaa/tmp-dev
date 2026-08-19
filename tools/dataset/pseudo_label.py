"""
Черновая разметка своей съёмки крупной моделью — под ручную доводку.

Открытые датасеты снимают город с капота машины или с высоты человеческого
роста в кадре-«пейзаже». Наше устройство смотрит с груди идущего человека
на 640x480, и именно этот сдвиг домена — главная причина низких метрик
(METRICS.md). Закрыть его можно только своими кадрами; плата их уже пишет
сама в videos/capture_<тег>.avi (после монтажа — videos/raw_video_<тег>.mp4).

Скрипт вынимает кадры из этих записей и прогоняет по ним большую модель
(по умолчанию COCO-претрейн yolo11x), раскладывая результат в YOLO-формат.
Это черновик: его обязательно нужно открыть в разметчике и поправить.

    pip install ultralytics
    python pseudo_label.py --video D:/board_rec/raw_video_19_08_2026.mp4 \\
                           --out D:/bn_raw/own --every 15

Важное ограничение: COCO-претрейн умеет только person, car/truck/bus,
traffic light, stop sign и bench. Классы curb, pole, trash_can он не знает
вообще — их придётся размечать руками с нуля. Зато person и car он находит
заметно лучше нашей yolo11n, и на них уходит основная часть ручной работы.

Порядок работы:
  1. этот скрипт -> черновая разметка;
  2. любой разметчик (labelImg, Label Studio, CVAT) -> правка и добавление
     недостающих классов; classes.txt рядом с результатом уже готов;
  3. prepare.py --src <out> ... -> сборка обучающего датасета.
"""
import argparse
import os
import sys

import cv2
import yaml


def load_coco_mapping(cfg_path):
    with open(cfg_path, encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    our_ids = {name: cid for cid, name in cfg["classes"].items()}
    mapping = {}
    for our_name, src_names in cfg["coco-2017"].items():
        for s in src_names:
            mapping[s] = our_ids[our_name]
    return cfg["classes"], mapping


def frames(video_path, every, limit):
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        sys.exit(f"Не открывается {video_path}")
    idx = taken = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if idx % every == 0:
            yield idx, frame
            taken += 1
            if limit and taken >= limit:
                break
        idx += 1
    cap.release()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", action="append", required=True,
                    help="Файл записи с платы; можно указать несколько раз")
    ap.add_argument("--out", required=True)
    ap.add_argument("--every", type=int, default=15,
                    help="Брать каждый N-й кадр (при 18 FPS 15 ~ раз в секунду)")
    ap.add_argument("--limit", type=int, default=0, help="Максимум кадров на видео (0 — без лимита)")
    ap.add_argument("--model", default="yolo11x.pt")
    ap.add_argument("--conf", type=float, default=0.35)
    ap.add_argument("--imgsz", type=int, default=640)
    ap.add_argument("--config", default=os.path.join(os.path.dirname(__file__), "sources.yaml"))
    args = ap.parse_args()

    names, mapping = load_coco_mapping(args.config)
    try:
        from ultralytics import YOLO
    except ImportError:
        sys.exit("Нужен ultralytics: pip install ultralytics")
    model = YOLO(args.model)

    img_dir = os.path.join(args.out, "images")
    lbl_dir = os.path.join(args.out, "labels")
    os.makedirs(img_dir, exist_ok=True)
    os.makedirs(lbl_dir, exist_ok=True)

    # Разметчикам нужен список классов в порядке id — включая те, что модель
    # не предсказывает: их проставляют руками.
    with open(os.path.join(args.out, "classes.txt"), "w", encoding="utf-8") as f:
        for cid in sorted(names):
            f.write(names[cid] + "\n")

    total_frames = total_boxes = 0
    per_class = {cid: 0 for cid in names}

    for video in args.video:
        tag = os.path.splitext(os.path.basename(video))[0]
        for idx, frame in frames(video, args.every, args.limit):
            res = model.predict(frame, imgsz=args.imgsz, conf=args.conf, verbose=False)[0]
            h, w = frame.shape[:2]
            lines = []
            for box in res.boxes:
                label = res.names[int(box.cls)]
                cid = mapping.get(label)
                if cid is None:
                    continue
                x0, y0, x1, y1 = box.xyxy[0].tolist()
                cx, cy = (x0 + x1) / 2 / w, (y0 + y1) / 2 / h
                bw, bh = (x1 - x0) / w, (y1 - y0) / h
                lines.append(f"{cid} {cx:.6f} {cy:.6f} {bw:.6f} {bh:.6f}")
                per_class[cid] += 1

            stem = f"{tag}_{idx:06d}"
            cv2.imwrite(os.path.join(img_dir, stem + ".jpg"), frame)
            # Пустой файл — валидный «на кадре ничего нет»; для ручной доводки
            # такие кадры тоже нужны, они дают модели отрицательные примеры.
            with open(os.path.join(lbl_dir, stem + ".txt"), "w", encoding="utf-8") as f:
                f.write("\n".join(lines) + ("\n" if lines else ""))
            total_frames += 1
            total_boxes += len(lines)
        print(f"{video}: обработано")

    print(f"\nКадров {total_frames}, черновых боксов {total_boxes} -> {args.out}")
    for cid, n in sorted(per_class.items()):
        print(f"  {names[cid]:<14} {n if n else '— размечать вручную'}")
    print("\nДальше: открыть каталог в разметчике, поправить боксы и добавить "
          "curb / pole / trash_can, затем скормить prepare.py")


if __name__ == "__main__":
    main()
