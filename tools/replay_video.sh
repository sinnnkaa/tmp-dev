#!/bin/bash
# Полный прогон записанного видео через BlindNav: рамки, таймер, экранная
# подпись и настоящая голосовая дорожка тем же голосом, что у устройства.
#
# Что происходит:
#   1. blind_nav/build/video_replay гоняет кадры через боевой пайплайн
#      (RKNNModel::infer -> decode() -> NavPipeline) с геометрией и темпом
#      устройства и пишет размеченное видео плюс TSV "секунда <TAB> фраза";
#   2. каждая фраза озвучивается тем же piper с той же моделью и теми же
#      параметрами, что в voice_nav_daemon.py и main.cpp;
#   3. реплики раскладываются по таймлайну ровно в те секунды, когда
#      устройство их произнесло бы, и монтируются на дорожку.
#
# Голос не пересказывает происходящее — это буквально те фразы, которые
# прозвучали бы в наушниках, с теми же выдержками (3с при смене цели, 1.5с при
# резком приближении, 12с на повторе).
#
# Запуск:
#   bash tools/replay_video.sh <вход.mp4> [выходной_каталог] [fps]
#
# По умолчанию fps=25 — реальный темп INT8-модели на плате (METRICS.md).

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

IN_VIDEO="${1:-}"
OUT_DIR="${2:-$REPO_DIR/replay_out}"
FPS="${3:-25}"

PIPER="$REPO_DIR/piper/piper/piper"
PIPER_MODEL="$REPO_DIR/piper/ru_RU-irina-medium.onnx"
REPLAY_BIN="$REPO_DIR/blind_nav/build/video_replay"
MODEL="$REPO_DIR/blind_nav/model/yolo11_int8.rknn"

# Частота piper-выхода и length_scale держатся одинаковыми с боевым трактом
# (voice_nav_daemon.py speak() и main.cpp): иначе голос в ролике звучал бы
# в другом темпе, чем у устройства.
PIPER_RATE=22050
PIPER_LENGTH_SCALE=0.85

die() { echo "ОШИБКА: $*" >&2; exit 1; }

[ -n "$IN_VIDEO" ] || die "не указан входной файл. Запуск: bash tools/replay_video.sh <вход.mp4> [каталог] [fps]"
[ -f "$IN_VIDEO" ] || die "нет такого файла: $IN_VIDEO"
[ -x "$REPLAY_BIN" ] || die "нет $REPLAY_BIN. Соберите: cd blind_nav/build && cmake -DBUILD_VIDEO_REPLAY=ON .. && make video_replay"
[ -f "$MODEL" ] || die "нет модели: $MODEL"
[ -x "$PIPER" ] || die "нет piper: $PIPER"
[ -f "$PIPER_MODEL" ] || die "нет голосовой модели: $PIPER_MODEL"
command -v ffmpeg >/dev/null || die "нужен ffmpeg"

mkdir -p "$OUT_DIR"
VOICE_DIR="$OUT_DIR/voice"
rm -rf "$VOICE_DIR"
mkdir -p "$VOICE_DIR"

SILENT_VIDEO="$OUT_DIR/replay_video.avi"
EVENTS="$OUT_DIR/replay_events.tsv"
FINAL="$OUT_DIR/replay_with_voice.mp4"

echo "==> 1/3 Прогон через боевой пайплайн"
# blind_nav_main.service держит NPU: два процесса на одном NPU конфликтуют.
STOPPED_MAIN=0
if systemctl is-active --quiet blind_nav_main 2>/dev/null; then
    echo "    останавливаю blind_nav_main на время прогона (NPU занят)"
    systemctl stop blind_nav_main
    STOPPED_MAIN=1
fi
restore_main() {
    if [ "$STOPPED_MAIN" = "1" ]; then
        echo "    возвращаю blind_nav_main"
        systemctl start blind_nav_main || true
        STOPPED_MAIN=0
    fi
}
trap restore_main EXIT

"$REPLAY_BIN" "$MODEL" "$IN_VIDEO" "$SILENT_VIDEO" "$EVENTS" "$FPS"
restore_main
trap - EXIT

echo
echo "==> 2/3 Озвучка реплик через piper"
# Читаем TSV: пустые строки и комментарии пропускаем.
mapfile -t EVENT_LINES < <(grep -vE '^\s*(#|$)' "$EVENTS" || true)

if [ "${#EVENT_LINES[@]}" -eq 0 ]; then
    echo "    Реплик нет — устройство на этом ролике промолчало бы."
    echo "    Готово (без дорожки): $SILENT_VIDEO"
    exit 0
fi

idx=0
declare -a WAVS TIMES
for line in "${EVENT_LINES[@]}"; do
    t="${line%%$'\t'*}"
    phrase="${line#*$'\t'}"
    [ -n "$phrase" ] || continue

    wav="$VOICE_DIR/$(printf '%03d' "$idx").wav"
    # --output-raw + ffmpeg вместо piper --output_file: тот же сырой поток
    # s16le, что уходит в aplay на устройстве, без промежуточного формата.
    printf '%s\n' "$phrase" |
        "$PIPER" --model "$PIPER_MODEL" --length_scale "$PIPER_LENGTH_SCALE" --output-raw 2>/dev/null |
        ffmpeg -loglevel error -y -f s16le -ar "$PIPER_RATE" -ac 1 -i - "$wav"

    [ -s "$wav" ] || die "piper не озвучил фразу: $phrase"
    WAVS+=("$wav")
    TIMES+=("$t")
    echo "    [$t с] $phrase"
    idx=$((idx + 1))
done

echo
echo "==> 3/3 Монтаж дорожки на видео"
DUR=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$SILENT_VIDEO")

# Собираем один вызов ffmpeg: вход 0 — видео, вход 1 — тишина на всю длину
# (база, чтобы дорожка не обрывалась после последней реплики), дальше реплики.
# adelay сдвигает каждую в её секунду, amix с normalize=0 складывает без
# автоматического приглушения — иначе каждая реплика стала бы тише в N раз.
args=(-loglevel error -y -i "$SILENT_VIDEO"
      -f lavfi -t "$DUR" -i "anullsrc=r=$PIPER_RATE:cl=mono")

filter=""
mix_inputs="[1:a]"
n=2
for i in "${!WAVS[@]}"; do
    args+=(-i "${WAVS[$i]}")
    delay_ms=$(awk -v t="${TIMES[$i]}" 'BEGIN{printf "%d", t*1000}')
    filter+="[${n}:a]adelay=${delay_ms}|${delay_ms}[d${i}];"
    mix_inputs+="[d${i}]"
    n=$((n + 1))
done
filter+="${mix_inputs}amix=inputs=$((${#WAVS[@]} + 1)):normalize=0:duration=first[aout]"

ffmpeg "${args[@]}" -filter_complex "$filter" \
    -map 0:v -map "[aout]" \
    -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p \
    -c:a aac -b:a 128k -shortest "$FINAL"

echo
echo "=== Готово ==="
echo "Видео с голосом: $FINAL"
echo "Без голоса:      $SILENT_VIDEO"
echo "Реплики:         $EVENTS"
