#!/bin/bash
# Живая проверка BT-аудиотракта BlindNav: действительно ли переключаются
# профили A2DP/HFP и в какое физическое устройство при этом уходит звук.
#
# Отвечает на вопрос, который по логам не виден: pactl рапортует смену
# профиля успешной, но звук может играть мимо гарнитуры — в динамик платы,
# если указатель default сервера сполз на встроенный кодек rk809.
#
# Запуск (наушники должны быть включены и подключены):
#   bash tools/bt_audio_check.sh
#
# Слушать в наушниках. Сигнал в A2DP должен звучать заметно чище и шире, чем
# в HFP (44.1кГц стерео против 16кГц моно). Если оба звучат одинаково узко —
# карта физически осталась в HFP. Если какой-то не слышен вовсе — он ушёл не
# в гарнитуру, смотреть строку "Играю в:" под ним.

export PULSE_RUNTIME_PATH=/run/user/0/pulse
MAC_UND="1C_6E_4C_89_E9_32"
CARD="bluez_card.${MAC_UND}"

fail() { echo "ОШИБКА: $*"; exit 1; }

pactl info >/dev/null 2>&1 || fail "PulseAudio не отвечает на $PULSE_RUNTIME_PATH."

if ! pactl list cards short | grep -q "$CARD"; then
    echo "Карты $CARD нет — гарнитура не подключена."
    echo "Включите наушники и подождите до 30 секунд (bt_keeper подключит сам),"
    echo "либо: bluetoothctl connect ${MAC_UND//_/:}"
    exit 1
fi

echo "=== Загруженные модули, влияющие на переключение ==="
if pactl list modules | grep -q "auto_switch=false"; then
    echo "  module-bluetooth-policy: auto_switch=false — политика не мешает (ок)"
else
    echo "  module-bluetooth-policy: auto_switch НЕ выключен — сервер может"
    echo "    переключать профиль сам, конкурируя с демоном."
    echo "    Лечится перезапуском: systemctl restart bt_keeper"
fi
if pactl list modules short | grep -q module-suspend-on-idle; then
    echo "  module-suspend-on-idle: ЗАГРУЖЕН — начало фраз может срезаться"
    echo "    после паузы. Лечится перезапуском: systemctl restart bt_keeper"
else
    echo "  module-suspend-on-idle: выгружен (ок)"
fi
echo

probe() {
    local profile="$1" rate="$2"

    echo "=== Профиль: $profile ==="
    pactl set-card-profile "$CARD" "$profile" || { echo "  set-card-profile отказал"; return; }

    # Ждём подтверждения ровно так же, как это делает switch_bt_profile().
    for _ in $(seq 1 30); do
        pactl list cards | grep -A200 "$CARD" | grep -q "Active Profile: $profile" && break
        sleep 0.1
    done
    echo -n "  Подтверждённый профиль карты: "
    pactl list cards | grep -A200 "$CARD" | grep -m1 "Active Profile:" | sed 's/^\s*//'

    local sink source
    sink=$(pactl list sinks short | awk '$2 ~ /^bluez_sink\./ {print $2; exit}')
    source=$(pactl list sources short | awk '$2 ~ /^bluez_source\./ {print $2; exit}')

    echo "  bluez-синк:      ${sink:-НЕТ}"
    echo "  bluez-источник:  ${source:-нет (в A2DP его и не должно быть)}"
    echo "  default sink:    $(pactl info | sed -n 's/^Default Sink: //p')"
    echo "  default source:  $(pactl info | sed -n 's/^Default Source: //p')"

    if [ -z "$sink" ]; then
        echo "  Играть некуда — bluez-синка нет."
        echo
        return
    fi

    # Формат синка показывает, что реально согласовано с гарнитурой:
    # 16000Hz 1ch = HFP/mSBC, 44100Hz 2ch = A2DP.
    echo -n "  Формат синка:    "
    pactl list sinks short | awk -v s="$sink" '$2 == s {print $4, $5}'

    echo "  Играю в: $sink"
    ffmpeg -loglevel quiet -f lavfi -i "sine=frequency=740:duration=0.35" \
        -ar "$rate" -ac 1 -f s16le - 2>/dev/null |
        aplay -D "pulse:$sink" -r "$rate" -f S16_LE -t raw -c 1 2>&1 | grep -v "^Playing"
    echo "  (слышали сигнал в наушниках?)"
    echo
    sleep 1
}

probe a2dp_sink 44100
probe handsfree_head_unit 16000
probe a2dp_sink 44100

echo "=== Итог ==="
echo "Если оба профиля звучали одинаково узко и глухо — переключение не"
echo "доходит до гарнитуры, смотреть 'Формат синка' выше: он должен меняться"
echo "между 44100Hz 2ch (A2DP) и 16000Hz 1ch (HFP)."
