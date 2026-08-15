"""
Сборка обучающего датасета из нескольких источников.

Берёт один или несколько каталогов вида <src>/images + <src>/labels
(так их отдают fetch_open.py, pseudo_label.py и исходный E:\\DATASET),
склеивает, чистит и режет на train/val.

    python prepare.py --src E:/DATASET/... --src D:/bn_raw/oi_train \\
                      --out D:/bn_dataset --drop sidewalk,crosswalk

Что делает чистка:
  * выбрасывает битые строки разметки (в исходном датасете ~1.4% файлов
    train содержат литеральное "/n" вместо перевода строки);
  * выбрасывает боксы мельче --min-box-frac от высоты кадра. В исходном
    датасете половина разметки — объекты мельче 16 px при входе 512,
    их модель физически не может выучить, а в лоссе они шумят (METRICS.md);
  * выбрасывает классы из --drop (sidewalk и crosswalk — это площади под
    ногами, а не препятствия; на них уходит ёмкость модели впустую);
  * дедуплицирует по содержимому файла — источники пересекаются.

Отдельно печатает долю "ближней зоны": боксов, которые по формуле
D = FOCAL * real_height / box_h попадают внутрь MAX_DANGER_DIST. Именно
эти объекты система и озвучивает, поэтому их количество в обучающей
выборке важнее общего числа картинок.

Скрипт только читает исходные каталоги; результат пишется в новый --out.
"""
import argparse
import hashlib
import os
import random
import re
import shutil
import sys

import cv2
import yaml

# Совпадает с blind_nav/src/main.cpp и threat_logic.cpp.
FOCAL_LENGTH = 450.0
FRAME_HEIGHT = 480
MAX_DANGER_DIST = 7.0
REAL_HEIGHT = {0: 1.70, 1: 1.50, 2: 0.15, 3: 3.00, 4: 0.70,
               5: 0.80, 6: 0.60, 7: 0.60, 8: 0.05, 9: 0.05}

IMG_EXT = (".jpg", ".jpeg", ".png", ".bmp")
# Так помечены офлайн-копии в исходном датасете: "<id>_aug_0.jpg".
AUG_RE = re.compile(r"_aug_\d+$")


def near_field_frac(cid):
    """Минимальная доля высоты кадра, при которой объект ближе MAX_DANGER_DIST."""
    h = REAL_HEIGHT.get(cid)
    if not h:
        return None
    return FOCAL_LENGTH * h / (MAX_DANGER_DIST * FRAME_HEIGHT)


def read_label(path):
    """YOLO-разметка -> список (cls, cx, cy, w, h). Битые строки пропускаются."""
    rows, broken = [], 0
    if not os.path.exists(path):
        return rows, broken
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 5:
                if line.strip():
                    broken += 1
                continue
            try:
                cid = int(float(parts[0]))
                vals = [float(v) for v in parts[1:]]
            except ValueError:
                broken += 1
                continue
            if len(vals) == 4:
                rows.append((cid, *vals))
            else:
                # Полигон сегментации -> описанный прямоугольник.
                xs, ys = vals[0::2], vals[1::2]
                if not xs or not ys:
                    broken += 1
                    continue
                x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
                rows.append((cid, (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0))
    return rows, broken


def iter_pairs(src):
    """
    Пары (изображение, файл разметки) внутри <src>/images и <src>/labels.

    Обход рекурсивный, чтобы одинаково работали обе раскладки: плоская
    (images/*.jpg — так пишут fetch_open.py и pseudo_label.py) и разбитая
    по сплитам (images/train/*.jpg — так лежит исходный E:\\DATASET).
    Готовое деление на train/val здесь игнорируется: скрипт всё равно
    перемешивает и режет заново.
    """
    img_root = os.path.join(src, "images")
    lbl_root = os.path.join(src, "labels")
    if not os.path.isdir(img_root):
        sys.exit(f"Нет каталога {img_root}")
    for dirpath, _, filenames in os.walk(img_root):
        rel = os.path.relpath(dirpath, img_root)
        for name in sorted(filenames):
            if not name.lower().endswith(IMG_EXT):
                continue
            stem = os.path.splitext(name)[0]
            yield (os.path.join(dirpath, name),
                   os.path.join(lbl_root, rel, stem + ".txt"))


def file_hash(path, chunk=1 << 20):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", required=True,
                    help="Каталог с images/ и labels/; можно указать несколько раз")
    ap.add_argument("--out", required=True)
    ap.add_argument("--names", default=os.path.join(os.path.dirname(__file__), "sources.yaml"))
    ap.add_argument("--drop", default="", help="Классы через запятую, напр. sidewalk,crosswalk")
    ap.add_argument("--min-box-frac", type=float, default=0.03,
                    help="Минимальная высота бокса в долях кадра (0.03 ~ 16 px при входе 512)")
    ap.add_argument("--val-frac", type=float, default=0.1)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--skip-aug", action="store_true",
                    help="Пропускать офлайн-аугментации (*_aug_0.jpg и т.п.). В исходном "
                         "датасете это 58%% train и копии тех же кадров: они втрое удлиняют "
                         "эпоху, а ultralytics всё равно аугментирует на лету и разнообразнее")
    ap.add_argument("--max-side", type=int, default=0,
                    help="Ужать длинную сторону до N px (0 — копировать как есть). "
                         "Учим на 640, поэтому хранить и возить оригиналы незачем. "
                         "Несовместимо с --link: пересжатие требует записи нового файла")
    ap.add_argument("--link", action="store_true",
                    help="Жёсткие ссылки вместо копирования (быстрее, но только в пределах тома)")
    ap.add_argument("--dry-run", action="store_true", help="Только статистика, ничего не писать")
    args = ap.parse_args()

    with open(args.names, encoding="utf-8") as f:
        names = yaml.safe_load(f)["classes"]
    drop_names = {s.strip() for s in args.drop.split(",") if s.strip()}
    unknown = drop_names - set(names.values())
    if unknown:
        sys.exit(f"Неизвестные классы в --drop: {', '.join(sorted(unknown))}")
    drop_ids = {cid for cid, n in names.items() if n in drop_names}

    # id остаются исходными: они зашиты в threat_logic.cpp и в озвучку,
    # перенумеровывать после --drop нельзя. Поэтому выбрасывать можно только
    # хвост списка — ultralytics ждёт непрерывную нумерацию с нуля.
    keep_ids = [cid for cid in sorted(names) if cid not in drop_ids]
    if keep_ids != list(range(len(keep_ids))):
        sys.exit(f"--drop оставляет дырку в нумерации классов: {keep_ids}. "
                 "Выбрасывать можно только классы с наибольшими id, иначе "
                 "придётся править threat_logic.cpp и озвучку.")

    stats = {cid: [0, 0] for cid in names}  # [оставлено, из них ближняя зона]
    dropped_small = dropped_class = broken_total = 0
    seen_hashes = {}
    items = []  # (img_path, [строки разметки])

    skipped_aug = 0
    for src in args.src:
        n_src = 0
        for img_path, lbl_path in iter_pairs(src):
            stem_name = os.path.splitext(os.path.basename(img_path))[0]
            if args.skip_aug and AUG_RE.search(stem_name):
                skipped_aug += 1
                continue
            rows, broken = read_label(lbl_path)
            broken_total += broken
            kept = []
            for cid, cx, cy, w, h in rows:
                if cid in drop_ids:
                    dropped_class += 1
                    continue
                if h < args.min_box_frac:
                    dropped_small += 1
                    continue
                kept.append(f"{cid} {cx:.6f} {cy:.6f} {w:.6f} {h:.6f}")
                stats.setdefault(cid, [0, 0])
                stats[cid][0] += 1
                nf = near_field_frac(cid)
                if nf is not None and h >= nf:
                    stats[cid][1] += 1
            if not kept:
                continue
            digest = file_hash(img_path)
            if digest in seen_hashes:
                continue
            seen_hashes[digest] = img_path
            items.append((img_path, kept))
            n_src += 1
        print(f"{src}: {n_src} изображений принято")

    if not items:
        sys.exit("После фильтрации не осталось ни одного изображения")

    random.Random(args.seed).shuffle(items)
    n_val = max(1, int(len(items) * args.val_frac))
    splits = {"val": items[:n_val], "train": items[n_val:]}

    print(f"\nВсего {len(items)} изображений (train {len(splits['train'])}, val {n_val})")
    print(f"Битых строк разметки пропущено: {broken_total}")
    if args.skip_aug:
        print(f"Офлайн-аугментаций пропущено: {skipped_aug}")
    print(f"Боксов отброшено: мелких {dropped_small}, по --drop {dropped_class}")
    print(f"\n{'класс':<14}{'боксов':>10}{'ближняя зона':>16}")
    for cid in sorted(stats):
        total, near = stats[cid]
        if total:
            # id вне нашего списка означает чужую нумерацию в источнике —
            # такое лучше увидеть в отчёте, чем молча смешать с нашими классами.
            label = names.get(cid, f"<id {cid}: неизвестный класс>")
            nf = near_field_frac(cid)
            # У низких объектов (бордюр, 0.15 м) порог ближней зоны получается
            # ниже порога --min-box-frac: тогда «ближняя зона» просто повторяет
            # число оставшихся боксов и ничего не означает.
            mark = " *" if nf is not None and nf < args.min_box_frac else ""
            print(f"{label:<14}{total:>10}{near:>10} ({near / total:5.1%}){mark}")
    if any(near_field_frac(c) is not None and near_field_frac(c) < args.min_box_frac
           for c in stats if stats[c][0]):
        print("  * порог ближней зоны ниже --min-box-frac, колонка неинформативна")

    if args.dry_run:
        return

    for split, rows in splits.items():
        img_dir = os.path.join(args.out, "images", split)
        lbl_dir = os.path.join(args.out, "labels", split)
        os.makedirs(img_dir, exist_ok=True)
        os.makedirs(lbl_dir, exist_ok=True)
        for img_path, lines in rows:
            base = os.path.basename(img_path)
            dst = os.path.join(img_dir, base)
            if args.max_side:
                img = cv2.imread(img_path)
                if img is None:
                    continue
                h, w = img.shape[:2]
                if max(h, w) > args.max_side:
                    k = args.max_side / max(h, w)
                    img = cv2.resize(img, (round(w * k), round(h * k)),
                                     interpolation=cv2.INTER_AREA)
                cv2.imwrite(dst, img, [cv2.IMWRITE_JPEG_QUALITY, 88])
            elif args.link:
                if not os.path.exists(dst):
                    os.link(img_path, dst)
            else:
                shutil.copyfile(img_path, dst)
            stem = os.path.splitext(base)[0]
            with open(os.path.join(lbl_dir, stem + ".txt"), "w", encoding="utf-8") as f:
                f.write("\n".join(lines) + "\n")

    yaml_path = os.path.join(args.out, "dataset.yaml")
    with open(yaml_path, "w", encoding="utf-8") as f:
        f.write(f"path: {os.path.abspath(args.out)}\n")
        f.write("train: images/train\nval: images/val\n\nnames:\n")
        for cid in keep_ids:
            f.write(f"  {cid}: {names[cid]}\n")
        if drop_ids:
            f.write("\n# Исключены при сборке: " + ", ".join(sorted(drop_names)) + "\n")
            f.write("# id остальных классов сохранены — они зашиты в threat_logic.cpp.\n")
    print(f"\nГотово: {args.out}\nКонфиг обучения: {yaml_path}")


if __name__ == "__main__":
    main()
