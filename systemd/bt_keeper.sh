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

echo "[BT Keeper] Вход в цикл мониторинга..."
while true; do
    # Проверяем статус подключения
    if ! bluetoothctl info "$MAC" | grep -q "Connected: yes"; then
        echo "[BT Keeper] Наушники отвалились. Подключаю..."
        # timeout защищает цикл от зависания, если bluetoothctl не получит ответ
        # (например, ждёт подтверждения пары) — иначе "keeper" сам встаёт намертво.
        timeout 10 bluetoothctl connect "$MAC"
    fi

    # Спим 10 секунд перед следующей проверкой
    sleep 10
done
