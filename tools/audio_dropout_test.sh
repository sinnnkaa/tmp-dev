#!/bin/bash
# Почему звук в наушниках плохой ВСЕГДА: измерение канала и проверка обхода.
#
# Скрипт больше не ищет виновника наугад — искать почти нечего. Что закрыто
# полевыми прогонами 22-23.08.2026 и повторять не нужно:
#
#   * записи в кэше целые. Монтажная дорожка собрана из ТЕХ ЖЕ файлов и
#     звучит чисто;
#   * сервер отдаёт поток в синк целым. Монитор синка совпал с эталоном
#     по длине и по числу участков тишины;
#   * эфир ни при чём. RSSI -1, Link quality 255 (это максимум), и два
#     прогона с выключенным wifi дали ровно тот же счёт, что с включённым;
#   * наушники ни при чём. Marshall Major IV на телефоне играют музыку без
#     нареканий;
#   * буфер не лечит. 68 мс — 0 попыток из 5 чисто, 500 мс — 2 из 5,
#     1000 мс снова плохо. Немонотонность означает упор не в запас времени,
#     а в пропускную способность.
#
# Что измерено и что это значит. За 4.63 секунды проигрывания контроллер
# передал 44731 байт — это 77 кбит/с. Минимум для SBC 44100 стерео это 229
# кбит/с (bitpool 35), обычный режим — 328 (bitpool 53). То есть в наушники
# уходит треть нужного потока: не "иногда рвётся", а постоянно играет
# сильно пережатый звук. Это и слышно как хрип и "зажевало".
#
# Откуда взялась треть. Контроллер сидит на UART (`Bus: UART`,
# hci_uart_bcm на /serial@fe650000/bluetooth). Линия 8N1 отдаёт бод*0,8:
# при 115200 это 92 кбит/с, и измеренные 77 — уже 84% занятости. При этом
# `max-speed` в device tree не нашёлся ни в одном узле, а без него драйвер
# остаётся на начальной скорости и никогда её не поднимает. Когда запись в
# транспорт начинает отказывать, PulseAudio сам понижает bitpool, чтобы
# влезть в канал, — и получается ровно наблюдаемая картина.
#
# Прогон 1 подтверждает это по журналу сервера (строки о смене bitpool).
# Прогон 2 — кандидат на лечение без пересборки ядра: HFP вместо A2DP.
#
# Использование: audio_dropout_test.sh [файл_из_кэша.raw]
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/voice_env.sh"

cache_utterances() {
    ls -S "$VOICE_CACHE_DIR"/*.raw 2>/dev/null | grep -E '/[0-9a-f]{40}\.raw$'
}

SRC="${1:-}"
[ -n "$SRC" ] || SRC=$(cache_utterances | head -1)
if [ ! -s "${SRC:-}" ]; then
    echo "Не нашёл готовых реплик в $VOICE_CACHE_DIR — соберите кэш:" >&2
    echo "    tools/build_voice_cache.sh" >&2
    exit 1
fi

DEV=$(voice_playback_device)
SINK=${DEV#pulse:}
if [ "$DEV" = "default" ]; then
    echo "Гарнитуры нет: PulseAudio не показывает ни одного bluez-синка." >&2
    echo "Включите наушники и дождитесь bt_keeper: journalctl -u bt_keeper -f" >&2
    exit 1
fi
CARD=$(pactl list cards short 2>/dev/null | awk -v m="$BLINDNAV_BT_MAC_UND" '$2 ~ m {print $2; exit}')
SRC_SECONDS=$(awk -v b="$(stat -c %s "$SRC")" -v r="$PIPER_RATE" 'BEGIN{printf "%.2f", b/2/r}')

TMP=$(mktemp -d /dev/shm/drop_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# Состояние синка. Через awk по блокам, а не `grep -A` после имени: в выводе
# `pactl list sinks` строка State идёт ПЕРЕД Name, поэтому поиск после имени
# не находил ничего и в поле состояние печаталось пустым все три раза. Ошибка
# безобидная на вид, но она ровно того сорта, ради которого затевался весь
# разбор: диагностика молча показывала пустоту вместо ответа.
sink_state() {
    pactl list sinks 2>/dev/null | awk -v n="$1" '
        /^Sink #/ { st = "" }
        /State:/  { st = $2 }
        index($0, "Name: " n) { print st; exit }'
}

sink_spec() {
    local sp
    sp=$(pactl list sinks 2>/dev/null | grep -A 20 "Name: $1" | grep -m1 "Sample Specification:")
    echo "$(echo "$sp" | grep -o '[0-9]*Hz' | tr -d 'Hz') $(echo "$sp" | grep -o '[0-9]*ch' | tr -d 'ch')"
}
read -r SINK_RATE SINK_CH <<<"$(sink_spec "$SINK")"
SINK_RATE=${SINK_RATE:-44100}
SINK_CH=${SINK_CH:-2}

# Счётчик переданных байт у самого контроллера. Единственная цифра во всей
# истории, которая не зависит ни от ушей, ни от моих догадок.
hci_tx() { hciconfig hci0 2>/dev/null | grep -o 'TX bytes:[0-9]*' | cut -d: -f2; }

# Общая мерка: сыграть файл в устройство и сказать, с какой скоростью это
# ушло в эфир. Одинаковая для A2DP и для HFP, иначе их не сравнить.
measure_play() {
    local dev=$1 rate=$2 ch=$3 file=$4
    local before after start end
    before=$(hci_tx); start=$(date +%s.%N)
    aplay -D "$dev" -r "$rate" -f S16_LE -t raw -c "$ch" "$file" 2>/dev/null
    local rc=$?
    end=$(date +%s.%N); after=$(hci_tx)
    awk -v b="${before:-0}" -v a="${after:-0}" -v s="$start" -v e="$end" -v rc="$rc" '
    BEGIN {
        dt = e - s; db = a - b
        if (dt <= 0 || db <= 0) { printf "       код %d, измерить не вышло\n", rc; exit }
        printf "       код %d, играли %.2f с, передано %d байт => %.0f кбит/с\n",
               rc, dt, db, db * 8 / dt / 1000
    }'
}

echo "=== Обстановка ==="
echo "Файл: $SRC ($SRC_SECONDS с)"
echo "Синк: $SINK ($SINK_RATE Гц, $SINK_CH кан.)"
echo "Карта: ${CARD:-не найдена}"
echo
echo "-- канал до контроллера --"
hciconfig -a 2>/dev/null | grep -E "^hci|Bus:|Type:|ACL MTU" | sed 's/^/   /'
echo "   -- узел bluetooth в device tree --"
# Прошлый поиск по именам max-speed/current-speed не нашёл ничего, и это само
# по себе улика: без max-speed драйвер hci_uart остаётся на начальной
# скорости. Но "не нашлось" могло значить и "искал не там", поэтому теперь
# узел показывается целиком, со всеми свойствами.
BT_NODE=$(find /proc/device-tree -maxdepth 4 -type d -name 'bluetooth*' 2>/dev/null | head -1)
if [ -n "$BT_NODE" ]; then
    echo "   $BT_NODE"
    for f in "$BT_NODE"/* "$(dirname "$BT_NODE")"/current-speed "$(dirname "$BT_NODE")"/clock-frequency; do
        [ -f "$f" ] || continue
        # Свойства бывают строками и числами u32 big-endian. Показываем и то и
        # другое: гадать по имени нельзя, а пропустить скорость — потерять ответ.
        num=$(od -An -tu4 --endian=big "$f" 2>/dev/null | tr -d ' ')
        txt=$(tr -d '\0' < "$f" | tr -c '[:print:]' ' ')
        printf '      %-24s %s\n' "$(basename "$f")" "${num:-$txt}"
    done
else
    echo "   узел не найден — device tree недоступен"
fi
echo "   -- качество линка (RSSI около нуля и LQ 255 — идеально) --"
hcitool rssi "$BLINDNAV_BT_MAC" 2>&1 | sed 's/^/   /'
hcitool lq "$BLINDNAV_BT_MAC" 2>&1 | sed 's/^/   /'
echo

announce() {
    echo
    echo ">>> $1"
    echo "    (Enter — выполнять, s — пропустить)"
    read -r key
    [ "$key" = "s" ] && return 1
    return 0
}

# --- 1. Сколько на самом деле уходит и каким bitpool ----------------------
# Ушей не требует. Скорость показывает, сколько влезает в канал; журнал
# сервера показывает, знает ли об этом сам PulseAudio. Строка вида
# "Changing SBC bitpool" — прямое доказательство того, что кодек ужимают
# под канал, а не что он таким выбран.
if announce "Прогон 1 из 4. СКОРОСТЬ И BITPOOL. Слушать не нужно."; then
    LOGSET=""
    if pacmd set-log-level 4 >/dev/null 2>&1; then LOGSET=pacmd; fi
    SINCE=$(date '+%Y-%m-%d %H:%M:%S')
    echo "    -- одна фраза --"
    measure_play "$DEV" "$PIPER_RATE" 1 "$SRC"
    echo "    -- длинный поток (десять реплик подряд) --"
    # Долгий поток нужен затем, что на короткой фразе скорость смазывается
    # разгоном. Если и здесь та же цифра — это потолок, а не переходный режим.
    : > "$TMP/long.raw"
    cache_utterances | head -10 | while read -r f; do cat "$f" >> "$TMP/long.raw"; done
    measure_play "$DEV" "$PIPER_RATE" 1 "$TMP/long.raw"
    if [ -n "$LOGSET" ]; then
        echo "    -- что сервер писал про кодек --"
        journalctl --since "$SINCE" --no-pager 2>/dev/null \
            | grep -iE "bitpool|sbc|a2dp|write error|EAGAIN" | tail -12 | sed 's/^/    /'
        pacmd set-log-level 1 >/dev/null 2>&1
    else
        echo "    (pacmd недоступен — уровень журнала не поднять, идём по скорости)"
    fi
    awk 'BEGIN{
        print ""
        print "    Ориентиры: SBC 44100 стерео это 229 кбит/с при bitpool 35 и"
        print "    328 при bitpool 53. Линия UART 8N1 отдаёт бод*0,8:"
        print "    115200 бод -> 92 кбит/с, 460800 -> 369, 1500000 -> 1200."
    }'
fi

# --- 2. HFP вместо A2DP ---------------------------------------------------
# Кандидат на лечение, доступный прямо сейчас, без правки device tree и без
# перезагрузки. Логика простая: если в канал не влезает музыкальный поток —
# не надо посылать музыкальный поток. Устройство озвучивает ТОЛЬКО речь.
# Профиль HFP для речи и сделан: mSBC это 16 кГц моно, порядка 64 кбит/с —
# втрое меньше, чем просит A2DP, и в 92 кбит/с оно помещается целиком.
#
# Гарнитура уже умеет этот профиль: демон переводит её в HFP на время
# диктовки (switch_bt_profile в voice_nav_daemon.py). Здесь проверяется
# обратное направление — как в нём звучит воспроизведение.
#
# Оговорка, которую надо знать заранее: у части контроллеров Broadcom звук
# SCO идёт не по UART, а отдельной шиной PCM/I2S. Если так, HFP либо будет
# идеально чистым, либо не зазвучит вовсе — и то и другое информативно.
if [ -z "$CARD" ]; then
    echo
    echo ">>> Прогон 2 пропущен: карта гарнитуры не найдена в pactl."
elif announce "Прогон 2 из 4. HFP ВМЕСТО A2DP. Слушайте: речь станет глуше (16 кГц), но должна быть ЧИСТОЙ."; then
    if pactl set-card-profile "$CARD" handsfree_head_unit 2>&1 | sed 's/^/    /'; then
        sleep 2
        HFP_SINK=$(pactl list sinks short 2>/dev/null \
                   | awk -F'\t' '$2 ~ /handsfree/ {print $2; exit}')
        if [ -z "$HFP_SINK" ]; then
            echo "    Синка HFP не появилось — профиль не поднялся."
        else
            read -r HR HC <<<"$(sink_spec "$HFP_SINK")"
            HR=${HR:-16000}; HC=${HC:-1}
            echo "    Синк HFP: $HFP_SINK ($HR Гц, $HC кан.)"
            # Пересчёт делает ffmpeg, а не сервер: разбирается качество
            # канала, и мешать в него ещё и ресемплер сервера незачем.
            if ffmpeg -nostdin -hide_banner -loglevel error -y \
                      -f s16le -ar "$PIPER_RATE" -ac 1 -i "$SRC" \
                      -ar "$HR" -ac "$HC" -f s16le "$TMP/hfp.raw"; then
                measure_play "pulse:$HFP_SINK" "$HR" "$HC" "$TMP/hfp.raw"
                echo "    -- та же фраза второй раз --"
                measure_play "pulse:$HFP_SINK" "$HR" "$HC" "$TMP/hfp.raw"
            fi
        fi
        echo "    Возвращаю A2DP."
        pactl set-card-profile "$CARD" a2dp_sink 2>&1 | sed 's/^/    /'
    fi
fi

# --- 3. Холодный старт против тёплого (уже отвечен) -----------------------
# Версия была такая: module-suspend-on-idle усыпляет синк, а для bluez сон
# означает разбор A2DP-транспорта, и следующая фраза начинается с переговоров
# AVDTP поверх первых долей секунды речи.
#
# ПРОВЕРЕНО 23.08.2026, НЕ ПОДТВЕРДИЛОСЬ: начало чистое и после двенадцати
# секунд простоя, и под фоновой тишиной. Причина видна там же — строка про
# suspend-on-idle пустая, модуль просто не загружен, и синку нечем засыпать.
# Прогон оставлен на случай, если модуль появится после обновления системы.
if announce "Прогон 3 из 4. ХОЛОДНЫЙ И ТЁПЛЫЙ СТАРТ (уже отвечен: разницы нет, можно пропустить)."; then
    echo "    -- как настроен сон синка --"
    if pactl list short modules 2>/dev/null | grep suspend-on-idle | sed 's/^/       /' | grep -q .; then
        echo "       (без аргумента timeout сервер усыпляет через 5 секунд)"
    else
        # Пустой grep выглядел как "всё в порядке", а означал обратное:
        # проверять нечего. Молчание тут — тоже ответ, и его надо назвать.
        echo "       модуль НЕ ЗАГРУЖЕН — синк не засыпает, разницы не будет"
    fi
    echo "    -- А: холодный старт (даём синку уснуть) --"
    for i in $(seq 12); do
        sleep 1
        printf '\r       ожидание %2d с, состояние синка: %-10s' "$i" \
            "$(sink_state "$SINK")"
    done
    echo
    measure_play "$DEV" "$PIPER_RATE" 1 "$SRC"
    echo "       Начало было чистым?"
    echo "    -- Б: тёплый старт (фоном идёт тишина, синк не засыпает) --"
    aplay -D "$DEV" -r "$SINK_RATE" -f S16_LE -t raw -c "$SINK_CH" /dev/zero 2>/dev/null &
    KEEP=$!
    sleep 3
    printf '       состояние синка: %s\n' \
        "$(sink_state "$SINK")"
    measure_play "$DEV" "$PIPER_RATE" 1 "$SRC"
    echo "       А теперь начало было чистым?"
    measure_play "$DEV" "$PIPER_RATE" 1 "$SRC"
    echo "       И вторая подряд?"
    kill "$KEEP" 2>/dev/null
    wait "$KEEP" 2>/dev/null
fi

# --- 4. Монитор синка (повтор) --------------------------------------------
# Уже отвечено: поток целый. Прогон оставлен как проверка на возврат
# поломки, а не как поиск — при разборе его можно пропускать.
if announce "Прогон 4 из 4. МОНИТОР СИНКА (уже отвечено, можно пропустить)."; then
    if ffmpeg -nostdin -hide_banner -loglevel error -y \
              -f s16le -ar "$PIPER_RATE" -ac 1 -i "$SRC" \
              -ar "$SINK_RATE" -ac "$SINK_CH" -f s16le "$TMP/ref.raw"; then
        parec --device="$SINK.monitor" --rate="$SINK_RATE" --channels="$SINK_CH" \
              --format=s16le --raw > "$TMP/mon.raw" 2>/dev/null &
        REC=$!
        sleep 0.5
        aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$SRC" 2>/dev/null
        sleep 0.5
        kill "$REC" 2>/dev/null; wait "$REC" 2>/dev/null
        python3 - "$TMP/mon.raw" "$TMP/ref.raw" "$SINK_RATE" "$SINK_CH" <<'ANALYZE'
import sys
import numpy as np

mon, ref, rate, ch = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])


def mono(path):
    x = np.fromfile(path, dtype=np.int16)
    if ch > 1:
        x = x[: x.size // ch * ch].reshape(-1, ch)[:, 0]
    return x


def speech_span(x):
    """Границы звучащей части в кадрах по 10 мс."""
    frame = rate // 100
    n = x.size // frame
    if n == 0:
        return None
    f = x[: n * frame].reshape(n, frame).astype(np.int32)
    peak = np.abs(f).max(axis=1)
    loud = peak > max(peak.max() // 50, 8)
    if not loud.any():
        return None
    idx = np.flatnonzero(loud)
    return int(idx[0]) * frame, (int(idx[-1]) + 1) * frame


def zero_runs(x):
    """Участки ТОЧНОГО нуля длиннее 5 мс: так выглядит недобор буфера."""
    zero = x == 0
    if not zero.any():
        return np.empty(0, dtype=int)
    edges = np.diff(np.concatenate(([0], zero.view(np.int8), [0])))
    runs = np.flatnonzero(edges == -1) - np.flatnonzero(edges == 1)
    return runs[runs > rate // 200]


m, r = mono(mon), mono(ref)
ms, rs = speech_span(m), speech_span(r)
if ms is None or rs is None:
    print("    Сравнивать нечего: в мониторе или в эталоне нет звука.")
    raise SystemExit
m_len, r_len = (ms[1] - ms[0]) / rate, (rs[1] - rs[0]) / rate
m_runs, r_runs = zero_runs(m[ms[0]:ms[1]]), zero_runs(r[rs[0]:rs[1]])
print(f"    звучащая часть {m_len:.2f} с против {r_len:.2f} с,"
      f" участков тишины {m_runs.size} против {r_runs.size}")
if m_runs.size > r_runs.size or m_len < r_len * 0.97 or m_len > r_len * 1.05:
    print("    ИЗМЕНЕНИЕ: сервер стал недодавать отсчёты, чего раньше не было.")
else:
    print("    Как и было: сервер отдаёт поток целым.")
ANALYZE
    fi
fi

cat <<'EOF'

=== Как читать результат ===
  1: та же скорость на        -> это потолок канала, а не разгон. Дальше
     длинном потоке, что и        либо HFP (прогон 2), либо max-speed в
     на короткой фразе            device tree и пересборка dtb.
  1: в журнале есть смена     -> подтверждение: сервер сам ужимает кодек,
     bitpool вниз                 чтобы влезть в канал.
  2: HFP звучит чисто         -> лечение найдено и оно бесплатное. Речи
                                  хватает 16 кГц моно, а музыку устройство
                                  не играет. Перевести озвучку на HFP:
                                  say.sh выбирает синк handsfree_head_unit,
                                  bt_keeper держит профиль.
  2: HFP молчит               -> SCO на этом контроллере идёт мимо HCI
                                  (PCM/I2S) и в PulseAudio не заведён.
                                  Тогда остаётся device tree.
  2: HFP так же плохо         -> дело не в объёме потока. Единственное
                                  оставшееся — скорость UART.
  3: А и Б одинаковы          -> так и есть, проверено. suspend-on-idle не
                                  загружен, транспорт из сна не поднимается,
                                  и к делу это отношения не имеет.
EOF
