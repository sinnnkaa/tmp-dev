"""
Выгрузка подвыборки открытого датасета в наш YOLO-формат.

Тянет из Open Images V7 или COCO-2017 только те изображения, где есть
интересные нам классы, и сразу перекладывает разметку в наши id из
sources.yaml. Скачивается не весь датасет (Open Images это ~18 ТБ),
а именно нужные картинки — за это отвечает fiftyone.

    pip install fiftyone
    python fetch_open.py --source open-images-v7 --split train --max-samples 8000 --out D:/bn_raw/oi_train

Результат — обычная YOLO-раскладка:
    <out>/images/*.jpg
    <out>/labels/*.txt      # "<cls> <cx> <cy> <w> <h>", нормированные

Дальше подвыборку нужно прогнать через prepare.py: он выкидывает мелкие
боксы, дедуплицирует и режет на train/val.

Скрипт ничего не удаляет и не трогает существующие датасеты; при повторном
запуске в тот же каталог файлы перезаписываются по именам изображений.
"""
import argparse
import os
import shutil
import sys

import yaml

SOURCES = {
    "open-images-v7": "open-images-v7",
    "coco-2017": "coco-2017",
}


def load_mapping(cfg_path, source):
    with open(cfg_path, encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    if source not in cfg:
        sys.exit(f"В {cfg_path} нет секции '{source}'")
    our_ids = {name: cid for cid, name in cfg["classes"].items()}
    # Обратное отображение: имя класса источника -> наш id.
    mapping = {}
    for our_name, src_names in cfg[source].items():
        if our_name not in our_ids:
            sys.exit(f"Класс '{our_name}' есть в секции {source}, но нет в classes")
        for s in src_names:
            mapping[s] = our_ids[our_name]
    return cfg["classes"], mapping


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", choices=sorted(SOURCES), default="open-images-v7")
    ap.add_argument("--split", default="train", choices=["train", "validation", "test"])
    ap.add_argument("--max-samples", type=int, default=5000,
                    help="Ограничение сверху; без него Open Images качается сутками")
    ap.add_argument("--out", required=True, help="Каталог для images/ и labels/")
    ap.add_argument("--config", default=os.path.join(os.path.dirname(__file__), "sources.yaml"))
    ap.add_argument("--dry-run", action="store_true",
                    help="Показать, что будет скачано, и выйти")
    args = ap.parse_args()

    names, mapping = load_mapping(args.config, args.source)
    wanted = sorted(mapping)
    print(f"Источник:  {args.source} / {args.split}")
    print(f"Классы:    {', '.join(wanted)}")
    print(f"Лимит:     {args.max_samples} изображений")
    for src_name, cid in sorted(mapping.items(), key=lambda kv: kv[1]):
        print(f"  {src_name:<16} -> {cid} {names[cid]}")
    if args.dry_run:
        return

    try:
        import fiftyone.zoo as foz
    except ImportError:
        sys.exit("Нужен fiftyone: pip install fiftyone")

    # COCO в fiftyone называет валидацию 'validation' так же, как Open Images.
    ds = foz.load_zoo_dataset(
        SOURCES[args.source],
        split=args.split,
        label_types=["detections"],
        classes=wanted,
        max_samples=args.max_samples,
        dataset_name=f"bn_{args.source}_{args.split}_{args.max_samples}",
    )

    img_dir = os.path.join(args.out, "images")
    lbl_dir = os.path.join(args.out, "labels")
    os.makedirs(img_dir, exist_ok=True)
    os.makedirs(lbl_dir, exist_ok=True)

    kept_images = 0
    kept_boxes = 0
    per_class = {cid: 0 for cid in names}

    for sample in ds:
        dets = sample.ground_truth.detections if sample.ground_truth else []
        lines = []
        for d in dets:
            cid = mapping.get(d.label)
            if cid is None:
                continue
            # fiftyone отдаёт bounding_box как [x, y, w, h] в долях кадра
            # от левого верхнего угла; YOLO хочет центр.
            x, y, w, h = d.bounding_box
            if w <= 0 or h <= 0:
                continue
            cx, cy = x + w / 2, y + h / 2
            lines.append(f"{cid} {cx:.6f} {cy:.6f} {w:.6f} {h:.6f}")
            per_class[cid] += 1
        if not lines:
            continue

        stem = os.path.splitext(os.path.basename(sample.filepath))[0]
        ext = os.path.splitext(sample.filepath)[1] or ".jpg"
        # Префикс источника, чтобы имена не столкнулись при слиянии датасетов.
        stem = f"{args.source.split('-')[0]}_{stem}"
        shutil.copyfile(sample.filepath, os.path.join(img_dir, stem + ext))
        with open(os.path.join(lbl_dir, stem + ".txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        kept_images += 1
        kept_boxes += len(lines)

    print(f"\nСохранено: {kept_images} изображений, {kept_boxes} боксов -> {args.out}")
    for cid, n in sorted(per_class.items()):
        if n:
            print(f"  {names[cid]:<14} {n}")


if __name__ == "__main__":
    main()
