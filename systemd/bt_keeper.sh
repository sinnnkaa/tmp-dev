#!/bin/bash
MAC="1C:6E:4C:89:E9:32"
export PULSE_RUNTIME_PATH=/run/user/0/pulse

# Статус наушников опрашивается часто (это дешёвый локальный вызов), а вот
# попытки подключения — с нарастающей паузой: прежний вариант дёргал
# bluetoothctl connect каждые 10 секунд независимо от результата, что давало
# 24 тысячи строк в журнале за сутки. Потолок выдержки намеренно небольшой:
# устройством пользуется незрячий человек, и полчаса без звука неприемлемы,
# а от спама защищает запись только по смене состояния, а не длина паузы.
POLL_SEC=10
RETRY_MIN_SEC=10
RETRY_MAX_SEC=30

audio_alive() {
    pactl info >/dev/null 2>&1
}

connected() {
    bluetoothctl info "$MAC" 2>/dev/null | grep -q "Connected: yes"
}

start_audio_server() {
    echo "[BT Keeper] Аудиосервер не отвечает — запускаю..."
    killall pulseaudio 2>/dev/null
    sleep 1

    # --exit-idle-time=-1 здесь обязателен. В daemon.conf этот параметр
    # закомментирован, то есть действует дефолт 20 секунд, а звук на плате
    # играется через сырой ALSA (aplay -D default), поэтому постоянных
    # клиентов у PulseAudio нет вообще. Без флага он поднимался, через 20
    # секунд простоя выходил и уносил с собой module-bluez5-device: A2DP
    # исчезал, наушники отваливались, и подключиться заново было некуда —
    # bluez отвечал br-connection-profile-unavailable. Флаг задан здесь, а не
    # в /etc/pulse/daemon.conf, чтобы настройка лежала в репозитории и уезжала
    # вместе с проектом, а не жила невидимо в системе.
    pulseaudio --start --exit-idle-time=-1

    # Ждём реальной готовности вместо фиксированного sleep (до ~10 секунд)
    for i in $(seq 1 20); do
        audio_alive && break
        sleep 0.5
    done

    if audio_alive; then
        echo "[BT Keeper] Аудиосервер поднят."
        # Обычно модули уже подняты из default.pa — тогда pactl откажется и это
        # нормально. Вызов нужен для случая, когда default.pa их не завёл.
        pactl load-module module-bluetooth-discover 2>/dev/null
        return 0
    fi

    echo "[BT Keeper] Аудиосервер поднять НЕ удалось."
    return 1
}

retry_delay=$RETRY_MIN_SEC
next_attempt=0
state=""

echo "[BT Keeper] Вход в цикл мониторинга..."
while true; do
    # Проверка аудиосервера живёт внутри цикла, а не перед ним. Раньше она
    # выполнялась один раз при старте, и смерть PulseAudio после этого уже
    # никто не замечал: цикл следил только за наушниками, а подключить их без
    # аудиопрофиля невозможно. Сервис выглядел работающим, звука не было, и
    # лечилось только systemctl restart руками.
    if ! audio_alive; then
        start_audio_server
        # Профиль bluez регистрируется заново, поэтому прежняя связь мертва:
        # пробуем подключиться немедленно, не дожидаясь выдержки.
        retry_delay=$RETRY_MIN_SEC
        next_attempt=0
        state=""
    fi

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
