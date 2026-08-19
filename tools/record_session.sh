#!/bin/bash
# Спутник записи прогулки: пишет микрофон и запускает автомонтаж.
#
# Видео с рамками пишет само C++ ядро (SessionRecorder в main.cpp) — оно же
# решает, когда начать новый отрезок, и публикует его тег в /dev/shm/nav_session.
# Этот скрипт следит за тегом и держит рядом с каждым отрезком непрерывную
# запись микрофона, а когда запись прекращается — собирает готовые ролики.
#
# Пишется здесь именно фоновый микрофон (вебкамера), а не гарнитура:
# bluez-источник существует только в профиле HFP, то есть ровно те несколько
# секунд, пока Vosk слушает адрес, и занимать его записью нельзя — это тот же
# поток, из которого распознаётся адрес. Диктовку с гарнитуры сохраняет сам
# демон маршрутов, а монтаж сводит обе записи в одну дорожку голоса человека.
set -u

ROOT=/root/diplom-cpp
VIDEO_DIR="$ROOT/videos"
TOOLS="$ROOT/tools"
SESSION_TAG_FILE=/dev/shm/nav_session

# Пауза перед догоняющим монтажом после перезагрузки: если плату включили,
# чтобы сразу идти, монтаж не должен отбирать ядра у распознавания в первые
# секунды прогулки.
MONTAGE_ON_START="${MONTAGE_ON_START:-1}"
MONTAGE_START_DELAY="${MONTAGE_START_DELAY:-60}"
# Сколько секунд отсутствия записи считать "прогулка кончилась".
IDLE_BEFORE_MONTAGE="${IDLE_BEFORE_MONTAGE:-30}"

export PULSE_RUNTIME_PATH=/run/user/0/pulse

MIC_PID=""
CUR_TAG=""

log() { echo "[Запись звука] $*"; }

# Микрофон, доступный всегда: не монитор синка и не гарнитура.
pick_mic_source() {
    pactl list sources short 2>/dev/null \
        | awk -F'\t' '$2 !~ /\.monitor$/ && $2 !~ /^bluez_/ {print $2; exit}'
}

start_mic() {
    local tag="$1" src
    src=$(pick_mic_source)
    if [ -z "$src" ]; then
        log "источник микрофона не найден — прогулка запишется без звука человека"
        return
    fi

    # Микрофон вебкамеры на этой плате приходит с приглушённым усилением, а
    # снимать ему предстоит улицу с расстояния вытянутой руки — поднимаем до
    # предела, тише сделать на монтаже можно, громче уже нет.
    pactl set-source-volume "$src" 100% >/dev/null 2>&1

    # Метка ставится до запуска ffmpeg; он открывает поток за доли секунды, и
    # именно на эту разницу монтаж сдвигает дорожку относительно видео.
    date +%s.%N > "$VIDEO_DIR/session_$tag.ambientstart"

    # Вывод в stdout с дозаписью в файл, а не прямая запись в файл: если ffmpeg
    # умрёт (например, PulseAudio перезапустится вместе с гарнитурой), надзор
    # ниже поднимет его снова, и кадры mp3 просто продолжат прежний файл вместо
    # того чтобы затереть уже записанное.
    ffmpeg -hide_banner -loglevel error -nostdin \
           -f pulse -i "$src" -ac 1 -ar 44100 \
           -c:a libmp3lame -q:a 5 -f mp3 pipe:1 >> "$VIDEO_DIR/ambient_$tag.mp3" &
    MIC_PID=$!
    log "пишу фоновый микрофон ($src) → ambient_$tag.mp3"
}

stop_mic() {
    [ -z "$MIC_PID" ] && return
    # SIGINT, а не SIGKILL: ffmpeg должен корректно дописать последний кадр.
    kill -INT "$MIC_PID" 2>/dev/null
    wait "$MIC_PID" 2>/dev/null
    MIC_PID=""
}

# Тег имеет смысл, только пока ядро действительно живо: после падения процесса
# в /dev/shm остаётся его последний тег, и без этой проверки микрофон писал бы
# в давно закрытый отрезок.
current_tag() {
    if ! pgrep -x blind_nav >/dev/null 2>&1; then
        echo ""
        return
    fi
    cat "$SESSION_TAG_FILE" 2>/dev/null || echo ""
}

has_backlog() {
    local meta tag
    shopt -s nullglob
    for meta in "$VIDEO_DIR"/session_*.meta; do
        tag=$(basename "$meta" .meta); tag=${tag#session_}
        [ -f "$VIDEO_DIR/session_$tag.montaged" ] && continue
        return 0
    done
    return 1
}

on_exit() {
    log "останавливаюсь"
    stop_mic
    # Монтаж отвязывается в отдельный transient-юнит: иначе systemd убьёт его
    # вместе с этим скриптом по TimeoutStopSec, не дав досчитать.
    if has_backlog; then
        systemd-run --collect --nice=19 --description="Автомонтаж прогулки BlindNav" \
            "$TOOLS/montage_session.sh" --all >/dev/null 2>&1 \
            || log "не удалось отвязать монтаж, запустите вручную: $TOOLS/montage_session.sh --all"
    fi
    exit 0
}
trap on_exit TERM INT

mkdir -p "$VIDEO_DIR"
log "старт"

if [ "$MONTAGE_ON_START" = "1" ]; then
    (
        sleep "$MONTAGE_START_DELAY"
        "$TOOLS/montage_session.sh" --all
    ) &
fi

IDLE=0
while true; do
    TAG=$(current_tag)

    if [ "$TAG" != "$CUR_TAG" ]; then
        stop_mic
        CUR_TAG="$TAG"
        [ -n "$CUR_TAG" ] && start_mic "$CUR_TAG"
    elif [ -n "$CUR_TAG" ] && [ -n "$MIC_PID" ] && ! kill -0 "$MIC_PID" 2>/dev/null; then
        log "запись микрофона оборвалась — поднимаю заново"
        MIC_PID=""
        start_mic "$CUR_TAG"
    fi

    if [ -z "$TAG" ]; then
        IDLE=$((IDLE + 1))
        if [ "$IDLE" -eq "$IDLE_BEFORE_MONTAGE" ] && has_backlog; then
            log "запись не идёт — монтирую накопившееся"
            "$TOOLS/montage_session.sh" --all
        fi
    else
        IDLE=0
    fi

    sleep 1
done
