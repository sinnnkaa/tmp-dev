#!/usr/bin/env python3
"""Собирает голосовую дорожку прогулки из отдельных реплик piper.

Каждую реплику say.sh сохранил сырым PCM (22050 Гц, моно, S16LE) и записал в
speech.tsv момент, когда звук реально пошёл в наушник. Здесь эти реплики
раскладываются по общей шкале времени видео: смещение = метка реплики минус
момент старта записи.

Почему не ffmpeg с adelay+amix, как в tools/replay_video.sh: там реплик было
пять, а за получасовую прогулку их набирается сотни — это сотни входов -i и
столько же ветвей фильтра в одной команде. Сложение int16 в numpy делает то же
самое в две строки и не упирается ни в длину командной строки, ни в
нормализацию amix.

Тем же способом собирается дорожка голоса человека: реплики, записанные с
микрофона гарнитуры во время диктовки адреса, лежат рядом в том же каталоге со
своим индексом (mic.tsv) и своей частотой дискретизации.

Использование:
  build_speech_track.py <каталог> <индекс.tsv> <частота> <старт_epoch> <длительность_с> <выход.raw>
"""
import os
import sys

import numpy as np

def main():
    if len(sys.argv) != 7:
        print(__doc__, file=sys.stderr)
        return 2

    speech_dir, index_name, rate, start_epoch, duration, out_path = sys.argv[1:]
    rate = int(rate)
    start_epoch = float(start_epoch)
    duration = float(duration)

    track = np.zeros(max(1, int(duration * rate)), dtype=np.int32)
    tsv = os.path.join(speech_dir, index_name)

    placed, skipped = 0, 0
    if os.path.isfile(tsv):
        with open(tsv, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 2:
                    continue
                try:
                    stamp = float(parts[0])
                except ValueError:
                    continue

                utt = os.path.join(speech_dir, parts[1])
                if not os.path.isfile(utt):
                    skipped += 1
                    continue

                offset = int((stamp - start_epoch) * rate)
                if offset < 0:
                    # Реплика началась до старта отрезка (её застало
                    # переключение на новый файл) — оставляем слышимым хвост.
                    data = np.fromfile(utt, dtype=np.int16)[-offset:]
                    offset = 0
                else:
                    data = np.fromfile(utt, dtype=np.int16)

                if offset >= track.size or data.size == 0:
                    skipped += 1
                    continue

                room = min(data.size, track.size - offset)
                track[offset:offset + room] += data[:room].astype(np.int32)
                placed += 1

    # Реплики почти никогда не накладываются (их разводит общий flock), но
    # клип на всякий случай: сумма двух int16 в int16 не влезает.
    np.clip(track, -32768, 32767).astype(np.int16).tofile(out_path)
    print(f"[Дорожка] {index_name}: уложено {placed}, пропущено {skipped}, длина {duration:.1f} с")
    return 0


if __name__ == "__main__":
    sys.exit(main())
