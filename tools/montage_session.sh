#!/bin/bash
# Автомонтаж прогулки: из записанных кусков собирает готовый ролик.
#
# На входе то, что накопилось за отрезок записи в videos/:
#   capture_<тег>.avi     — кадры с рамками (MJPG, пишет C++ ядро)
#   session_<тег>.meta    — старт, число кадров, реальный FPS
#   speech_<тег>/         — реплики piper и куски диктовки с метками времени
#   ambient_<тег>.mp3     — непрерывный микрофон (пишет record_session.sh)
#
# На выходе:
#   raw_video_<тег>.mp4   — картинка с рамками без звука
#   piper_sound_<тег>.mp3 — голос модели на шкале времени видео
#   micro_sound_<тег>.mp3 — голос человека: диктовка с гарнитуры плюс общий фон
#   full_video_<тег>.mp4  — картинка + голос модели + голос человека
#
# Использование:
#   montage_session.sh <тег>     смонтировать один отрезок
#   montage_session.sh --all     смонтировать всё несмонтированное
#
# Переменные окружения:
#   KEEP_CAPTURE=1  не удалять исходный capture_<тег>.avi после монтажа
#   VOICE_GAIN, MIC_GAIN  громкость дорожек в общем миксе
set -u

TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TOOLS/voice_env.sh"

LOCK=/run/lock/blind_nav_montage.lock

KEEP_CAPTURE="${KEEP_CAPTURE:-0}"
# Голос модели заметно громче микрофона: микрофон здесь — это уличный фон и
# реплики человека, он не должен перекрывать предупреждение.
VOICE_GAIN="${VOICE_GAIN:-1.0}"
MIC_GAIN="${MIC_GAIN:-0.6}"
# Диктовка с гарнитуры — это ровно голос человека, её слышно должно быть
# хорошо; непрерывный микрофон вебкамеры даёт уличный фон и идёт тише.
HEADSET_GAIN="${HEADSET_GAIN:-1.0}"
AMBIENT_GAIN="${AMBIENT_GAIN:-0.8}"

meta_get() { sed -n "s/^$2=//p" "$1" | tail -1; }

# -nostdin: монтаж запускается и из циклов, и из systemd-юнита без
# терминала — ffmpeg не должен трогать чужой стандартный ввод.
ff() { nice -n 19 ionice -c 3 ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }

montage_one() {
    local tag="$1"
    local meta="$VIDEO_DIR/session_$tag.meta"

    if [ ! -f "$meta" ]; then
        echo "[Монтаж] $tag: нет $meta — пропускаю"
        return 1
    fi

    local capture start_epoch fps frames
    capture="$VIDEO_DIR/$(meta_get "$meta" capture)"
    start_epoch=$(meta_get "$meta" start_epoch)
    fps=$(meta_get "$meta" fps)
    frames=$(meta_get "$meta" frames)

    if [ ! -f "$capture" ]; then
        echo "[Монтаж] $tag: нет $capture — пропускаю"
        return 1
    fi

    # Отрезок короче секунды монтировать нечего: так выглядит запись,
    # оборванная сразу после старта (например, рестартом сервиса).
    if [ -z "$frames" ] || [ "$frames" -lt 30 ]; then
        echo "[Монтаж] $tag: всего $frames кадров — пропускаю"
        return 1
    fi

    # FPS считает C++ ядро по факту (кадры / прошедшее время) — заголовок AVI
    # всегда номинальные 30. Если значение вышло бессмысленным (например, meta
    # успела записаться до первых кадров), берём номинал: лучше небольшой
    # рассинхрон, чем несобранный ролик.
    if ! awk -v f="$fps" 'BEGIN{exit !(f+0 > 1 && f+0 < 120)}'; then
        echo "[Монтаж] $tag: подозрительный fps=$fps, беру 30"
        fps=30
    fi

    local duration
    duration=$(awk -v n="$frames" -v f="$fps" 'BEGIN{printf "%.3f", n/f}')
    echo "[Монтаж] $tag: $frames кадров, $fps fps, $duration с"

    local raw_mp4="$VIDEO_DIR/raw_video_$tag.mp4"
    local piper_mp3="$VIDEO_DIR/piper_sound_$tag.mp3"
    local full_mp4="$VIDEO_DIR/full_video_$tag.mp4"
    local mic_mp3="$VIDEO_DIR/micro_sound_$tag.mp3"
    local ambient_mp3="$VIDEO_DIR/ambient_$tag.mp3"

    # 1. Картинка. -r перед -i переопределяет темп входного файла: без этого
    # ролик проигрывался бы в заголовочные 30 fps, и звук уезжал бы от
    # картинки тем сильнее, чем длиннее прогулка.
    echo "[Монтаж] $tag: кодирую видео..."
    if ! ff -r "$fps" -i "$capture" \
            -c:v libx264 -preset ultrafast -crf 26 -threads 2 -pix_fmt yuv420p -an "$raw_mp4"; then
        echo "[Монтаж] $tag: кодирование видео не удалось"
        return 1
    fi

    # 2. Голос модели, разложенный по шкале времени видео.
    local speech_dir tmp_speech tmp_headset
    speech_dir="$VIDEO_DIR/speech_$tag"
    tmp_speech=$(mktemp /dev/shm/track_piper_XXXXXX.raw)
    "$TOOLS/build_speech_track.py" "$speech_dir" speech.tsv "$PIPER_RATE" "$start_epoch" "$duration" "$tmp_speech"
    ff -f s16le -ar "$PIPER_RATE" -ac 1 -i "$tmp_speech" -c:a libmp3lame -q:a 4 "$piper_mp3"
    rm -f "$tmp_speech"

    # 3. Голос человека. Две независимые записи, и это не избыточность:
    # микрофон гарнитуры существует только в профиле HFP, то есть ровно те
    # секунды, пока Vosk слушает адрес — зато в них голос чистый и близкий.
    # Микрофон вебкамеры доступен всегда, но снимает улицу целиком и заметно
    # тише. По отдельности каждый источник теряет половину прогулки, вместе
    # дают связную дорожку.
    tmp_headset=$(mktemp /dev/shm/track_mic_XXXXXX.raw)
    "$TOOLS/build_speech_track.py" "$speech_dir" mic.tsv "$HEADSET_RATE" "$start_epoch" "$duration" "$tmp_headset"

    if [ -s "$ambient_mp3" ]; then
        local amb_epoch delay_ms trim
        amb_epoch=$(cat "$VIDEO_DIR/session_$tag.ambientstart" 2>/dev/null || echo "$start_epoch")
        delay_ms=$(awk -v a="$amb_epoch" -v b="$start_epoch" 'BEGIN{d=(a-b)*1000; if(d<0)d=0; printf "%d", d}')
        trim=$(awk -v a="$amb_epoch" -v b="$start_epoch" 'BEGIN{d=b-a; if(d<0)d=0; printf "%.3f", d}')

        ff -f s16le -ar "$HEADSET_RATE" -ac 1 -i "$tmp_headset" -ss "$trim" -i "$ambient_mp3" \
           -filter_complex \
           "[0:a]volume=$HEADSET_GAIN[h];[1:a]adelay=$delay_ms:all=1,volume=$AMBIENT_GAIN[b];[h][b]amix=inputs=2:normalize=0:duration=first[m]" \
           -map "[m]" -c:a libmp3lame -q:a 5 "$mic_mp3"
    else
        echo "[Монтаж] $tag: непрерывной записи микрофона нет, беру только диктовку с гарнитуры"
        ff -f s16le -ar "$HEADSET_RATE" -ac 1 -i "$tmp_headset" -c:a libmp3lame -q:a 5 "$mic_mp3"
    fi
    rm -f "$tmp_headset"

    # 4. Сведение. Обе звуковые дорожки уже равны длине видео и начинаются с
    # его нуля, поэтому сдвигать здесь нечего.
    echo "[Монтаж] $tag: свожу звук..."
    if [ -s "$mic_mp3" ]; then
        ff -i "$raw_mp4" -i "$piper_mp3" -i "$mic_mp3" \
           -filter_complex \
           "[1:a]volume=$VOICE_GAIN[p];[2:a]volume=$MIC_GAIN[m];[p][m]amix=inputs=2:normalize=0:duration=first[a]" \
           -map 0:v -map "[a]" -c:v copy -c:a aac -b:a 128k -shortest "$full_mp4"
    else
        ff -i "$raw_mp4" -i "$piper_mp3" \
           -map 0:v -map 1:a -c:v copy -c:a aac -b:a 128k -shortest "$full_mp4"
    fi

    if [ ! -s "$full_mp4" ]; then
        echo "[Монтаж] $tag: свести звук не удалось"
        return 1
    fi

    : > "$VIDEO_DIR/session_$tag.montaged"

    # Исходный MJPG весит впятеро больше готового mp4 и на карте держать его
    # незачем: те же кадры целиком лежат в raw_video_<тег>.mp4. KEEP_CAPTURE=1
    # оставляет исходник, если он всё же нужен.
    if [ "$KEEP_CAPTURE" != "1" ]; then
        rm -f "$capture"
    fi

    echo "[Монтаж] $tag: готово → $full_mp4"
    return 0
}

montage_all() {
    local any=0
    shopt -s nullglob
    for meta in "$VIDEO_DIR"/session_*.meta; do
        local tag
        tag=$(basename "$meta" .meta); tag=${tag#session_}
        [ -f "$VIDEO_DIR/session_$tag.montaged" ] && continue
        # Отрезок, который прямо сейчас пишется, не трогаем.
        [ "$tag" = "$(cat "$SESSION_TAG_FILE" 2>/dev/null || true)" ] && continue
        any=1
        montage_one "$tag" || true
    done
    [ "$any" = 0 ] && echo "[Монтаж] Несмонтированных прогулок нет."
    return 0
}

main() {
    mkdir -p "$VIDEO_DIR"
    if [ $# -lt 1 ]; then
        echo "Использование: $0 <тег> | --all" >&2
        exit 2
    fi
    if [ "$1" = "--all" ]; then
        montage_all
    else
        montage_one "$1"
    fi
}

# Монтаж тяжёлый и однопоточным его держит не вежливость, а NPU: параллельные
# кодировщики отбирают ядра у распознавания препятствий.
exec 9>"$LOCK"
# Ждать бесконечно нельзя: монтаж запускается и по простою, и по остановке
# сервиса, и вручную — без ограничения набралась бы очередь из оболочек,
# висящих до перезагрузки. Час — заведомо больше самого долгого отрезка.
if ! flock -w 3600 9; then
    echo "[Монтаж] Другой монтаж уже идёт больше часа — выхожу."
    exit 0
fi
main "$@"
