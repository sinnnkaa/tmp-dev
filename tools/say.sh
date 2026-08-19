#!/bin/bash
# Единая точка озвучки: синтез фразы piper'ом и её воспроизведение в гарнитуру.
#
# Раньше одинаковый конвейер "piper --output-raw | tee | aplay" был выписан
# дважды — в main.cpp через std::system и в voice_nav_daemon.py через
# subprocess. Любая правка адресации звука доходила только до одной половины
# системы (так C++ ядро и осталось с "-D default", когда демон уже адресовался
# bluez-синку по имени). Теперь конвейер один и правится в одном месте.
#
# Вторая задача — запись реплики для автомонтажа (tools/montage_session.sh):
# сохраняем сырой PCM фразы и момент, когда звук реально пошёл в наушник.
#
# Использование: say.sh "текст фразы" [core|nav]
#   core — предупреждение об опасности от C++ ядра
#   nav  — голос маршрута от python-демона
set -u

ROOT=/root/diplom-cpp
TEXT="${1:-}"
SRC="${2:-core}"
[ -z "$TEXT" ] && exit 0

# У systemd-юнитов нет своего XDG_RUNTIME_DIR, а сервер PulseAudio поднимает
# bt_keeper.sh под root — без этой переменной pactl/aplay не находят сервер
# и молча уходят в сырой ALSA мимо наушников.
export PULSE_RUNTIME_PATH=/run/user/0/pulse

PIPER="$ROOT/piper/piper/piper"
PIPER_MODEL="$ROOT/piper/ru_RU-irina-medium.onnx"
AUDIO_LOG="$ROOT/system_audio.raw"
AUDIO_LOG_MAX=$((20 * 1024 * 1024))
SESSION_TAG_FILE=/dev/shm/nav_session
VIDEO_DIR="$ROOT/videos"

# Куда играть. Указатель default у PulseAudio на этой плате указывает на
# встроенный кодек платы и переставляется при каждой смене BT-профиля, поэтому
# спрашиваем фактическое имя bluez-синка у сервера. Если гарнитуры нет
# (не подключена, села батарея) — остаётся default: звук уйдёт хоть куда-то,
# а не пропадёт совсем.
SINK=$(pactl list sinks short 2>/dev/null | awk -F'\t' '$2 ~ /^bluez_sink\./ {print $2; exit}')
if [ -n "$SINK" ]; then DEV="pulse:$SINK"; else DEV="default"; fi

# Куда сохранять реплику. Тег текущей записи публикует C++ ядро; если запись
# не идёт, реплика уходит во временный файл в tmpfs и удаляется после
# воспроизведения — озвучка не должна зависеть от того, пишется ли видео.
TAG=""
[ -r "$SESSION_TAG_FILE" ] && TAG=$(cat "$SESSION_TAG_FILE" 2>/dev/null)
STAMP=$(date +%s%N)
SPEECH_DIR="$VIDEO_DIR/speech_$TAG"
if [ -n "$TAG" ] && [ -d "$SPEECH_DIR" ]; then
    UTT="$SPEECH_DIR/utt_${STAMP}_${SRC}.raw"
    TSV="$SPEECH_DIR/speech.tsv"
else
    UTT="/dev/shm/nav_utt_$$.raw"
    TSV=""
fi

if [ -f "$AUDIO_LOG" ] && [ "$(stat -c%s "$AUDIO_LOG" 2>/dev/null || echo 0)" -gt "$AUDIO_LOG_MAX" ]; then
    : > "$AUDIO_LOG"
fi

# Синтез и воспроизведение разделены, а не соединены пайпом, как было раньше.
# Причина: на этой плате piper отдаёт первый байт практически одновременно с
# последним (замер на короткой фразе: 5.49 с до первого байта против 5.68 с на
# всё), то есть потоковости в пайпе фактически нет и терять от разделения
# нечего. Взамен появляется точный момент начала звука — тот, что нужен
# автомонтажу, чтобы положить фразу на видеодорожку в ту же секунду, в которой
# её услышал человек.
printf '%s\n' "$TEXT" | "$PIPER" --model "$PIPER_MODEL" --length_scale 0.85 --output-raw 2>/dev/null > "$UTT"

if [ ! -s "$UTT" ]; then
    [ -z "$TSV" ] && rm -f "$UTT"
    exit 1
fi

[ -n "$TSV" ] && printf '%s\t%s\t%s\n' "$(date +%s.%N)" "$(basename "$UTT")" "$TEXT" >> "$TSV"

# tee в system_audio.raw сохранён: это давний общий лог всего произнесённого,
# на него смотрят при разборе прогулок.
tee -a "$AUDIO_LOG" < "$UTT" | aplay -D "$DEV" -r 22050 -f S16_LE -t raw -c 1 2>/dev/null

[ -z "$TSV" ] && rm -f "$UTT"
exit 0
