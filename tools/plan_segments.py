#!/usr/bin/env python3
"""Строит временную шкалу записи по летописи session_<тег>.frames.

Ядро отмечает в этой летописи, какой кадр когда снят (см. FRAME_STAMP_EVERY в
main.cpp). Нужна она потому, что реальный темп камеры за прогулку плавает, а
монтаж до этого переклеивал всю запись под один постоянный fps из meta: на
прогулке 19.08.2026 темп прошёл от 14.4 до 16.2 кадра/с (камера ослепла на
6:09, и обработка пустых кадров стала вдвое дешевле), из-за чего к концу ролика
звук разъехался с картинкой на 23 секунды.

Здесь запись режется на минимальное число отрезков, внутри каждого из которых
постоянный fps описывает происходившее с точностью до MAX_ERR секунд. Границы
отрезков ложатся ровно на отметки летописи, поэтому ошибка не накапливается от
отрезка к отрезку: каждый следующий начинается в своё настоящее время.

У ровной прогулки на выходе будет один отрезок — то есть ровно прежнее
поведение монтажа, без единого лишнего действия.

Использование:
  plan_segments.py <session.frames> <всего_кадров> <запасной_fps>

Печатает по строке на отрезок: <первый_кадр> <кадров> <fps>
Пустой вывод означает "плана нет, монтируй как раньше".
"""
import sys

# Допустимое расхождение картинки со звуком внутри отрезка. Полсекунды человек
# на глаз уже ловит, четверть — нет; берём с запасом.
MAX_ERR = 0.4

# Потолок числа отрезков: каждый — отдельный запуск кодировщика на плате, и
# сотня отрезков стоила бы дороже самой перекодировки. Если в MAX_ERR не
# уложились, допуск ослабляется, пока план не влезет в потолок: лучше шкала с
# расхождением в секунду, чем сутки монтажа.
MAX_SEGMENTS = 24

# Границы, за которыми число уже не темп камеры, а испорченная летопись.
# Снизу намеренно почти ноль: отрезок с темпом в доли кадра в секунду — это не
# ошибка, а честно записанная пауза (запись останавливалась из-за места на
# карте и возобновлялась через полминуты, см. SessionRecorder). Выкинуть такой
# отрезок значило бы сдвинуть весь звук после паузы на её длину.
FPS_MIN, FPS_MAX = 0.01, 200.0


def read_marks(path):
    """Отметки (кадр, epoch), только строго возрастающие по обоим полям.

    Отбраковка не формальность: CLOCK_REALTIME на этой плате может шагнуть
    назад, когда после появления сети отработает синхронизация времени. Одна
    такая отметка превратила бы отрезок в отрицательную длительность.
    """
    marks = []
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) != 2:
                    continue
                try:
                    idx, stamp = int(parts[0]), float(parts[1])
                except ValueError:
                    continue
                if marks and (idx <= marks[-1][0] or stamp <= marks[-1][1]):
                    continue
                marks.append((idx, stamp))
    except OSError:
        return []
    return marks


def split(marks, max_err):
    """Жадное разбиение: отрезок тянется, пока постоянный fps врёт не больше max_err."""
    segments = []
    i = 0
    while i < len(marks) - 1:
        j = i + 1
        best = None
        while j < len(marks):
            span_frames = marks[j][0] - marks[i][0]
            span_time = marks[j][1] - marks[i][1]
            if span_time <= 0:
                break
            fps = span_frames / span_time

            # Насколько прямая от i до j расходится с промежуточными отметками.
            worst = 0.0
            for k in range(i + 1, j):
                predicted = marks[i][1] + (marks[k][0] - marks[i][0]) / fps
                worst = max(worst, abs(predicted - marks[k][1]))

            if worst > max_err and best is not None:
                break
            best = (j, fps)
            j += 1

        # Соседняя пара отметок описывается постоянным fps точно, поэтому
        # best здесь всегда есть: отрезок хотя бы на один интервал вырастет.
        j, fps = best
        segments.append([marks[i][0], marks[j][0] - marks[i][0], fps])
        i = j
    return segments


def plan(marks, total_frames):
    """Разбиение, в котором постоянный fps врёт не больше MAX_ERR.

    Если такое разбиение не влезает в MAX_SEGMENTS, допуск ослабляется. Так
    ведёт себя запись, у которой темп дёргается непрерывно (перегрев,
    конкуренция за карту): точной шкалы для неё не существует в принципе, и
    выбор стоит не между точной и грубой, а между грубой и никакой.
    """
    if len(marks) < 2:
        return []

    max_err = MAX_ERR
    segments = split(marks, max_err)
    while len(segments) > MAX_SEGMENTS:
        max_err *= 1.8
        segments = split(marks, max_err)

    if not segments:
        return []

    # Хвост после последней отметки. Обычно его нет — ядро ставит отметку и при
    # закрытии отрезка, — но запись, оборванная питанием, кончается на середине
    # интервала между отметками.
    covered = segments[-1][0] + segments[-1][1]
    if total_frames > covered:
        segments[-1][1] += total_frames - covered

    for _, count, fps in segments:
        if count <= 0 or not (FPS_MIN < fps < FPS_MAX):
            return []
    return segments


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2

    marks = read_marks(sys.argv[1])
    total_frames = int(sys.argv[2])

    segments = plan(marks, total_frames)
    if not segments:
        return 0

    for start, count, fps in segments:
        print(f"{start} {count} {fps:.6f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
