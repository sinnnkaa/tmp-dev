#!/bin/bash
# Автомонтаж прогулки: из записанных кусков собирает готовый ролик.
#
# На входе то, что накопилось за отрезок записи в videos/:
#   capture_<тег>.avi     — кадры с рамками (MJPG, пишет C++ ядро)
#   session_<тег>.meta    — старт, число кадров, реальный FPS
#   session_<тег>.frames  — летопись «кадр N снят в момент T» для точной шкалы
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

# Кодирует запись по переменной шкале: каждый отрезок плана — своим темпом,
# потом склейка без перекодирования.
#
# Отрезками, а не одним проходом с setpts на всю запись, потому что выражение
# для такой шкалы пришлось бы собирать из десятков вложенных условий, и его
# работоспособность зависела бы от версии ffmpeg на плате. Здесь задействованы
# только -ss, setpts и concat — то, что есть в любой сборке.
#
# Перемотка считается по СОБСТВЕННОМУ темпу файла (его берём у ffprobe), а не
# по темпу отрезка: -ss работает во временной шкале входного файла, а в
# заголовке AVI стоит номинальные 30 кадров/с независимо от того, с какой
# скоростью кадры снимались на самом деле. Кадры MJPG все ключевые, поэтому
# перемотка попадает точно в нужный кадр.
encode_variable() {
    local tag="$1" capture="$2" out="$3" plan="$4"
    local dir list native k=0 start count segfps ss seg

    native=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate \
             -of csv=p=0 "$capture" 2>/dev/null | awk -F/ 'NF==2 && $2>0 {print $1/$2}')
    if ! awk -v f="${native:-0}" 'BEGIN{exit !(f > 0)}'; then
        echo "[Монтаж] $tag: не удалось узнать темп файла — переменная шкала невозможна"
        return 1
    fi

    dir=$(mktemp -d "$VIDEO_DIR/segments_${tag}_XXXXXX") || return 1
    list="$dir/list.txt"
    : > "$list"

    while read -r start count segfps; do
        [ -z "${start:-}" ] && continue
        seg="$dir/seg_$k.mp4"
        ss=$(awk -v a="$start" -v f="$native" 'BEGIN{printf "%.6f", a / f}')
        # -vsync 0 обязателен: без него ffmpeg выровняет кадры под постоянный
        # темп и выбросит ровно ту разметку времени, ради которой всё это.
        # Единый timescale у всех отрезков нужен склейке: со своей шкалой у
        # каждого она отказывается сшивать их без перекодирования.
        if ! ff -ss "$ss" -i "$capture" -frames:v "$count" \
                -vf "setpts=N/($segfps*TB)" -vsync 0 \
                -c:v libx264 -preset ultrafast -crf 26 -threads 2 -pix_fmt yuv420p -an \
                -video_track_timescale 90000 "$seg"; then
            rm -rf "$dir"
            return 1
        fi
        printf "file '%s'\n" "$seg" >> "$list"
        k=$((k + 1))
    done <<< "$plan"

    if ! ff -f concat -safe 0 -i "$list" -c copy "$out"; then
        rm -rf "$dir"
        return 1
    fi
    rm -rf "$dir"
    [ -s "$out" ] || return 1

    # Сверка результата с планом. Перемотка и склейка ведут себя по-разному в
    # разных сборках ffmpeg, а ошибка здесь молчаливая: ролик соберётся, просто
    # покажет не то. Дешевле проверить, чем узнать об этом на защите.
    local got_frames got_dur want_frames want_dur
    got_frames=$(ffprobe -v error -select_streams v:0 -count_packets \
                 -show_entries stream=nb_read_packets -of csv=p=0 "$out" 2>/dev/null | tr -d '\r')
    got_dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$out" 2>/dev/null)
    want_frames=$(printf '%s\n' "$plan" | awk '{n += $2} END {print n}')
    want_dur=$(printf '%s\n' "$plan" | awk '{d += $2 / $3} END {printf "%.3f", d}')

    if [ "${got_frames:-0}" != "$want_frames" ] \
       || ! awk -v a="${got_dur:-0}" -v b="$want_dur" 'BEGIN{exit !(a-b < 1 && b-a < 1)}'; then
        echo "[Монтаж] $tag: переменная шкала собралась неверно" \
             "(кадров ${got_frames:-?} вместо $want_frames, длина ${got_dur:-?} вместо $want_dur)"
        rm -f "$out"
        return 1
    fi
    return 0
}

# -nostdin: монтаж запускается и из циклов, и из systemd-юнита без
# терминала — ffmpeg не должен трогать чужой стандартный ввод.
ff() { nice -n 19 ionice -c 3 ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }

# Читается ли файл как готовый mp4: есть видеопоток и ненулевая длительность.
# Пакеты не считаем — это чтение всего файла, а вызывается проверка там, где
# ролик уже собран и надо решить, можно ли удалять исходник.
mp4_is_playable() {
    local f="$1" dur
    [ -s "$f" ] || return 1
    ffprobe -v error -select_streams v:0 -show_entries stream=codec_type \
            -of csv=p=0 "$f" 2>/dev/null | grep -q video || return 1
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f" 2>/dev/null)
    awk -v d="${dur:-0}" 'BEGIN{exit !(d > 0.5)}'
}

# Пишется ли отрезок прямо сейчас. Тег в /dev/shm — это только то, что в нём
# написано: после падения или убийства ядра там остаётся его последний тег, и
# проверка по одному лишь тегу навсегда исключала бы эту прогулку из монтажа.
# record_session.sh в том же месте сверяется с pgrep — здесь этого не делали.
tag_is_recording() {
    local tag="$1"
    [ -n "$tag" ] || return 1
    pgrep -x blind_nav >/dev/null 2>&1 || return 1
    [ "$tag" = "$(cat "$SESSION_TAG_FILE" 2>/dev/null || true)" ]
}

montage_one() {
    local tag="$1"
    local meta="$VIDEO_DIR/session_$tag.meta"

    if [ ! -f "$meta" ]; then
        echo "[Монтаж] $tag: нет $meta — пропускаю"
        return 1
    fi

    local capture start_epoch fps frames
    capture=$(meta_get "$meta" capture)
    start_epoch=$(meta_get "$meta" start_epoch)
    fps=$(meta_get "$meta" fps)
    frames=$(meta_get "$meta" frames)

    [ -n "$capture" ] || capture="capture_$tag.avi"
    capture="$VIDEO_DIR/$capture"

    if [ ! -f "$capture" ]; then
        echo "[Монтаж] $tag: нет $capture — пропускаю"
        return 1
    fi

    # meta, не пережившая срыв питания. ext4 фиксирует переименование раньше
    # данных, поэтому после обрыва файл читается как нули правильной длины —
    # так потерялась прогулка 19_08_2026_3 при полностью записанном видео.
    # Само ядро теперь сбрасывает meta на карту (SessionRecorder::write_meta),
    # но записи, оборванные до этой правки, и любой другой повреждённый файл
    # монтаж обязан вытянуть сам: сто мегабайт отснятой прогулки не должны
    # пропадать из-за семи строк заголовка. Всё недостающее восстанавливается
    # из самой записи и из метки старта микрофона.
    if [ -z "$frames" ] || [ -z "$start_epoch" ]; then
        echo "[Монтаж] $tag: meta повреждена — восстанавливаю по самой записи"

        if [ -z "$frames" ]; then
            # Именно счёт пакетов, а не nb_frames из заголовка: у записи,
            # оборванной питанием, заголовок AVI дописать никто не успел.
            frames=$(ffprobe -v error -select_streams v:0 -count_packets \
                     -show_entries stream=nb_read_packets -of csv=p=0 \
                     "$capture" 2>/dev/null | tr -d '\r')
        fi

        if [ -z "$start_epoch" ]; then
            # Микрофон стартует в пределах секунды от начала отрезка, и его
            # метка пишется отдельным файлом — она переживает обрыв чаще.
            start_epoch=$(cat "$VIDEO_DIR/session_$tag.ambientstart" 2>/dev/null)
        fi

        # Последний рубеж: время последней записи в файл минус длительность
        # по номинальным 30 кадрам. Точность хуже, но ролик собирается.
        if [ -z "$start_epoch" ] && [ -n "$frames" ]; then
            start_epoch=$(awk -v m="$(stat -c %Y "$capture")" -v n="$frames" \
                          'BEGIN{printf "%.3f", m - n/30}')
        fi

        # fps считаем по факту: кадры за время от старта до последней записи.
        if [ -n "$frames" ] && [ -n "$start_epoch" ]; then
            fps=$(awk -v m="$(stat -c %Y "$capture")" -v s="$start_epoch" -v n="$frames" \
                  'BEGIN{d=m-s; if (d>1) printf "%.6f", n/d; else print ""}')
        fi

        if [ -z "$frames" ] || [ -z "$start_epoch" ]; then
            echo "[Монтаж] $tag: восстановить не удалось — пропускаю"
            return 1
        fi
        echo "[Монтаж] $tag: восстановлено — кадров $frames, старт $start_epoch"
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

    # Временная шкала по летописи кадров (session_<тег>.frames, пишет ядро).
    # Один fps на всю запись — это среднее, а реальный темп камеры за прогулку
    # успевает измениться: 19.08.2026 он прошёл от 14.4 до 16.2 кадра/с, и звук
    # к концу ролика разъехался с картинкой на 23 секунды. Летопись говорит,
    # какой кадр когда снят, и запись режется на отрезки со своим темпом.
    #
    # Летописи нет ни у одной записи, сделанной до этой правки, а у ровной
    # прогулки план состоит из одного отрезка — в обоих случаях монтаж идёт
    # прежним путём, и ничего лишнего не происходит.
    local plan="" nseg=0
    if [ -s "$VIDEO_DIR/session_$tag.frames" ]; then
        plan=$("$TOOLS/plan_segments.py" "$VIDEO_DIR/session_$tag.frames" "$frames" "$fps" 2>/dev/null || true)
        [ -n "$plan" ] && nseg=$(printf '%s\n' "$plan" | grep -c .)
    fi
    if [ "$nseg" -gt 1 ]; then
        duration=$(printf '%s\n' "$plan" | awk '{d += $2 / $3} END {printf "%.3f", d}')
        echo "[Монтаж] $tag: темп камеры плавал — шкала из $nseg отрезков, итого $duration с"
    fi

    local raw_mp4="$VIDEO_DIR/raw_video_$tag.mp4"
    local piper_mp3="$VIDEO_DIR/piper_sound_$tag.mp3"
    local full_mp4="$VIDEO_DIR/full_video_$tag.mp4"
    local mic_mp3="$VIDEO_DIR/micro_sound_$tag.mp3"
    local ambient_mp3="$VIDEO_DIR/ambient_$tag.mp3"

    # 1. Картинка.
    echo "[Монтаж] $tag: кодирую видео..."
    local encoded=0
    if [ "$nseg" -gt 1 ]; then
        if encode_variable "$tag" "$capture" "$raw_mp4" "$plan"; then
            encoded=1
        else
            # Переменная шкала — улучшение, а не условие сборки: не вышла, так
            # собираем как раньше. Ролик с разъехавшимся звуком всё же лучше,
            # чем отсутствующий.
            echo "[Монтаж] $tag: переменная шкала не собралась — возвращаюсь к постоянному темпу"
            duration=$(awk -v n="$frames" -v f="$fps" 'BEGIN{printf "%.3f", n/f}')
        fi
    fi
    if [ "$encoded" = 0 ]; then
        # -r перед -i переопределяет темп входного файла: без этого ролик
        # проигрывался бы в заголовочные 30 fps, и звук уезжал бы от картинки
        # тем сильнее, чем длиннее прогулка.
        if ! ff -r "$fps" -i "$capture" \
                -c:v libx264 -preset ultrafast -crf 26 -threads 2 -pix_fmt yuv420p -an "$raw_mp4"; then
            echo "[Монтаж] $tag: кодирование видео не удалось"
            return 1
        fi
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

    # Проверка именно на пригодность файла, а не на «непустой». Дальше отрезок
    # помечается смонтированным и исходный capture удаляется — то есть после
    # этой строки исходных кадров больше не существует. Оборванный на середине
    # mp4 (кончилось место, ffmpeg убит по TimeoutStopSec, питание) размер имеет
    # ненулевой и проверку [ -s ] проходил, унося с собой запись прогулки.
    if ! mp4_is_playable "$full_mp4"; then
        echo "[Монтаж] $tag: готовый ролик не читается — исходник оставляю"
        rm -f "$full_mp4"
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
        tag_is_recording "$tag" && continue
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
