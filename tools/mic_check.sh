#!/bin/bash
# Проверка микрофонов перед съёмкой: слышно ли вообще то, что они пишут.
#
# Записывает по несколько секунд с фонового микрофона (вебкамера) и с
# микрофона гарнитуры, если она в профиле HFP, и печатает уровень сигнала.
# Нужен именно живой прогон: тишина в записи прогулки выглядит точно так же,
# как рабочий микрофон в тихой комнате, и по логам их не отличить.
#
# Использование: mic_check.sh [секунд]
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/voice_env.sh"

SECONDS_TO_RECORD="${1:-5}"
TMP=$(mktemp -d /dev/shm/mic_check_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

level() {
    ffmpeg -hide_banner -i "$1" -af volumedetect -f null - 2>&1 \
        | sed -n 's/.*\(mean_volume\|max_volume\): \(.*\)/  \1: \2/p'
}

verdict() {
    local mean
    mean=$(ffmpeg -hide_banner -i "$1" -af volumedetect -f null - 2>&1 \
        | sed -n 's/.*mean_volume: \(-\?[0-9.]*\) dB/\1/p')
    [ -z "$mean" ] && { echo "  не удалось измерить"; return; }
    if awk -v m="$mean" 'BEGIN{exit !(m < -60)}'; then
        echo "  ВЕРДИКТ: тишина. Микрофон не пишет или усиление на нуле."
    elif awk -v m="$mean" 'BEGIN{exit !(m < -45)}'; then
        echo "  ВЕРДИКТ: очень тихо. На улице голос человека может пропасть."
    else
        echo "  ВЕРДИКТ: сигнал есть."
    fi
}

check_source() {
    local name="$1" src="$2"
    echo "=== $name: $src"
    pactl set-source-volume "$src" 100% >/dev/null 2>&1
    echo "  говорите вслух ${SECONDS_TO_RECORD} секунд..."
    if ! ffmpeg -nostdin -hide_banner -loglevel error -f pulse -i "$src" \
            -t "$SECONDS_TO_RECORD" -ac 1 -ar 16000 -y "$TMP/$name.wav"; then
        echo "  записать не удалось"
        return
    fi
    level "$TMP/$name.wav"
    verdict "$TMP/$name.wav"
}

AMBIENT=$(pactl list sources short 2>/dev/null \
    | awk -F'\t' '$2 !~ /\.monitor$/ && $2 !~ /^bluez_/ {print $2; exit}')
HEADSET=$(pactl list sources short 2>/dev/null \
    | awk -F'\t' '$2 ~ /^bluez_source\./ {print $2; exit}')

if [ -n "$AMBIENT" ]; then
    check_source "фоновый" "$AMBIENT"
else
    echo "=== фоновый микрофон не найден"
fi

echo
if [ -n "$HEADSET" ]; then
    check_source "гарнитура" "$HEADSET"
else
    echo "=== микрофона гарнитуры сейчас нет."
    echo "    Он появляется только в профиле HFP. Переключить:"
    echo "    pactl set-card-profile bluez_card.<MAC> handsfree_head_unit"
fi
