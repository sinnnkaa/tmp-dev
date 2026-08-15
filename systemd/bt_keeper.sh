#!/bin/bash
MAC="1C:6E:4C:89:E9:32"
export PULSE_RUNTIME_PATH=/run/user/0/pulse

# Перезапускаем pulseaudio только если он реально не отвечает — раньше сервис
# убивал и поднимал звуковой сервер на КАЖДЫЙ рестарт (Restart=always,
# RestartSec=5), что при зацикленных падениях этого юнита рвало звук во всех
# остальных процессах (main.cpp / voice_nav_daemon.py) каждые 5 секунд.
if ! pactl info >/dev/null 2>&1; then
    echo "[BT Keeper] Аудиосервер не отвечает — перезапуск..."
    killall pulseaudio 2>/dev/null
    sleep 1

    echo "[BT Keeper] Запуск аудиосервера..."
    pulseaudio --start

    # Ждём реальной готовности вместо фиксированного sleep (до ~10 секунд)
    for i in $(seq 1 20); do
        pactl info >/dev/null 2>&1 && break
        sleep 0.5
    done
else
    echo "[BT Keeper] Аудиосервер уже работает."
fi

echo "[BT Keeper] Подгрузка Bluetooth модулей..."
pactl load-module module-bluetooth-discover 2>/dev/null

# Статус наушников опрашивается часто (это дешёвый локальный вызов), а вот
# попытки подключения — с нарастающей паузой. При выключенной гарнитуре
# прежний вариант дёргал bluetoothctl connect каждые 10 секунд: 24 тысячи
# строк в журнале за сутки и столько же безответных запросов к стеку bluez.
# Частый опрос при этом сохранён, поэтому включённые наушники подхватываются
# за POLL_SEC, а не за время выдержки.
POLL_SEC=10
RETRY_MIN_SEC=10
RETRY_MAX_SEC=300

connected() {
    bluetoothctl info "$MAC" 2>/dev/null | grep -q "Connected: yes"
}

retry_delay=$RETRY_MIN_SEC
next_attempt=0
state=""

echo "[BT Keeper] Вход в цикл мониторинга..."
while true; do
    if connected; then
        # Пишем только смену состояния: иначе журнал забивается строками
        # об одном и том же, и в нём не видно ничего другого.
        if [ "$state" != "up" ]; then
            echo "[BT Keeper] Наушники на связи."
            state="up"
        fi
        retry_delay=$RETRY_MIN_SEC
        next_attempt=0
    else
        if [ "$state" != "down" ]; then
            echo "[BT Keeper] Наушники отвалились."
            state="down"
        fi

        now=$(date +%s)
        if [ "$now" -ge "$next_attempt" ]; then
            # timeout защищает цикл от зависания, если bluetoothctl не получит
            # ответ (например, ждёт подтверждения пары) — иначе "keeper" сам
            # встаёт намертво.
            timeout 10 bluetoothctl connect "$MAC" >/dev/null 2>&1
            # Код возврата bluetoothctl доверия не заслуживает: он бывает
            # нулевым и при "Failed to connect". Проверяем фактом.
            if connected; then
                echo "[BT Keeper] Подключены."
                state="up"
                retry_delay=$RETRY_MIN_SEC
                next_attempt=0
            else
                next_attempt=$(( now + retry_delay ))
                retry_delay=$(( retry_delay * 2 ))
                if [ "$retry_delay" -gt "$RETRY_MAX_SEC" ]; then
                    retry_delay=$RETRY_MAX_SEC
                fi
            fi
        fi
    fi

    sleep "$POLL_SEC"
done
