#!/bin/bash
# Где рвётся звук в наушниках: до радио, в радио или в самом канале до
# контроллера Bluetooth.
#
# Чем этот скрипт отличается от audio_ab_test.sh: тот сравнивал СПОСОБЫ
# доставки и своё отработал — помехи одинаковы на всех, значит виноват не
# формат, не ресемплер и не alsa-плагин. Дальше нужны не уши, а измерения.
#
# Что уже установлено полевыми прогонами и больше не проверяется:
#   * монтажная дорожка собрана из ТЕХ ЖЕ файлов кэша и звучит чисто —
#     записи целые, версия про испорченный кэш закрыта;
#   * помехи каждый раз в разном месте фразы — это потери во времени, а не
#     в данных;
#   * с выключенным wifi помехи остаются — соседство по эфиру не виновато;
#   * буфер 500 мс лучше, чем 68, но 1000 мс НЕ лучше 500.
#
# Последний пункт и задаёт нынешнюю версию. При нехватке буфера помогает
# любое увеличение, и чем больше, тем лучше. Немонотонность означает другое:
# упёрлись не в запас, а в пропускную способность. А самое узкое место здесь
# видно в `hciconfig -a`: контроллер сидит не на USB, а на UART. Скорость
# этого последовательного канала задаётся в device tree один раз, и если она
# мала, A2DP в неё физически не влезает — 44100 стерео SBC это около 230
# кбит/с плюс накладные HCI. Симптом ровно такой, каким вы его описали:
# короткая фраза иногда проходит, длинную "зажёвывает", и ни буфер, ни
# глушение wifi не помогают.
#
# Прогон 2 измеряет это без ушей, по счётчикам самого контроллера.
#
# Использование: audio_dropout_test.sh [файл_из_кэша.raw]
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/voice_env.sh"

# Отбор реплик тот же, что в audio_ab_test.sh: имя боевого файла — sha1 от
# текста, сорок шестнадцатеричных знаков. Без фильтра `ls -S` вытаскивает
# killtest_long.raw, огрызок от старых проверок вытеснения.
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

SINK_SPEC=$(pactl list sinks 2>/dev/null | grep -A 20 "Name: $SINK" \
            | grep -m1 "Sample Specification:")
SINK_RATE=$(echo "$SINK_SPEC" | grep -o '[0-9]*Hz' | tr -d 'Hz')
SINK_CH=$(echo "$SINK_SPEC" | grep -o '[0-9]*ch' | tr -d 'ch')
SINK_RATE=${SINK_RATE:-44100}
SINK_CH=${SINK_CH:-2}
SRC_SECONDS=$(awk -v b="$(stat -c %s "$SRC")" -v r="$PIPER_RATE" 'BEGIN{printf "%.2f", b/2/r}')

TMP=$(mktemp -d /dev/shm/drop_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

echo "=== Обстановка ==="
echo "Файл:    $SRC ($(stat -c %s "$SRC") байт, $SRC_SECONDS с)"
echo "Синк:    $SINK ($SINK_RATE Гц, $SINK_CH кан.)"
echo

echo "-- канал до контроллера --"
# Главное, на что смотреть. UART — это последовательный провод с одной раз и
# навсегда заданной скоростью, и никакая настройка PulseAudio её не поднимет.
hciconfig -a 2>/dev/null | grep -E "^hci|Bus:|Type:|ACL MTU" | sed 's/^/   /'
FOUND_SPEED=""
for f in $(find /proc/device-tree -name 'max-speed' -o -name 'current-speed' 2>/dev/null); do
    # Значение лежит big-endian u32. od --endian есть в coreutils; если его
    # нет, считаем через hexdump, а не молчим.
    v=$(od -An -tu4 --endian=big "$f" 2>/dev/null | tr -d ' ')
    [ -n "$v" ] || v=$((0x$(hexdump -v -e '4/1 "%02x"' "$f" 2>/dev/null)))
    printf '   %-56s %s бод\n' "${f#/proc/device-tree/}" "$v"
    case "$f" in *bluetooth*|*bt*) FOUND_SPEED="$v";; esac
done
[ -n "$FOUND_SPEED" ] || echo "   (скорость UART в device tree не нашлась — смотрите dmesg ниже)"
echo "   -- жалобы драйвера --"
# hci_uart ругается на переполнение и на битые кадры. Если такие строки есть,
# искать больше нечего.
dmesg 2>/dev/null | grep -iE "hci_uart|Bluetooth:.*(error|fail|timeout|overrun|reassembl)" \
    | tail -8 | sed 's/^/   /' || true
echo "   -- качество линка --"
# Слабый сигнал даёт те же прерывания, что и узкий канал, и различать их надо
# до того, как крутить настройки. RSSI около нуля — норма, -20 и ниже — далеко.
hcitool rssi "$BLINDNAV_BT_MAC" 2>&1 | sed 's/^/   /'
hcitool lq "$BLINDNAV_BT_MAC" 2>&1 | sed 's/^/   /'
echo "   -- выбранный кодек --"
journalctl -u bluetooth --no-pager -n 2000 2>/dev/null \
    | grep -iE "bitpool|SBC|codec" | tail -5 | sed 's/^/   /' || true
echo

announce() {
    echo
    echo ">>> $1"
    echo "    (Enter — выполнять, s — пропустить)"
    read -r key
    [ "$key" = "s" ] && return 1
    return 0
}

# Эталон для сравнения: тот же файл, приведённый к формату синка тем же
# способом, каким его приводит сервер. Без этого шага разбор врал дважды.
# Первое: длительность в мониторе сравнивалась с ПОЛНОЙ длиной файла, хотя в
# мониторе меряется участок от первого до последнего громкого кадра — у
# записи piper по краям есть тишина, и 4.12 против 4.56 выглядело потерей,
# не будучи ей. Второе: участки точного нуля считались в исходнике на 22050
# моно, а в мониторе — после пересчёта в 44100 стерео, где ресемплер эти
# нули размазывает. Счёт получался несопоставимый, и "в файле было 2, в
# мониторе 1" ничего не доказывало.
make_reference() {
    ffmpeg -nostdin -hide_banner -loglevel error -y \
           -f s16le -ar "$PIPER_RATE" -ac 1 -i "$SRC" \
           -ar "$SINK_RATE" -ac "$SINK_CH" -f s16le "$TMP/ref.raw"
}

# --- 1. Что сервер отдаёт в синк ------------------------------------------
# Монитор синка — копия потока в точке ПЕРЕД кодированием и передачей. Всё,
# что случилось дальше (UART, эфир, наушники), в него не попадает.
if announce "Прогон 1 из 4. ЗАПИСЬ ВЫХОДА СЕРВЕРА. Слушайте фразу и запомните, были ли помехи."; then
    if ! make_reference; then
        echo "    ffmpeg не справился с эталоном — прогон пропущен."
    else
    if command -v parec >/dev/null 2>&1; then
        parec --device="$SINK.monitor" --rate="$SINK_RATE" --channels="$SINK_CH" \
              --format=s16le --raw > "$TMP/mon.raw" 2>"$TMP/rec.err" &
    else
        ffmpeg -nostdin -hide_banner -loglevel error -y \
               -f pulse -i "$SINK.monitor" \
               -ar "$SINK_RATE" -ac "$SINK_CH" -f s16le "$TMP/mon.raw" 2>"$TMP/rec.err" &
    fi
    REC=$!
    # Полсекунды на раскрутку записи: монитор поднимается не мгновенно, и без
    # запаса начало фразы не попадёт в файл, а мы примем это за провал.
    sleep 0.5
    aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$SRC" 2>/dev/null
    echo "    aplay вернул $?, записано $(stat -c %s "$TMP/mon.raw" 2>/dev/null || echo 0) байт"
    sleep 0.5
    kill "$REC" 2>/dev/null
    wait "$REC" 2>/dev/null
    [ -s "$TMP/rec.err" ] && sed 's/^/    запись: /' "$TMP/rec.err"

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
    """Границы звучащей части в кадрах по 10 мс.

    По кадрам, а не по всему сигналу: дырка в один период буфера тонет в
    средней громкости целой фразы и по ней не находится.
    """
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
    """Начала и длины участков ТОЧНОГО нуля длиннее 5 мс.

    Недобор буфера PulseAudio заполняет именно точными нулями; в речи такого
    подряд десятками отсчётов не бывает.
    """
    zero = x == 0
    if not zero.any():
        return np.empty(0, dtype=int), np.empty(0, dtype=int)
    edges = np.diff(np.concatenate(([0], zero.view(np.int8), [0])))
    starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
    runs = ends - starts
    keep = runs > rate // 200
    return starts[keep], runs[keep]


m, r = mono(mon), mono(ref)
ms, rs = speech_span(m), speech_span(r)
if ms is None:
    print("    В записи нет звука вообще: монитор писал тишину, пока aplay играл.")
    print("    Это уже ответ — поток не дошёл даже до синка.")
    raise SystemExit
if rs is None:
    print("    Эталон оказался беззвучным — сравнивать не с чем.")
    raise SystemExit

m_len, r_len = (ms[1] - ms[0]) / rate, (rs[1] - rs[0]) / rate
print(f"    звучащая часть: в мониторе {m_len:.2f} с, в эталоне {r_len:.2f} с")

m_starts, m_runs = zero_runs(m[ms[0]:ms[1]])
_, r_runs = zero_runs(r[rs[0]:rs[1]])
extra = m_runs.size - r_runs.size
print(f"    участков тишины: в мониторе {m_runs.size}"
      f" (суммарно {m_runs.sum() * 1000 / rate:.0f} мс),"
      f" в эталоне {r_runs.size} — лишних {max(extra, 0)}")
if m_runs.size:
    print("    они на секундах:", ", ".join(f"{s / rate:.2f}" for s in m_starts[:8]))

print()
if extra > 0 or m_len < r_len * 0.97 or m_len > r_len * 1.05:
    print("    ВЫВОД: рвётся ДО радио. Сервер сам недодаёт отсчёты — это")
    print("    нехватка буфера или вытеснение процесса, а не Bluetooth.")
else:
    print("    ВЫВОД: сервер отдаёт поток ЦЕЛЫМ, побайтово как в файле.")
    print("    Если в наушниках были помехи, теряется дальше: в канале до")
    print("    контроллера (прогон 2), в эфире (прогон 4) или в наушниках.")
ANALYZE
    fi
fi

# --- 2. Пропускная способность канала -------------------------------------
# Прогон без ушей, и сейчас самый важный. Контроллер сам считает переданные
# байты; разделив их на время, получаем фактическую скорость, с которой звук
# уходит в наушники. Сравнивать её надо с двумя числами: сколько нужно A2DP и
# сколько вообще способен отдать UART.
if announce "Прогон 2 из 4. ПРОПУСКНАЯ СПОСОБНОСТЬ. Слушать не нужно, вывод по счётчикам контроллера."; then
    hci_tx() { hciconfig hci0 2>/dev/null | grep -o 'TX bytes:[0-9]*' | cut -d: -f2; }
    BEFORE=$(hci_tx)
    START=$(date +%s.%N)
    aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$SRC" 2>/dev/null
    END=$(date +%s.%N)
    AFTER=$(hci_tx)
    if [ -z "$BEFORE" ] || [ -z "$AFTER" ]; then
        echo "    hciconfig не отдал счётчики TX — измерить нечем."
    else
        awk -v b="$BEFORE" -v a="$AFTER" -v s="$START" -v e="$END" \
            -v src="$SRC_SECONDS" -v baud="${FOUND_SPEED:-0}" '
        BEGIN {
            dt = e - s; dbytes = a - b
            printf "    играли %.2f с (сам файл %.2f с), передано %d байт\n", dt, src, dbytes
            if (dt <= 0 || dbytes <= 0) { print "    Измерение не удалось."; exit }
            kbps = dbytes * 8 / dt / 1000
            printf "    фактическая скорость: %.0f кбит/с\n", kbps
            printf "    нужно для SBC 44100 стерео: примерно 230-330 кбит/с\n"
            if (baud > 0) {
                # 8N1: на байт уходит десять бит линии.
                ceil = baud * 8 / 10 / 1000
                printf "    потолок UART при %d бод: %.0f кбит/с (занято %.0f%%)\n", baud, ceil, 100 * kbps / ceil
                if (kbps > ceil * 0.6)
                    print "    ВНИМАНИЕ: канал загружен больше чем наполовину — он и есть узкое место."
            }
            # Отставание времени проигрывания от длины файла — прямое
            # доказательство того, что поток не успевали забирать.
            if (dt > src * 1.08)
                printf "    ВНИМАНИЕ: играли на %.2f с дольше файла — поток тормозился.\n", dt - src
        }'
    fi
    echo "    -- ошибки контроллера после прогона --"
    hciconfig hci0 2>/dev/null | grep -E "RX bytes|TX bytes" | sed 's/^/    /'
fi

# --- 3. Буфер, по пять попыток --------------------------------------------
# Одна попытка на условие — это гадание: помехи случайны, и "500 нормально, а
# 1000 нет" при одном прогоне каждого объясняется простым везением. Поэтому
# каждое значение играется пять раз, а записывается не впечатление, а счёт.
if announce "Прогон 3 из 4. БУФЕР, ПО ПЯТЬ ПОПЫТОК. Считайте, сколько попыток из пяти прошло чисто."; then
    for ms in 68 500; do
        echo "    -- буфер $ms мс, пять попыток --"
        for i in 1 2 3 4 5; do
            printf '       попытка %d ... ' "$i"
            PULSE_LATENCY_MSEC=$ms \
                aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$SRC" 2>/dev/null
            echo "код $?"
            sleep 1
        done
        echo "       сколько из пяти было чисто? (запишите, скрипт этого не слышит)"
    done
fi

# --- 4. Эфир --------------------------------------------------------------
# Уже проверялось и помех не убрало, поэтому прогон оставлен, но последним:
# повторяют его только чтобы убедиться, что результат воспроизводится.
#
# Проверка обязана быть автономной. Связь с платой идёт по тому самому wifi,
# который надо выключить: команда, набранная в ssh, оборвётся вместе с
# сессией. Поэтому всё отдаётся systemd отдельным юнитом — он переживёт
# разрыв и сам вернёт wifi.
if announce "Прогон 4 из 4. ЭФИР (повтор): пять фраз с выключенным wifi. Связь прервётся на минуту."; then
    systemctl stop wifi-mute 2>/dev/null
    systemd-run --unit=wifi-mute --collect /bin/sh -c "
        rfkill block wifi
        sleep 3
        for i in 1 2 3 4 5; do
            $BLINDNAV_ROOT/tools/say.sh 'Проверка эфира без вайфая, фраза номер '\$i
            sleep 1
        done
        sleep 2
        rfkill unblock wifi
    " && echo "    Запущено. Слушайте наушники ~25 секунд, ssh сейчас отвалится."
    echo "    Wifi вернётся сам. Если связь не восстановилась за две минуты —"
    echo "    подключитесь по проводу и выполните: rfkill unblock wifi"
fi

cat <<'EOF'

=== Как читать результат ===
  2 показал загрузку UART    -> найдено. Канал до контроллера физически не
    больше половины             тянет A2DP, и настройки PulseAudio тут
                                бессильны. Лечится либо поднятием скорости
                                UART в device tree, либо снижением потока:
                                моно вместо стерео, меньший bitpool SBC.
  2 показал, что играли      -> то же самое другими словами: поток
    дольше самого файла         тормозился на выходе.
  2 в норме, 1 показал       -> недобор буфера, смотреть прогон 3.
    лишние участки тишины
  всё в норме, а помехи есть -> остались сами наушники. Проверить Marshall
                                на телефоне той же фразой: если и там
                                зажёвывает, плата ни при чём.
EOF
