#!/bin/bash
# Путь разрешается через readlink: install.sh кладёт симлинк /root/bt_keeper.sh
# на этот файл, и без разрешения ссылки каталог репозитория не найдётся.
. "$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/../tools" && pwd)/voice_env.sh"

MAC="$BLINDNAV_BT_MAC"

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

# Две настройки сервера, которые нельзя задать из voice_nav_daemon.py: они
# относятся к загруженным модулям, а не к отдельному воспроизведению.
#
# Вызывается на каждом витке цикла, а не один раз при подъёме сервера. При
# холодной загрузке платы PulseAudio стартует раньше, чем bluez успевает
# завести module-bluetooth-policy, и разовая настройка проваливалась целиком:
# unload-module отвечал отказом на ещё не загруженный модуль, обе правки молча
# не применялись, а в журнале это выглядело обычным успешным стартом. Плата
# поднималась с auto_switch=1 и живым module-suspend-on-idle — ровно с теми
# двумя дефектами, ради которых эти строки и написаны: профиль уводило обратно
# в A2DP и начало фраз срезал засыпающий синк. Лечилось только ручным
# systemctl restart bt_keeper, чего на съёмках делать некому.
# Проверки идут по факту (какие модули загружены и с какими аргументами),
# сообщения печатаются только при реальном изменении — журнал не засоряется.
tune_audio_server() {
    local policy

    # 1. module-bluetooth-policy идёт из /etc/pulse/default.pa с дефолтным
    #    auto_switch=1 и сам переключает профиль карты, когда на bluez-источнике
    #    появляется или исчезает записывающий поток. Демон переключает профиль
    #    вручную и дожидается подтверждения (switch_bt_profile), после чего
    #    политика могла увести карту обратно в A2DP — отсюда наблюдавшееся
    #    "a2dp иногда звучит как hfp" и срабатывание сигнала через раз.
    #    Переключение остаётся ровно одно, явное, из демона.
    policy=$(pactl list short modules 2>/dev/null | grep 'module-bluetooth-policy')
    if [ -n "$policy" ] && ! printf '%s' "$policy" | grep -q 'auto_switch=false'; then
        if pactl unload-module module-bluetooth-policy 2>/dev/null; then
            pactl load-module module-bluetooth-policy auto_switch=false >/dev/null 2>&1 \
                && echo "[BT Keeper] module-bluetooth-policy: auto_switch=false" \
                || echo "[BT Keeper] ВНИМАНИЕ: module-bluetooth-policy не поднялся обратно."
        fi
    fi

    # 2. module-suspend-on-idle усыпляет синк через несколько секунд тишины.
    #    Пробуждение BT-линка (особенно свежего SCO в HFP) стоит секунду-две, и
    #    ровно в неё проваливалось начало фразы: aplay рапортовал успешный старт,
    #    а физически не звучало ничего. В play_ready_tone это обходилось секундой
    #    тишины-разгона перед сигналом — лечение симптома. Без модуля синк не
    #    засыпает вовсе, линк остаётся тёплым, начало фраз не режется.
    #    Цена — гарнитура постоянно принимает поток и садится быстрее; для
    #    носимого устройства на съёмках это приемлемый размен.
    if pactl list short modules 2>/dev/null | grep -q 'module-suspend-on-idle'; then
        pactl unload-module module-suspend-on-idle 2>/dev/null \
            && echo "[BT Keeper] module-suspend-on-idle выгружен (звук не засыпает)."
    fi
}

start_audio_server() {
    echo "[BT Keeper] Аудиосервер не отвечает — запускаю..."
    killall pulseaudio 2>/dev/null
    sleep 1

    # --exit-idle-time=-1 здесь обязателен. В daemon.conf этот параметр
    # закомментирован, то есть действует дефолт 20 секунд, а звук на плате
    # играется разовыми запусками aplay на каждую фразу (tools/say.sh),
    # поэтому постоянных клиентов у PulseAudio нет вообще: между фразами
    # сервер простаивает. Без флага он поднимался, через 20
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
        tune_audio_server
        return 0
    fi

    echo "[BT Keeper] Аудиосервер поднять НЕ удалось."
    return 1
}

retry_delay=$RETRY_MIN_SEC
next_attempt=0
state=""

# Сервер мог пережить перезапуск самого keeper'а (Restart=always) — тогда
# start_audio_server ниже не вызовется и настройки модулей не применятся.
# Применяем их к уже живому серверу: обе операции идемпотентны.
audio_alive && tune_audio_server

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

    # Настройки модулей проверяются на каждом витке: bluez заводит
    # module-bluetooth-policy не сразу после старта сервера, а при появлении
    # адаптера, и на холодной загрузке это происходит уже после подъёма звука.
    audio_alive && tune_audio_server

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
