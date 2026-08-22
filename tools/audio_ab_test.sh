#!/bin/bash
# Разбор "плохого звука в наушниках" на слух: одна и та же фраза, четыре пути
# до гарнитуры. Скрипт ничего не чинит — он отвечает на вопрос, ГДЕ портится.
#
# Зачем нужен именно A/B, а не догадки: симптом "хруст, скрип, прерывания"
# одинаково выдают три разные причины, и лечатся они по-разному.
#
#   1. Пересчёт частоты. Кэш лежит в 22050 моно (частота модели piper), а
#      bluez-синк работает на 44100 стерео — каждую фразу PulseAudio
#      пересчитывает на лету. Если сервер собран без speex, дефолтный
#      ресемплер молча падает до "trivial", то есть до выбрасывания отсчётов:
#      на речи это металлический призвук и скрип.
#   2. Старт потока. Боевой путь запускает aplay на КАЖДУЮ фразу: новый
#      поток, новый разгон SBC-кодера, полторы секунды звука, закрытие.
#      Если рвётся только начало, а длинная склейка играет чисто — виноват
#      старт, а не тракт.
#   3. Радио. Тогда рвётся всё одинаково, независимо от формата и длины.
#
# Прогон 4 (paplay) дополнительно снимает вопрос об alsa-плагине pulse: он
# отдаёт звук серверу напрямую, минуя alsa-lib.
#
# Слушать нужно В НАУШНИКАХ и подряд, не отвлекаясь: разница между 1 и 2
# слышна сразу, если она есть.
#
# Использование: audio_ab_test.sh [файл_из_кэша.raw]
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/voice_env.sh"

SRC="${1:-}"
if [ -z "$SRC" ]; then
    # Самый длинный файл кэша: на длинной фразе дефекты слышно лучше, чем на
    # "Осторожно".
    SRC=$(ls -S "$VOICE_CACHE_DIR"/*.raw 2>/dev/null | head -1)
fi
if [ ! -s "${SRC:-}" ]; then
    echo "Нечего играть: в $VOICE_CACHE_DIR нет готовых реплик." >&2
    echo "Сначала tools/build_voice_cache.sh" >&2
    exit 1
fi

DEV=$(voice_playback_device)
SINK=${DEV#pulse:}

echo "=== Обстановка ==="
echo "Файл:      $SRC ($(stat -c %s "$SRC") байт, $(awk -v b="$(stat -c %s "$SRC")" -v r="$PIPER_RATE" 'BEGIN{printf "%.1f", b/2/r}') с)"
echo "Устройство: $DEV"
echo
echo "-- синк --"
pactl list sinks 2>/dev/null | grep -A 20 "Name: $SINK" \
    | grep -E "Name:|State:|Sample Specification:|Latency:" | sed 's/^/   /'
echo
echo "-- ресемплер --"
# Если speex-float-1 в списке нет, дефолт сервера молча становится trivial.
if pulseaudio --dump-resample-methods 2>/dev/null | grep -q '^speex-float-1$'; then
    echo "   speex-float-1 доступен"
else
    echo "   ВНИМАНИЕ: speex-float-1 НЕДОСТУПЕН — сервер пересчитывает частоту"
    echo "   методом trivial. Это и есть скрип. Прогон 2 должен звучать заметно чище."
fi
grep -E "^\s*resample-method" /etc/pulse/daemon.conf 2>/dev/null | sed 's/^/   daemon.conf: /'
echo
echo "-- модули --"
pactl list short modules 2>/dev/null \
    | grep -E "suspend-on-idle|bluetooth-policy|bluez" | sed 's/^/   /'
echo

TMP=$(mktemp -d /dev/shm/ab_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

announce() {
    echo
    echo ">>> $1"
    echo "    (Enter — играть, s — пропустить)"
    read -r -n 1 key
    echo
    [ "$key" = "s" ] && return 1
    return 0
}

# --- 1. Боевой путь -------------------------------------------------------
if announce "Прогон 1 из 4. БОЕВОЙ ПУТЬ: 22050 моно, пересчёт в сервере."; then
    aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$SRC"
fi

# --- 2. Без пересчёта -----------------------------------------------------
# Пересчёт делает ffmpeg (soxr/swr, качество несопоставимо с trivial), синк
# получает свой родной формат и не трогает поток вовсе.
if announce "Прогон 2 из 4. БЕЗ ПЕРЕСЧЁТА: заранее в 44100 стерео."; then
    if ffmpeg -nostdin -hide_banner -loglevel error -y \
              -f s16le -ar "$PIPER_RATE" -ac 1 -i "$SRC" \
              -ar 44100 -ac 2 -f s16le "$TMP/44.raw"; then
        aplay -D "$DEV" -r 44100 -f S16_LE -t raw -c 2 "$TMP/44.raw"
    else
        echo "    ffmpeg не справился — прогон пропущен."
    fi
fi

# --- 3. Длинный непрерывный поток ----------------------------------------
# Десять реплик подряд одним запуском aplay. Если здесь чисто, а в боевом
# режиме рвётся — дело в старте каждого потока, а не в тракте.
if announce "Прогон 3 из 4. ДЛИННЫЙ ПОТОК: десять реплик одним запуском."; then
    : > "$TMP/long.raw"
    ls -S "$VOICE_CACHE_DIR"/*.raw 2>/dev/null | head -10 | while read -r f; do
        cat "$f" >> "$TMP/long.raw"
    done
    aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$TMP/long.raw"
fi

# --- 4. Мимо alsa-плагина -------------------------------------------------
if announce "Прогон 4 из 4. НАПРЯМУЮ В СЕРВЕР: paplay вместо aplay."; then
    if command -v paplay >/dev/null 2>&1; then
        paplay --raw --rate="$PIPER_RATE" --channels=1 --format=s16le \
               --device="$SINK" "$SRC"
    else
        echo "    paplay не установлен — прогон пропущен."
    fi
fi

cat <<'EOF'

=== Как читать результат ===
  2 чище 1                  -> виноват пересчёт частоты. Лечится тем, что кэш
                               собирается сразу в 44100 (правка на две строки
                               в build_voice_cache.sh и say.sh).
  3 чисто, 1 рвётся         -> виноват старт потока на каждую фразу. Лечится
                               одним долгоживущим потоком в гарнитуру.
  4 чище 1                  -> виноват alsa-плагин pulse; say.sh переводится
                               на paplay.
  рвётся везде одинаково    -> тракт ни при чём, это радио. Дальше проверять
                               wifi (см. TESTING.md, раздел Аудио/Bluetooth).
EOF
