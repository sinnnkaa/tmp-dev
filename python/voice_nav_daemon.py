import os
import sys
import time
import json
import difflib
import requests
import subprocess
import socket
import threading
from vosk import Model, KaldiRecognizer

# Тексты, произносимые по шаблону, живут отдельным модулем без зависимостей —
# из него же их берёт сборщик кэша озвучки (tools/build_voice_cache.sh).
from phrases import TURN_ANNOUNCE_MAX_DIST, is_announced, plural_meters, turn_phrase

try:
    import gps
except ImportError:
    print("Ошибка: не установлена библиотека gps.")
    sys.exit(1)

ADDRESS_DB_PATH = "/root/diplom-cpp/blind_nav/map/addresses.json"
OSRM_URL = "http://127.0.0.1:5000/route/v1/foot/"
VOSK_MODEL_PATH = "/root/diplom-cpp/blind_nav/vosk/model"
SOCKET_PORT = 9999

# Общая блокировка звукового устройства с C++ ядром (main.cpp), чтобы
# предупреждения об опасности и голосовые инструкции маршрута не звучали
# одновременно поверх друг друга.
AUDIO_LOCK_PATH = "/run/lock/blind_nav_audio.lock"

# Общий с C++ ядром скрипт озвучки: синтез piper, воспроизведение в гарнитуру
# и сохранение реплики с меткой времени для автомонтажа прогулки.
SAY_SCRIPT = "/root/diplom-cpp/tools/say.sh"

MIC_LOG_PATH = "/root/diplom-cpp/mic_audio.raw"
MIC_LOG_MAX_BYTES = 20 * 1024 * 1024

# Диагностический признак "идёт маршрут". Раньше его читало C++ ядро, чтобы
# подавлять предупреждения об опасности на время навигации; подавление убрано
# по техзаданию — предупреждение должно звучать всегда, — и потребителя у файла
# больше нет. Оставлен как способ снаружи посмотреть состояние демона
# (cat /dev/shm/nav_active), поведение системы от него не зависит.
NAV_STATUS_PATH = "/dev/shm/nav_active"

# Тег текущего отрезка записи прогулки: его публикует C++ ядро, а сюда он нужен,
# чтобы складывать записанную диктовку рядом с остальными кусками этого отрезка
# (см. tools/montage_session.sh).
SESSION_TAG_PATH = "/dev/shm/nav_session"

# Частота захвата с микрофона гарнитуры: её требует Vosk и в ней же работает
# HFP-синк (mSBC), пересчёт на лету только портит звук. На этой же частоте
# сохраняется диктовка для монтажа прогулки, поэтому значение продублировано в
# tools/voice_env.sh (HEADSET_RATE) — оттуда его берёт tools/montage_session.sh.
# Разойтись им нельзя: диктовка в ролике зазвучала бы не в том темпе.
HEADSET_RATE = 16000

# Профиль Bluetooth, в котором работает устройство. Дублирует
# BLINDNAV_BT_PROFILE из tools/voice_env.sh — языки разные, общий код
# невозможен, расходиться нельзя. Там же записано, почему это HFP, а не A2DP:
# коротко, A2DP не влезает в UART-канал контроллера и играет на дне шкалы SBC
# (журнал сервера: "Bitpool has changed to 7 ... 17" за каждую фразу).
#
# Следствие для этого файла: профиль больше не переключается туда-обратно.
# Раньше диктовка переводила карту в HFP, а после — обратно в A2DP; теперь
# устройство в HFP постоянно, микрофон гарнитуры доступен всегда, и один из
# самых частых источников "звук пропал на середине прогулки" исчезает вместе
# с переключениями.
BT_PROFILE = "handsfree_head_unit"
VIDEO_DIR = "/root/diplom-cpp/videos"

# Флаг "микрофон гарнитуры открыт": C++ ядро (main.cpp) читает его и на это
# время откладывает предупреждения об опасности. Без него предупреждение
# звучит в тот же наушник, с микрофона которого сейчас пишется адрес, TTS
# попадает на вход Vosk и портит распознавание. Окно ограничено длительностью
# захвата (4-7 секунд); ядро откладывает фразу, не сбрасывая выдержку, и
# произносит её сразу после закрытия микрофона.
MIC_OPEN_PATH = "/dev/shm/nav_mic_open"

# Порог нечёткого поиска адреса и порог "подряд идущих" сбоев OSRM,
# после которого пользователь получает голосовое предупреждение.
ADDRESS_MATCH_CUTOFF = 0.5
OSRM_FAILURE_WARN_EVERY = 3

NAV_ACTIVE = False


def write_nav_status(active: bool):
    """Публикует признак "идёт маршрут" в tmpfs (см. NAV_STATUS_PATH)."""
    try:
        with open(NAV_STATUS_PATH, "w") as f:
            f.write("1" if active else "0")
    except Exception as e:
        print(f"[NavStatus] Не удалось записать статус: {e}")


def write_mic_open(open_: bool):
    """Сообщает C++ ядру, открыт ли сейчас микрофон гарнитуры (см. MIC_OPEN_PATH)."""
    try:
        with open(MIC_OPEN_PATH, "w") as f:
            f.write("1" if open_ else "0")
    except Exception as e:
        print(f"[MicStatus] Не удалось записать статус: {e}")


def session_speech_dir():
    """Каталог кусков текущей записи прогулки или None, если запись не идёт."""
    try:
        with open(SESSION_TAG_PATH) as f:
            tag = f.read().strip()
    except Exception:
        return None

    if not tag:
        return None

    path = os.path.join(VIDEO_DIR, f"speech_{tag}")
    return path if os.path.isdir(path) else None


def cap_log_file(path, max_bytes):
    """Обрезает файл, если он превысил лимит — защита от бесконечного роста на SD-карте."""
    try:
        if os.path.exists(path) and os.path.getsize(path) > max_bytes:
            open(path, "w").close()
    except Exception as e:
        print(f"[LogCap] Не удалось обрезать {path}: {e}")


def pulse_env():
    """Окружение для любого обращения к PulseAudio.

    Сервер поднимается из bt_keeper.sh под root и кладёт сокет в
    /run/user/0/pulse. У systemd-юнита nav_daemon.service своего
    XDG_RUNTIME_DIR нет, поэтому путь задаётся явно — иначе pactl/aplay/ffmpeg
    не находят сервер и молча уходят в сырой ALSA мимо наушников.
    """
    env = os.environ.copy()
    env["PULSE_RUNTIME_PATH"] = "/run/user/0/pulse"
    return env


def bt_device_name(kind):
    """Фактическое имя bluez-устройства (kind: "sink" или "source") или None.

    Имя не собирается из MAC-адреса руками, а спрашивается у сервера: оно зависит
    от активного профиля (`bluez_sink.MAC.a2dp_sink` против
    `bluez_sink.MAC.handsfree_head_unit`), а в HFP появляется ещё и
    `bluez_source.MAC.handsfree_head_unit`, которого в A2DP не существует
    вовсе. Обращение по вычисленному "на бумаге" имени — как в прежнем
    BT_SINK_HFP — тихо промахивается мимо реального устройства.
    """
    try:
        out = subprocess.run(
            ["pactl", "list", f"{kind}s", "short"],
            capture_output=True, text=True, env=pulse_env()
        ).stdout
    except Exception:
        return None

    # Сначала устройство рабочего профиля, и только потом любое bluez.
    # При смене профиля старый синк исчезает не мгновенно, поэтому "первый
    # попавшийся" какое-то время указывает на покидаемый профиль — играть в
    # него означает тихо промахнуться мимо гарнитуры. Запасной проход
    # оставлен: глухая речь в неверном профиле лучше молчания.
    prefix = f"bluez_{kind}."
    names = [
        fields[1] for fields in (line.split("\t") for line in out.splitlines())
        if len(fields) > 1 and fields[1].startswith(prefix)
    ]
    for name in names:
        if BT_PROFILE in name:
            return name
    return names[0] if names else None


def bt_card_name():
    """Имя bluez-карты гарнитуры или None, если она не подключена.

    Спрашивается у сервера, а не собирается из MAC-адреса, записанного здесь
    константой. Прежний BT_CARD был третьим экземпляром одного и того же адреса
    в проекте — в bt_keeper.sh он записан через двоеточия, в диагностике через
    подчёркивания, здесь в составе имени карты. При замене наушников забыть
    один из трёх было легко, а результат получался не отказом, а половинчатой
    работой: подключение есть, профиль не переключается, звук идёт мимо.
    Теперь адрес записан ровно в одном месте (tools/voice_env.sh), и тому, кто
    работает с уже подключённой картой, он вообще не нужен.
    """
    try:
        out = subprocess.run(
            ["pactl", "list", "cards", "short"],
            capture_output=True, text=True, env=pulse_env()
        ).stdout
    except Exception:
        return None

    for line in out.splitlines():
        fields = line.split("\t")
        if len(fields) > 1 and fields[1].startswith("bluez_card."):
            return fields[1]
    return None


def playback_device():
    """Аргумент -D для aplay: конкретный bluez-синк, а не "default".

    Это ключевая правка. `aplay -D default` уходит в alsa-плагин pulse и
    играет в *default sink* сервера, а тот на этой плате не гарантированно
    указывает на наушники: в /root/.config/pulse/*-default-sink записан
    встроенный кодек `alsa_output.platform-rk809-sound.stereo-fallback`, и
    module-default-device-restore возвращает его при каждом старте сервера.
    Хуже того, set-card-profile уничтожает синк старого профиля и создаёт
    новый, так что указатель default переставляется на каждом переключении
    A2DP<->HFP и вполне успевает сползти обратно на плату. Отсюда живой
    симптом "сигнал слышно через раз" — половина попыток физически звучала в
    динамик платы, а не в гарнитуру.

    Если гарнитуры нет (не подключена, севшая батарея), остаётся "default" —
    тогда звук идёт хоть куда-то, а не пропадает совсем.

    Тот же поиск на shell живёт в tools/voice_env.sh (voice_playback_device) —
    им пользуются озвучка и диагностика. Разные языки, общий код невозможен;
    правки нужны в обоих местах.
    """
    sink = bt_device_name("sink")
    return f"pulse:{sink}" if sink else "default"


def capture_device():
    """Имя bluez-микрофона для `ffmpeg -f pulse -i ...` или None, если его нет.

    Возврата к "default" здесь больше нет, и это принципиально. Default source
    на этой плате — `alsa_input...Webcam...`, микрофон USB-вебкамеры: он висит
    на груди и снимает улицу целиком. Прежняя подстраховка выглядела как
    надёжность, а работала как тихий отказ от правила «речь распознаём только
    с гарнитуры»: пока указатель default не переставился на наушники,
    распознавание слушало вебкамеру, и переключение в HFP на захват не влияло
    вовсе.

    Микрофон вебкамеры остаётся ровно у одной задачи — фоновой дорожки
    прогулки (record_session.sh, `ambient_<тег>.mp3`). К распознаванию речи он
    отношения не имеет, поэтому подменять им гарнитуру нечем: нет
    гарнитурного источника — нет захвата.
    """
    return bt_device_name("source")


def bind_defaults_to_bt():
    """Прибивает default sink/source сервера к гарнитуре после смены профиля.

    Прежнее обоснование этой функции («C++ ядро играет через aplay -D default
    и имени синка не знает») больше не соответствует коду: ядро зовёт
    tools/say.sh, а тот спрашивает синк у сервера сам (voice_playback_device).
    Ни один потребитель проекта на указатель default уже не опирается.

    Функция всё же оставлена, и вот зачем. set-card-profile уничтожает синк
    старого профиля и создаёт новый, а module-default-device-restore при этом
    возвращает указатель на встроенный кодек платы. Всё, что запускается на
    плате мимо нашего кода — ручная проверка через aplay/paplay, ffmpeg из
    записи прогулки, любой сторонний клиент, — попадёт в динамик платы, и
    выглядеть это будет как «звука в наушниках нет», хотя гарнитура
    подключена. Держать указатель в осмысленном состоянии дешевле, чем
    объяснять этот симптом каждый раз заново.

    Второе действие функции, снятие засыпания с синка, к указателю отношения
    не имеет и нужно по-настоящему (см. комментарий ниже).
    """
    env = pulse_env()
    for kind, setter in (("sink", "set-default-sink"), ("source", "set-default-source")):
        name = bt_device_name(kind)
        if not name:
            continue
        subprocess.run(
            ["pactl", setter, name],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env
        )

    # Снимаем засыпание с синка немедленно: module-suspend-on-idle усыпляет
    # устройство через несколько секунд простоя, а пробуждение BT-линка стоит
    # секунду-другую, в течение которой начало фразы физически не звучит.
    sink = bt_device_name("sink")
    if sink:
        subprocess.run(
            ["pactl", "suspend-sink", sink, "0"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env
        )


def switch_bt_profile(profile, verify_timeout=3.0, retries=1):
    """Переключает профиль и дожидается, пока Pulse подтвердит переход, а не
    угадывает время фиксированным sleep. pactl возвращается сразу после того,
    как поставил команду в очередь — согласование HFP/SCO (или повторное
    согласование A2DP при переключении назад) с самой гарнитурой может занять
    дольше и не всегда укладывается с первой попытки — тогда следующая фраза
    молча проигрывается ещё в старом, неподходящем профиле (например, TTS
    после возврата в A2DP звучит в оставшемся HFP-режиме — 16кГц моно вместо
    44кГц стерео, отсюда заметно "грязный" звук). Поэтому при неудаче команда
    переключения переиздаётся ещё раз (retries), а не просто отдаётся один раз
    в надежде на удачу.

    Отдельно: module-bluetooth-policy (загружен из /etc/pulse/default.pa)
    по умолчанию идёт с auto_switch=1 и сам дёргает профиль карты по
    появлению/исчезновению записывающего потока на bluez-источнике. Он
    конкурирует с этим ручным переключением и может увести карту обратно в
    A2DP уже после того, как здесь профиль подтверждён — это второй источник
    симптома "a2dp звучит как hfp". Политика выключается в bt_keeper.sh
    (auto_switch=false), потому что живёт она на стороне сервера, а не
    демона."""
    custom_env = pulse_env()

    card = bt_card_name()
    if not card:
        # Наушники выключены или вне зоны. Раньше здесь шла команда карте с
        # именем, собранным из константы: pactl молча отвечал ошибкой, а демон
        # продолжал так, будто профиль переключился.
        print("[BT Error] bluez-карты нет — гарнитура не подключена.")
        return False

    for attempt in range(retries + 1):
        try:
            subprocess.run(
                ["pactl", "set-card-profile", card, profile],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=custom_env
            )
        except Exception as e:
            print(f"[BT Error] Не удалось переключить профиль: {e}")
            return False

        deadline = time.time() + verify_timeout
        while time.time() < deadline:
            try:
                out = subprocess.run(
                    ["pactl", "list", "cards"], capture_output=True, text=True,
                    env=custom_env
                ).stdout
            except Exception:
                break
            idx = out.find(card)
            if idx != -1 and f"Active Profile: {profile}" in out[idx:idx + 2000]:
                bind_defaults_to_bt()
                return True
            time.sleep(0.1)

        if attempt < retries:
            print(f"[BT Error] Профиль {profile} не подтверждён за {verify_timeout}с — повторяю попытку.")

    print(f"[BT Error] Профиль {profile} не подтверждён после {retries + 1} попыток — продолжаю без гарантии готовности звука.")
    return False


# Тишина-разгон перед сигналом готовности, в секундах.
#
# Была 1.0 — обход того, что свежий SCO-канал гарнитуры первую секунду после
# подтверждения профиля "глухой": aplay рапортовал успешный старт, а физически
# не звучало ничего. Живой тест тогда показал, что 0.35 с хватает начиная со
# второй попытки подряд, но не для первой — то есть мало было именно для
# ПЕРВОГО открытия линка в сессии.
#
# С переходом на постоянный HFP (BT_PROFILE) первого открытия больше не
# бывает: линк поднят с момента подключения наушников и держится bt_keeper'ом,
# а профиль на диктовку не переключается. Условие, ради которого стояла
# секунда, исчезло, и значение возвращено к 0.35 — к тому, что живой тест
# подтвердил для тёплого линка.
#
# Цена ошибки несимметрична, поэтому ноль здесь не ставится: слишком маленький
# запас означает, что человек не услышит "можно говорить" и промолчит в
# открытый микрофон, а лишние треть секунды он просто подождёт. Если сигнал
# когда-нибудь снова начнёт пропадать — возвращать сюда, а не искать в звуке.
READY_TONE_WARMUP = 0.35


def play_ready_tone():
    """Короткий двухтоновый сигнал сразу после подтверждённого переключения на
    HFP-гарнитуру — вместо молчаливой паузы явно подсказывает пользователю,
    что микрофон уже слушает и можно говорить. Играется через тот же
    flock/AUDIO_LOCK_PATH, что и speak(), чтобы не наложиться на предупреждения
    об опасности из C++ ядра.

    Перед самим сигналом — настоящая тишина внутри того же потока, а не
    отдельный time.sleep() до запуска ffmpeg: это даёт устройству
    воспроизведения непрерывный поток данных с самого открытия, без
    непредсказуемой задержки на повторный запуск ffmpeg/aplay после паузы.
    Длительность и её обоснование — в READY_TONE_WARMUP выше.

    Частота дискретизации — 16000Гц, как у самого HFP-синка (mSBC, см.
    `pactl list sinks`), а не 22050 как у speak()/ffmpeg по умолчанию: на
    рассинхронизации частот (запрошенные 22050 передискретизируются в родные
    16000 буквально на 200мс клипе) живой тест дал не тон, а шум/треск —
    отдаём сразу в родном формате синка, без пересчёта на лету.

    Громкость: `pactl set-sink-volume 150%` из предыдущей правки не дал
    заметного эффекта на живом тесте ("тихий"). Настоящая причина, скорее
    всего, была не в шагах AT+VGS гарнитуры, а в том, что громкость крутилась
    у синка, собранного из MAC-адреса "на бумаге", тогда как звук в этот момент
    мог играть совсем в другое устройство (см. playback_device). Теперь и
    громкость, и воспроизведение адресуются одному и тому же фактическому
    синку. Амплитуда сигнала оставлена на 1.0 (пик без клиппинга)."""
    device = playback_device()
    inner = (
        f'ffmpeg -loglevel quiet -f lavfi -i "anullsrc=r=16000:cl=mono:d={READY_TONE_WARMUP}" '
        '-f lavfi -i "sine=frequency=740:duration=0.09" '
        '-f lavfi -i "sine=frequency=1050:duration=0.13" '
        '-filter_complex "[0:a][1:a][2:a]concat=n=3:v=0:a=1,volume=1.0" '
        '-ar 16000 -ac 1 -f s16le - | '
        f'aplay -D {device} -r 16000 -f S16_LE -t raw -c 1'
    )
    custom_env = pulse_env()
    sink = bt_device_name("sink")
    if sink:
        subprocess.run(
            ["pactl", "set-sink-volume", sink, "100%"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=custom_env
        )
    print(f"[Сигнал] Устройство воспроизведения: {device}")
    subprocess.run(["flock", AUDIO_LOCK_PATH, "-c", inner], env=custom_env)


def speak(text, sync=False):
    """Произносит фразу через общий с C++ ядром скрипт озвучки (tools/say.sh).

    Конвейер piper|aplay больше не собирается здесь строкой: он был выписан
    дважды — тут и в main.cpp через std::system, и правки расходились (ядро
    так и осталось с "-D default", когда демон уже адресовался bluez-синку по
    имени). Заодно say.sh сохраняет реплику с меткой времени, из которых
    tools/montage_session.sh собирает голосовую дорожку ролика.

    Команда передаётся списком без shell: flock умеет запускать программу
    напрямую, поэтому текст фразы не проходит ни через одну стадию разбора
    оболочкой и не нуждается в экранировании.

    При sync=True проверяется код возврата say.sh: ненулевой означает, что
    звука не было вообще — гарнитура отвалилась, синк исчез, устройство занято.
    Раньше проверять было нечего (say.sh всегда возвращал успех), и отказ
    озвучки выглядел как обычная работа. Все ответственные реплики демона —
    подтверждение адреса, отмена, прибытие — идут именно с sync=True.

    Возвращает True, если фраза прозвучала. При sync=False результата ещё не
    существует: процесс только запущен, и ждать его здесь нельзя — это цикл
    навигации. Такой вызов возвращает True как "запущено без ошибки", а сам
    отказ, если он случится, останется в журнале от say.sh.
    """
    print(f"[Голос]: {text}")
    cmd = ["flock", AUDIO_LOCK_PATH, SAY_SCRIPT, text, "nav"]
    custom_env = pulse_env()

    if not sync:
        subprocess.Popen(cmd, env=custom_env)
        return True

    rc = subprocess.run(cmd, env=custom_env).returncode
    if rc != 0:
        print(f"[Голос] ФРАЗА НЕ ПРОЗВУЧАЛА (код {rc}): {text}")
        return False
    return True


def mic_profile_ready():
    """Переводит гарнитуру в HFP и честно отвечает, состоялся ли переход.

    Возврат switch_bt_profile игнорировался во всех пяти местах, где он
    вызывался. В четырёх из них — возвратах в A2DP, которых больше нет, — это
    было терпимо: фраза просто звучала в неподходящем режиме. Здесь неудача
    означает, что микрофон гарнитуры не активирован — и listen_and_transcribe запишет микрофон вебкамеры. Тот
    висит на груди, слышит улицу, и нечёткий поиск подтянет получившийся мусор
    к какому-нибудь адресу: маршрут построится не туда, причём молча.

    Хуже того, сразу за переключением звучал сигнал готовности. Устройство
    сообщало "микрофон слушает" именно в тот момент, когда он не слушал.
    Поэтому при неудаче диктовка не начинается вовсе: говорим об отказе и
    отдаём False вызывающему.

    С переходом на постоянный HFP (см. BT_PROFILE) вызов почти всегда стоит
    почти ничего: карта уже в нужном профиле, и pactl подтверждает это сразу.
    Функция при этом не лишняя, а становится проверкой — профиль мог увести
    кто угодно, от переподключения гарнитуры до ручной команды, и молча
    записывать в таком состоянии нельзя.

    Отката в A2DP при неудаче больше нет. Раньше он был осмыслен: A2DP был
    рабочим профилем, и вернуться в него значило вернуться к звуку. Теперь
    рабочий профиль — этот, и уход в A2DP означал бы смену одной поломки на
    другую, гарантированно плохо звучащую.
    """
    if switch_bt_profile(BT_PROFILE):
        return True

    speak("Микрофон гарнитуры не готов", sync=True)
    return False


def normalize_text(text):
    fillers = ["улица", "ул", "проспект", "пр", "дом", "д", "бульвар", "набережная", "переулок"]
    words = text.lower().split()
    words = [w for w in words if w not in fillers]

    num_map = {
        "один": 1, "два": 2, "три": 3, "четыре": 4, "пять": 5,
        "шесть": 6, "семь": 7, "восемь": 8, "девять": 9, "десять": 10,
        "одиннадцать": 11, "двенадцать": 12, "тринадцать": 13,
        "четырнадцать": 14, "пятнадцать": 15, "шестнадцать": 16,
        "семнадцать": 17, "восемнадцать": 18, "девятнадцать": 19,
        "двадцать": 20, "тридцать": 30, "сорок": 40, "пятьдесят": 50,
        "шестьдесят": 60, "семьдесят": 70, "восемьдесят": 80, "девяносто": 90,
        "сто": 100
    }

    result = []
    current_num = 0
    for w in words:
        if w in num_map:
            current_num += num_map[w]
        else:
            if current_num > 0:
                result.append(str(current_num))
                current_num = 0
            result.append(w)
    if current_num > 0: result.append(str(current_num))
    return " ".join(result)


def find_coordinates(text, address_keys, addresses_db):
    clean_text = normalize_text(text)
    if not clean_text: return None, None
    matches = difflib.get_close_matches(clean_text, address_keys, n=1, cutoff=ADDRESS_MATCH_CUTOFF)
    if matches: return matches[0], addresses_db[matches[0]]
    return None, None


def get_current_location():
    session = None
    try:
        session = gps.gps(mode=gps.WATCH_ENABLE | gps.WATCH_NEWSTYLE)
        for _ in range(10):
            report = session.next()
            if report["class"] == "TPV" and hasattr(report, "lat") and hasattr(report, "lon"):
                return report.lat, report.lon
            time.sleep(0.5)
    except Exception as e:
        print(f"Ошибка GPS: {e}")
    finally:
        # Сессия gpsd открывает сокет — не закрывая её, демон копит утечку
        # дескрипторов за долгую прогулку (опрос идёт каждые 5-10 секунд).
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
    return None, None


def get_route(start_lat, start_lon, end_lat, end_lon):
    url = f"{OSRM_URL}{start_lon},{start_lat};{end_lon},{end_lat}?steps=true&overview=false"
    try:
        response = requests.get(url, timeout=5)
        data = response.json()
        if data.get("code") == "Ok":
            return data["routes"][0]["distance"], data["routes"][0]["legs"][0]["steps"]
    except Exception as e:
        print(f"Ошибка OSRM: {e}")
    return None, None


def listen_and_transcribe(model, seconds):
    """Захватывает звук с гарнитуры и возвращает распознанный текст (или пустую строку)."""
    cap_log_file(MIC_LOG_PATH, MIC_LOG_MAX_BYTES)

    rec = KaldiRecognizer(model, HEADSET_RATE)

    # Источник спрашивается до флага "микрофон открыт": если гарнитурного
    # микрофона нет, открывать нечего и разворачивать флаг обратно не
    # придётся. Писать вместо гарнитуры вебкамеру нельзя — см. capture_device().
    source = capture_device()
    if not source:
        print("[STT] Микрофона гарнитуры нет — захват не начат.")
        return ""

    # Флаг ставится до запуска ffmpeg, а не после: ядро опрашивает файл раз в
    # 300мс, и запас нужен именно на старте, пока запись ещё не пошла.
    write_mic_open(True)
    print(f"[STT] Источник записи: {source}")
    # -nostdin: демон запускается под systemd и из shell, и ffmpeg не должен
    # трогать чужой стандартный ввод — это уже стоило потери данных в сборщике
    # кэша озвучки, где он вычитал себе список фраз.
    cmd = ['ffmpeg', '-nostdin', '-loglevel', 'quiet', '-f', 'pulse', '-i', source,
           '-ar', str(HEADSET_RATE), '-ac', '1', '-f', 's16le', '-t', str(seconds), '-']

    custom_env = pulse_env()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, env=custom_env)

    # Копия диктовки для монтажа прогулки. Это единственная запись, где голос
    # человека слышно чисто: микрофон гарнитуры существует только в профиле
    # HFP, то есть ровно в эти несколько секунд, а фоновый микрофон вебкамеры
    # снимает всю улицу разом. Метка времени ставится по первому пришедшему
    # блоку, а не по запуску ffmpeg: между ними уходит заметная доля секунды на
    # открытие потока, и на монтаже это был бы сдвиг дорожки.
    speech_dir = session_speech_dir()
    dictation = None
    dictation_path = None
    dictation_start = None
    if speech_dir:
        try:
            dictation_path = os.path.join(speech_dir, f"mic_{time.time_ns()}.raw")
            dictation = open(dictation_path, "wb")
        except Exception as e:
            print(f"[STT] Не удалось сохранить диктовку: {e}")
            dictation = None

    text = ""
    try:
        while True:
            data = proc.stdout.read(4000)
            if not data: break

            if dictation:
                if dictation_start is None:
                    dictation_start = time.time()
                dictation.write(data)

            with open(MIC_LOG_PATH, "ab") as f_mic:
                f_mic.write(data)
            if rec.AcceptWaveform(data):
                res = json.loads(rec.Result())
                if res['text']:
                    text = res['text']
                    break
    finally:
        proc.terminate()
        # Снимается в finally: если захват свалится с исключением, ядро иначе
        # осталось бы навсегда с отложенными предупреждениями — то есть немым.
        write_mic_open(False)

        if dictation:
            dictation.close()
            if dictation_start is None:
                os.remove(dictation_path)
            else:
                try:
                    with open(os.path.join(speech_dir, "mic.tsv"), "a", encoding="utf-8") as f:
                        f.write(f"{dictation_start:.6f}\t{os.path.basename(dictation_path)}\t{text}\n")
                except Exception as e:
                    print(f"[STT] Не удалось записать индекс диктовки: {e}")

    return text


def confirm_address(model, address_name):
    """Голосовое подтверждение найденного адреса перед стартом маршрута — защита
    от того, что нечёткий поиск (Левенштейн/difflib) подберёт похожий, но не тот адрес."""
    speak(f"Вы имели в виду {address_name}? Скажите да или нет.", sync=True)
    if not mic_profile_ready():
        # Подтверждения не было — считаем адрес неподтверждённым, вызывающий
        # скажет "Хорошо, отменяю" и не поведёт человека по случайному адресу.
        return False
    play_ready_tone()

    answer = listen_and_transcribe(model, 4)

    # Сравнение по целым словам, а не подстрокой: "да" — подстрока в "туда",
    # "погода", "надо" и т.п., из-за чего отказ ("нет, не туда") ошибочно
    # засчитывался как подтверждение маршрута.
    return "да" in answer.lower().split()


def navigation_worker(model, addresses_db, address_keys):
    global NAV_ACTIVE

    speak("Слушаю адрес", sync=True)
    if not mic_profile_ready():
        NAV_ACTIVE = False
        write_nav_status(False)
        return
    play_ready_tone()

    print("[STT] Говорите...")
    user_input = listen_and_transcribe(model, 7)

    if not NAV_ACTIVE:
        return

    if not user_input:
        speak("Адрес не распознан", sync=True)
        NAV_ACTIVE = False
        write_nav_status(False)
        return

    print(f"Вы сказали: {user_input}")
    address_name, end_coords = find_coordinates(user_input, address_keys, addresses_db)

    if not end_coords:
        speak("Не могу найти этот адрес", sync=True)
        NAV_ACTIVE = False
        write_nav_status(False)
        return

    if not NAV_ACTIVE:
        return

    if not confirm_address(model, address_name):
        speak("Хорошо, отменяю", sync=True)
        NAV_ACTIVE = False
        write_nav_status(False)
        return

    if not NAV_ACTIVE:
        return

    print(f"[OSRM] Ищу маршрут до {address_name}")
    start_lat, start_lon = get_current_location()
    if start_lat is None:
        speak("ДЖИПИЭС координаты не доступны!", sync=True)
        NAV_ACTIVE = False
        write_nav_status(False)
        return

    dist, steps = get_route(start_lat, start_lon, end_coords[0], end_coords[1])
    if dist is None:
        speak("Ошибка построения маршрута", sync=True)
        NAV_ACTIVE = False
        write_nav_status(False)
        return

    dist_m = int(dist)
    speak(f"Маршрут до {address_name} построен. Расстояние {dist_m} {plural_meters(dist_m)}.", sync=True)

    last_announced_loc = ""
    consecutive_route_failures = 0

    while NAV_ACTIVE:
        lat, lon = get_current_location()
        if lat is None:
            speak("ДЖИПИЭС координаты не доступны!", sync=True)
            time.sleep(3)
            continue

        dist, steps = get_route(lat, lon, end_coords[0], end_coords[1])
        if dist is None:
            consecutive_route_failures += 1
            # Повторяем предупреждение периодически, а не один раз, чтобы пользователь
            # не остался без обратной связи на весь оставшийся маршрут.
            if consecutive_route_failures % OSRM_FAILURE_WARN_EVERY == 0:
                speak("Проблема со связью с сервером маршрутизации, повторяю попытки.", sync=True)
            time.sleep(3)
            continue
        consecutive_route_failures = 0

        if dist < 15:
            speak("Вы достигли пункта назначения. Навигация завершена.", sync=True)
            NAV_ACTIVE = False
            write_nav_status(False)
            break

        if len(steps) > 1:
            next_step = steps[1]
            step_dist = int(next_step.get("distance", 0))
            maneuver = next_step.get("maneuver", {})
            direction = maneuver.get("modifier", "straight")

            loc = maneuver.get("location", [0,0])
            loc_str = f"{loc[0]},{loc[1]}"

            # Список молчаливых манёвров живёт в phrases.py вместе с самими
            # формулировками: пока он был выписан здесь строкой, сборщик кэша
            # об этом не знал и заранее синтезировал девять десятков фраз,
            # которые устройство не произносит.
            if is_announced(direction):
                if step_dist < TURN_ANNOUNCE_MAX_DIST and loc_str != last_announced_loc:
                    speak(turn_phrase(step_dist, direction), sync=True)
                    last_announced_loc = loc_str # Запоминаем, что уже сказали

        for _ in range(10):
            if not NAV_ACTIVE: break
            time.sleep(0.5)


def main():
    global NAV_ACTIVE
    print("[Daemon] Инициализация моделей в память...")
    write_nav_status(False)
    # Демон мог упасть с открытым микрофоном — сбрасываем флаг, иначе ядро
    # молчало бы до первого удачного захвата.
    write_mic_open(False)
    # Профиль при старте демона больше не переставляется: его держит
    # bt_keeper (enforce_audio_profile), причём с первой секунды после
    # подключения наушников, тогда как демон поднимается заметно позже и
    # предупреждения об опасности из C++ ядра идут вообще мимо него.

    try:
        with open(ADDRESS_DB_PATH, "r", encoding="utf-8") as f:
            addresses_db = json.load(f)
        address_keys = list(addresses_db.keys())
        model = Model(VOSK_MODEL_PATH)
    except Exception as e:
        print(f"[Daemon] ФАТАЛЬНАЯ ОШИБКА инициализации: {e}")
        try:
            speak("Ошибка инициализации навигационного модуля", sync=True)
        except Exception:
            pass
        sys.exit(1)

    # TCP по loopback, а не AF_UNIX — сознательный выбор: единственное
    # сообщение на нажатие кнопки (main.cpp:send_start_signal), частота
    # — секунды, не кадры. Разница в задержке с AF_UNIX человеку не заметна.
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', SOCKET_PORT))
    server.listen(1)

    print(f"[Daemon] Сервер готов и слушает порт {SOCKET_PORT}...")

    while True:
        try:
            conn, addr = server.accept()
            data = conn.recv(1024).decode('utf-8')

            if data == "START_NAV":
                if NAV_ACTIVE:
                    print("[Daemon] Кнопка нажата во время пути! ОТМЕНА МАРШРУТА.")
                    NAV_ACTIVE = False
                    write_nav_status(False)
                    speak("Маршрут отменен", sync=False)
                else:
                    print("\n[Daemon] Запуск навигации...")
                    NAV_ACTIVE = True
                    write_nav_status(True)
                    threading.Thread(target=navigation_worker, args=(model, addresses_db, address_keys), daemon=True).start()

            conn.close()
        except Exception as e:
            print(f"[Daemon] Ошибка сокета: {e}")

if __name__ == "__main__":
    main()
