#!/bin/bash
# Где именно рвётся звук в наушниках: до радио или в радио.
#
# Этот скрипт задаёт не тот вопрос, что audio_ab_test.sh. Тот сравнивал
# способы доставки и уже ответил: помехи есть на всех, значит виноват не
# формат, не ресемплер и не alsa-плагин. Полевые данные к тому же показали:
#   * монтажная дорожка, собранная ИЗ ТЕХ ЖЕ файлов кэша, звучит чисто —
#     значит записи целые;
#   * помехи каждый раз в разном месте фразы — значит это не данные, а
#     потери во времени;
#   * фактическая задержка синка гуляет 50000..110000 мкс при настроенных
#     68537 — то есть буфер то переполнен, то пуст.
#
# Остаётся ровно два подозреваемых, и различить их на слух нельзя:
#
#   А. Плата не успевает. PulseAudio недодаёт кодеру отсчёты, дырку он
#      заполняет тишиной. Тогда провал слышен в наушниках И виден в том, что
#      сервер отдаёт синку.
#   Б. Радио теряет пакеты. Сервер отдал всё вовремя, до наушников доехало не
#      всё. Тогда провал слышен в наушниках, но НЕ виден на выходе сервера.
#
# Разделяет их запись монитора синка. Монитор — это копия потока в точке
# ПЕРЕД кодированием и передачей: всё, что случилось дальше, в него не
# попадает. Ушей для этой проверки не нужно вовсе, ответ читается числами.
#
# Прогон 1 отвечает на вопрос А/Б. Прогоны 2 и 3 — это уже лечение: размер
# буфера против А, глушение wifi против Б.
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

# Формат монитора обязан совпадать с форматом синка, иначе сервер вставит в
# запись свой пересчёт и мы будем искать дырки в его артефактах.
SINK_SPEC=$(pactl list sinks 2>/dev/null | grep -A 20 "Name: $SINK" \
            | grep -m1 "Sample Specification:")
SINK_RATE=$(echo "$SINK_SPEC" | grep -o '[0-9]*Hz' | tr -d 'Hz')
SINK_CH=$(echo "$SINK_SPEC" | grep -o '[0-9]*ch' | tr -d 'ch')
SINK_RATE=${SINK_RATE:-44100}
SINK_CH=${SINK_CH:-2}

TMP=$(mktemp -d /dev/shm/drop_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

echo "=== Обстановка ==="
echo "Файл:    $SRC ($(stat -c %s "$SRC") байт, $(awk -v b="$(stat -c %s "$SRC")" -v r="$PIPER_RATE" 'BEGIN{printf "%.1f", b/2/r}') с)"
echo "Синк:    $SINK ($SINK_RATE Гц, $SINK_CH кан.)"
echo "Монитор: $SINK.monitor"
echo

announce() {
    echo
    echo ">>> $1"
    echo "    (Enter — выполнять, s — пропустить)"
    read -r -n 1 key
    echo
    [ "$key" = "s" ] && return 1
    return 0
}

# --- 1. Что сервер отдаёт в синк ------------------------------------------
# Главный прогон. Слушать при нём тоже надо — сравниваются именно уши и
# числа, — но вывод делается по числам.
if announce "Прогон 1 из 3. ЗАПИСЬ ВЫХОДА СЕРВЕРА. Слушайте фразу и запомните, были ли помехи."; then
    if command -v parec >/dev/null 2>&1; then
        parec --device="$SINK.monitor" --rate="$SINK_RATE" --channels="$SINK_CH" \
              --format=s16le --raw > "$TMP/mon.raw" 2>"$TMP/parec.err" &
    else
        ffmpeg -nostdin -hide_banner -loglevel error -y \
               -f pulse -i "$SINK.monitor" \
               -ar "$SINK_RATE" -ac "$SINK_CH" -f s16le "$TMP/mon.raw" \
               2>"$TMP/parec.err" &
    fi
    REC=$!
    # Полсекунды на раскрутку записи: монитор поднимается не мгновенно, и без
    # запаса начало фразы просто не попадёт в файл — а мы примем это за провал.
    sleep 0.5
    aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$SRC" 2>/dev/null
    RC=$?
    sleep 0.5
    kill "$REC" 2>/dev/null
    wait "$REC" 2>/dev/null

    echo "    aplay вернул $RC, записано $(stat -c %s "$TMP/mon.raw" 2>/dev/null || echo 0) байт"
    [ -s "$TMP/parec.err" ] && sed 's/^/    запись: /' "$TMP/parec.err"

    python3 - "$TMP/mon.raw" "$SINK_RATE" "$SINK_CH" "$SRC" "$PIPER_RATE" <<'ANALYZE'
import sys
import numpy as np

mon, rate, ch, src, src_rate = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
d = np.fromfile(mon, dtype=np.int16)
if d.size < rate:
    print("    Запись не удалась — монитор дал меньше секунды. Разбор невозможен.")
    raise SystemExit
if ch > 1:
    d = d[: d.size // ch * ch].reshape(-1, ch)[:, 0]

# Речь ищется по кадрам в 10 мс: так дырка в один период буфера не тонет в
# средней громкости всей фразы.
frame = rate // 100
n = d.size // frame
f = d[: n * frame].reshape(n, frame).astype(np.int32)
peak = np.abs(f).max(axis=1)
loud = peak > max(peak.max() // 50, 8)
if not loud.any():
    print("    В записи нет звука вообще: монитор писал тишину, пока aplay играл.")
    print("    Это уже ответ — поток не дошёл даже до синка.")
    raise SystemExit

first, last = int(np.flatnonzero(loud)[0]), int(np.flatnonzero(loud)[-1])
span = (last - first + 1) * frame / rate
src_len = np.fromfile(src, dtype=np.int16).size / src_rate
print(f"    длительность в мониторе {span:.2f} с, в файле {src_len:.2f} с")

# Недобор буфера PulseAudio заполняет ТОЧНЫМИ нулями. В речи точный ноль
# подряд десятками отсчётов не встречается, поэтому такой участок внутри
# фразы — провал, а не пауза.
def zero_runs(x, sr):
    """Начала и длины участков точного нуля длиннее 5 мс."""
    zero = x == 0
    if not zero.any():
        return np.empty(0, dtype=int), np.empty(0, dtype=int)
    edges = np.diff(np.concatenate(([0], zero.view(np.int8), [0])))
    starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
    runs = ends - starts
    keep = runs > sr // 200
    return starts[keep], runs[keep]

inside = d[first * frame : (last + 1) * frame]
starts, runs = zero_runs(inside, rate)
holes, hole_ms = runs.size, float(runs.sum()) * 1000 / rate

# Тот же счёт по исходному файлу. Без него скрипт объявил бы провалом паузу,
# которая была записана в кэш ещё синтезатором: у piper между предложениями
# бывает ровная тишина, и в мониторе она выглядит точно так же, как недобор
# буфера. Виновата доставка только тогда, когда дырок стало БОЛЬШЕ.
_, src_runs = zero_runs(np.fromfile(src, dtype=np.int16), src_rate)
print(f"    провалов внутри фразы: {holes} (суммарно {hole_ms:.0f} мс),"
      f" в самом файле было {src_runs.size}")
if holes:
    where = [f"{s / rate:.2f}" for s in starts[:8]]
    print("    первые провалы на секундах:", ", ".join(where))

print()
if holes > src_runs.size or span > src_len * 1.05:
    print("    ВЫВОД: рвётся ДО радио. Сервер сам недодаёт отсчёты — это")
    print("    нехватка буфера или вытеснение процесса, а не Bluetooth.")
    print("    Лечится прогоном 2 (размер буфера).")
else:
    print("    ВЫВОД: сервер отдаёт поток ЦЕЛЫМ. Если в наушниках при этом")
    print("    были помехи — теряет радио, и буфер тут не поможет.")
    print("    Лечится прогоном 3 (эфир) и разносом с wifi.")
ANALYZE
fi

# --- 2. Размер буфера -----------------------------------------------------
# Лечение случая А. alsa-плагин pulse читает PULSE_LATENCY_MSEC и просит у
# сервера буфер такого размера. Сейчас синк настроен на 68 мс — это мало для
# контроллера на UART: любая задержка планировщика длиннее 68 мс мгновенно
# опустошает буфер. Плата за увеличение честная и её надо знать: на столько же
# позже начнёт звучать предупреждение. Поэтому берётся не максимум, а самое
# маленькое значение, на котором чисто.
if announce "Прогон 2 из 3. РАЗМЕР БУФЕРА: та же фраза при 68 (как сейчас), 200, 500 и 1000 мс."; then
    for ms in 200 500 1000; do
        echo "    -- буфер $ms мс --"
        PULSE_LATENCY_MSEC=$ms \
            aplay -D "$DEV" -r "$PIPER_RATE" -f S16_LE -t raw -c 1 "$SRC" 2>/dev/null
        echo "       код $?"
        sleep 1
    done
    echo "    Запомните наименьшее значение, на котором помех уже нет."
fi

# --- 3. Эфир --------------------------------------------------------------
# Лечение случая Б. Контроллер сидит на UART (hciconfig: Bus: UART) — это
# встроенный совмещённый чип, у которого Bluetooth и wifi делят одну антенну
# и один диапазон 2.4 ГГц. Классический итог — ровно такие помехи: короткие,
# в случайных местах, независимые от формата и громкости.
#
# Проверка обязана быть автономной. Связь с платой идёт по тому самому wifi,
# который надо выключить: команда, набранная в ssh, оборвётся вместе с
# сессией и наушники останутся молчать. Поэтому вся проверка отдаётся systemd
# отдельным юнитом — он переживает разрыв ssh и сам вернёт wifi обратно.
if announce "Прогон 3 из 3. ЭФИР: пять фраз с выключенным wifi. Связь с платой прервётся на минуту — это ожидаемо."; then
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
  1 показал провалы,        -> недобор буфера. Смотреть прогон 2: если на
    в наушниках помехи         200-500 мс чисто, лечится одной строкой
                               PULSE_LATENCY_MSEC в tools/say.sh.
  1 показал ноль провалов,  -> теряет радио. Буфер не поможет. Смотреть
    в наушниках помехи         прогон 3: чисто без wifi -> это соседство с
                               wifi на одной антенне. Лечится переводом точки
                               доступа на 5 ГГц либо работой прогулки без wifi.
  1 показал ноль провалов,  -> и радио цело. Тогда остаются сами наушники:
    и в прогоне 3 помехи       проверить их на телефоне той же фразой.
  2 чисто на всех значениях -> помеха не в буфере; вывод делает прогон 3.
EOF
