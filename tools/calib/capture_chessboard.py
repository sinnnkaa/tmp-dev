"""
Захват кадров для калибровки камеры (шахматная доска).

Запускать на самой плате (там же, где /dev/video0), не с ПК: нужен реальный
V4L2-девайс, а картинка нужна ровно из того же тракта, что boevoy main.cpp
(FOURCC=MJPG, 640x480), иначе результат калибровки не будет соответствовать
тому, что видит детектор.

    python3 capture_chessboard.py --cols 9 --rows 6 --out /root/calib_frames

Работает в реальном времени: каждую секунду пытается найти углы доски на
свежем кадре и, если нашёл, сохраняет его и коротко пищит в консоль. Держит
доску нужно перед камерой под разными углами и на разном расстоянии — по
центру кадра и по краям, наклонённой влево/вправо/вверх/вниз. Останавливать
Ctrl+C, когда наберётся 15-20 удачных кадров (столько печатает счётчик).
"""
import argparse
import os
import time

import cv2

ap = argparse.ArgumentParser()
ap.add_argument("--device", type=int, default=0)
ap.add_argument("--cols", type=int, default=9, help="внутренних углов по горизонтали")
ap.add_argument("--rows", type=int, default=6, help="внутренних углов по вертикали")
ap.add_argument("--out", default="/root/calib_frames")
ap.add_argument("--min-interval", type=float, default=1.0,
                 help="не сохранять чаще, чем раз в N секунд — иначе один и тот же "
                      "наклон доски даст десять почти одинаковых кадров подряд")
args = ap.parse_args()

os.makedirs(args.out, exist_ok=True)
pattern = (args.cols, args.rows)

cap = cv2.VideoCapture(args.device, cv2.CAP_V4L2)
cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter.fourcc(*"MJPG"))
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
if not cap.isOpened():
    raise SystemExit(f"Камера /dev/video{args.device} не открылась")

print(f"Согласовано: {int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x"
      f"{int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))}, ищу доску {args.cols}x{args.rows} "
      "внутренних углов. Ctrl+C для остановки.")

saved = 0
last_save = 0.0
try:
    while True:
        ok, frame = cap.read()
        if not ok:
            time.sleep(0.05)
            continue

        now = time.time()
        if now - last_save < args.min_interval:
            continue

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        found, corners = cv2.findChessboardCorners(
            gray, pattern, cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE)

        if found:
            path = os.path.join(args.out, f"calib_{saved:03d}.jpg")
            cv2.imwrite(path, frame)
            saved += 1
            last_save = now
            print(f"[{saved:2d}] сохранён {path}")
        else:
            print(".", end="", flush=True)
except KeyboardInterrupt:
    pass
finally:
    cap.release()

print(f"\nГотово: {saved} кадров с найденной доской в {args.out}")
if saved < 10:
    print("Меньше 10 удачных кадров — калибровка будет неустойчивой, "
          "лучше набрать хотя бы 15.")
