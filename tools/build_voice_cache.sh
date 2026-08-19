#!/bin/bash
# Предварительный синтез всех фраз, которые устройство произносит регулярно.
#
# Зачем: piper на этой плате тратит около 3.5 секунды только на загрузку модели
# при каждом запуске, плюс примерно секунду синтеза на секунду речи. Живой
# замер короткого предупреждения — 5.5 секунды от решения пайплайна до первого
# звука в наушнике. За это время человек проходит метров пять, то есть
# предупреждение приходит уже после того места, о котором предупреждает.
#
# Набор предупреждений конечен (класс x сектор x целые метры до порога
# опасности), поэтому он синтезируется заранее и на устройстве только
# проигрывается — задержка падает до задержки самого aplay, то есть до
# десятков миллисекунд.
#
# Список фраз берётся из боевого кода (blind_nav/tools/voice_phrases.cpp) и из
# исходника демона маршрутов, а не выписывается здесь: второй экземпляр
# формулировок разъехался бы с первым, и кэш молча перестал бы совпадать.
#
# Использование: build_voice_cache.sh [--force]
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/voice_env.sh"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

PHRASES_BIN="$BLINDNAV_ROOT/blind_nav/build/voice_phrases"
DAEMON_SRC="$BLINDNAV_ROOT/python/voice_nav_daemon.py"

mkdir -p "$VOICE_CACHE_DIR"

if [ ! -x "$PHRASES_BIN" ]; then
    echo "==> Собираю voice_phrases"
    ( cd "$BLINDNAV_ROOT/blind_nav/build" \
      && cmake -DBUILD_VOICE_PHRASES=ON .. >/dev/null \
      && make voice_phrases >/dev/null ) || {
        echo "Не удалось собрать voice_phrases" >&2
        exit 1
    }
fi

TMP=$(mktemp -d /dev/shm/voice_cache_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# 1. Предупреждения об опасности — из боевого кода.
"$PHRASES_BIN" > "$TMP/phrases.txt"

# 2. Фразы демона маршрутов с фиксированным текстом. Берутся прямо из
# исходника: строки с подстановкой (f-строки с фигурными скобками) пропускаем,
# они содержат названия улиц и расстояния и заранее не известны.
if [ -f "$DAEMON_SRC" ]; then
    grep -oP 'speak\(\s*f?"\K[^"]*' "$DAEMON_SRC" | grep -v '{' >> "$TMP/phrases.txt"
fi

# 3. Фразы демона, собираемые по шаблону: подсказки поворотов. Их множество
# конечно (порог объявления поворота — TURN_ANNOUNCE_MAX_DIST метров), а вот
# фразы с названиями улиц заранее не известны и в кэш не идут: они звучат,
# когда человек стоит на месте, там задержка синтеза не на пути.
if [ -f "$BLINDNAV_ROOT/python/phrases.py" ]; then
    python3 -c "
import sys
sys.path.insert(0, '$BLINDNAV_ROOT/python')
from phrases import cacheable_phrases
print('\n'.join(cacheable_phrases()))
" >> "$TMP/phrases.txt"
fi

sort -u "$TMP/phrases.txt" > "$TMP/unique.txt"
TOTAL=$(wc -l < "$TMP/unique.txt")
echo "==> Всего фраз: $TOTAL"

# Отбираем те, которых в кэше ещё нет, и сразу готовим задание для piper.
# Один запуск на всё задание — модель грузится один раз вместо $TOTAL раз.
python3 - "$TMP/unique.txt" "$TMP/job.jsonl" "$TMP/map.txt" "$VOICE_CACHE_DIR" "$TMP" "$FORCE" <<'PY'
import hashlib, json, os, sys

src, job_path, map_path, cache_dir, tmp_dir, force = sys.argv[1:]
force = force == "1"

todo = 0
with open(src, encoding="utf-8") as f, \
     open(job_path, "w", encoding="utf-8") as job, \
     open(map_path, "w", encoding="utf-8") as mapping:
    for line in f:
        text = line.rstrip("\n").strip()
        if not text:
            continue
        key = hashlib.sha1(text.encode("utf-8")).hexdigest()
        cached = os.path.join(cache_dir, key + ".raw")
        if not force and os.path.exists(cached) and os.path.getsize(cached) > 0:
            continue
        wav = os.path.join(tmp_dir, key + ".wav")
        job.write(json.dumps({"text": text, "output_file": wav}, ensure_ascii=False) + "\n")
        mapping.write(f"{key}\n")
        todo += 1

print(f"==> Синтезировать нужно: {todo}")
PY

TODO=$(wc -l < "$TMP/map.txt")
if [ "$TODO" -eq 0 ]; then
    echo "==> Кэш уже полон, синтезировать нечего."
    exit 0
fi

echo "==> Синтез (одна загрузка модели на всё задание, это надолго)..."
"$PIPER_BIN" --model "$PIPER_MODEL" --length_scale "$PIPER_LENGTH_SCALE" \
             --json-input --quiet < "$TMP/job.jsonl" 2>/dev/null

echo "==> Перекладываю в кэш"
DONE=0
FAILED=0
while read -r key; do
    wav="$TMP/$key.wav"
    if [ ! -s "$wav" ]; then
        FAILED=$((FAILED + 1))
        continue
    fi
    # piper пишет WAV, а устройство играет сырой поток — переводим один раз
    # здесь, чтобы на плате не оставалось лишней работы перед звуком.
    # -nostdin обязателен: цикл читает список ключей со стандартного ввода, а
    # ffmpeg без этого флага вычитывает его себе и глотает остаток списка —
    # первый прогон так и потерял две трети фраз, отчитавшись об успехе.
    if ffmpeg -nostdin -hide_banner -loglevel error -y -i "$wav" \
              -f s16le -ar "$PIPER_RATE" -ac 1 "$VOICE_CACHE_DIR/$key.raw.tmp" \
       && [ -s "$VOICE_CACHE_DIR/$key.raw.tmp" ]; then
        mv "$VOICE_CACHE_DIR/$key.raw.tmp" "$VOICE_CACHE_DIR/$key.raw"
        DONE=$((DONE + 1))
    else
        rm -f "$VOICE_CACHE_DIR/$key.raw.tmp"
        FAILED=$((FAILED + 1))
    fi
done < "$TMP/map.txt"

echo "==> Готово: $DONE фраз в кэше, не удалось $FAILED"
du -sh "$VOICE_CACHE_DIR"
