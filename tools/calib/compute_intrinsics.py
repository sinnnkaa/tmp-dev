"""
Считает fx/fy/cx/cy и коэффициенты дисторсии по кадрам из capture_chessboard.py.

    python3 compute_intrinsics.py --frames /root/calib_frames --cols 9 --rows 6 --square 25

--square — сторона одной клетки доски в мм. Без неё fx/fy посчитаются в
"единицах клетки", а не в пикселях на реальный метр — расстояние потом будет
неправильным на постоянный множитель. Измерить линейкой сам распечатанный
лист, не верить размеру в PDF: принтер почти никогда не печатает 1:1.

Выход: FOCAL_LENGTH для main.cpp (это fy, потому что compute_distance в
threat_logic.cpp использует высоту бокса, то есть вертикальный размер) и
полная матрица с cx/cy/дисторсией — на случай, если дальше понадобится
cv::undistort (шаг из CALIBRATION.md, этап 1, пункт 3).
"""
import argparse
import glob
import os

import cv2
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--frames", required=True)
ap.add_argument("--cols", type=int, default=9)
ap.add_argument("--rows", type=int, default=6)
ap.add_argument("--square", type=float, default=1.0,
                 help="сторона клетки доски в мм (измерить линейкой на распечатке)")
args = ap.parse_args()

pattern = (args.cols, args.rows)
objp = np.zeros((args.rows * args.cols, 3), np.float32)
objp[:, :2] = np.mgrid[0:args.cols, 0:args.rows].T.reshape(-1, 2) * args.square

paths = sorted(glob.glob(os.path.join(args.frames, "*.jpg")))
if not paths:
    raise SystemExit(f"Нет .jpg в {args.frames}")

objpoints, imgpoints = [], []
size = None
used = 0
for p in paths:
    img = cv2.imread(p)
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    size = gray.shape[::-1]
    found, corners = cv2.findChessboardCorners(gray, pattern)
    if not found:
        print(f"  пропущен (доска не нашлась повторно): {p}")
        continue
    corners = cv2.cornerSubPix(
        gray, corners, (11, 11), (-1, -1),
        (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001))
    objpoints.append(objp)
    imgpoints.append(corners)
    used += 1

if used < 8:
    raise SystemExit(f"Только {used} валидных кадров — мало для устойчивой калибровки, "
                      "нужно от 10-15 под разными углами")

rms, mtx, dist, _, _ = cv2.calibrateCamera(objpoints, imgpoints, size, None, None)

fx, fy = mtx[0, 0], mtx[1, 1]
cx, cy = mtx[0, 2], mtx[1, 2]

print(f"\nИспользовано кадров: {used}/{len(paths)}")
print(f"Размер кадра: {size[0]}x{size[1]}")
print(f"RMS ошибка репроекции: {rms:.3f} px "
      f"({'хорошо' if rms < 1.0 else 'многовато, стоит добавить кадров/углов' if rms < 2.0 else 'плохо — переснять'})")
print(f"\nfx = {fx:.2f} px")
print(f"fy = {fy:.2f} px   <- это FOCAL_LENGTH для main.cpp/rknn_image_test.cpp/rknn_video_bench.cpp")
print(f"cx = {cx:.2f}, cy = {cy:.2f}")
print(f"distCoeffs = {dist.ravel().tolist()}")

if args.square == 1.0:
    print("\n--square не задан (=1.0) — fx/fy сейчас в единицах клетки доски, "
          "а не в пикселях на метр. Измерьте клетку линейкой и пересчитайте с "
          "правильным --square, иначе дистанция будет ошибаться на постоянный "
          "множитель.")
