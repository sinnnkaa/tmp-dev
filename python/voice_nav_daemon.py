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
import shlex

try:
    import gps
except ImportError:
    print("Ошибка: не установлена библиотека gps.")
    sys.exit(1)

ADDRESS_DB_PATH = "/root/diplom-cpp/blind_nav/map/addresses.json"
OSRM_URL = "http://127.0.0.1:5000/route/v1/foot/"
PIPER_PATH = "/root/diplom-cpp/piper/piper/piper"
PIPER_MODEL = "/root/diplom-cpp/piper/ru_RU-irina-medium.onnx"
VOSK_MODEL_PATH = "/root/diplom-cpp/blind_nav/vosk/model"
BT_CARD = "bluez_card.1C_6E_4C_89_E9_32"
SOCKET_PORT = 9999

# Общая блокировка звукового устройства с C++ ядром (main.cpp), чтобы
# предупреждения об опасности и голосовые инструкции маршрута не звучали
# одновременно поверх друг друга.
AUDIO_LOCK_PATH = "/run/lock/blind_nav_audio.lock"
AUDIO_LOG_PATH = "/root/diplom-cpp/system_audio.raw"
AUDIO_LOG_MAX_BYTES = 20 * 1024 * 1024

MIC_LOG_PATH = "/root/diplom-cpp/mic_audio.raw"
MIC_LOG_MAX_BYTES = 20 * 1024 * 1024

# Файл статуса навигации: C++ ядро (main.cpp) читает его, чтобы подавлять
# предупреждения об опасности ровно на время реального маршрута.
NAV_STATUS_PATH = "/dev/shm/nav_active"

# Порог нечёткого поиска адреса и порог "подряд идущих" сбоев OSRM,
# после которого пользователь получает голосовое предупреждение.
ADDRESS_MATCH_CUTOFF = 0.5
OSRM_FAILURE_WARN_EVERY = 3

NAV_ACTIVE = False


def write_nav_status(active: bool):
    """Синхронизирует статус навигации с C++ ядром через общий файл в tmpfs."""
    try:
        with open(NAV_STATUS_PATH, "w") as f:
            f.write("1" if active else "0")
    except Exception as e:
        print(f"[NavStatus] Не удалось записать статус: {e}")


def cap_log_file(path, max_bytes):
    """Обрезает файл, если он превысил лимит — защита от бесконечного роста на SD-карте."""
    try:
        if os.path.exists(path) and os.path.getsize(path) > max_bytes:
            open(path, "w").close()
    except Exception as e:
        print(f"[LogCap] Не удалось обрезать {path}: {e}")


def switch_bt_profile(profile):
    try:
        custom_env = os.environ.copy()
        custom_env["PULSE_RUNTIME_PATH"] = "/run/user/0/pulse"
        subprocess.run(
            ["pactl", "set-card-profile", BT_CARD, profile],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=custom_env
        )
    except Exception as e:
        print(f"[BT Error] Не удалось переключить профиль: {e}")


def speak(text, sync=False):
    print(f"[Голос]: {text}")
    safe_text = shlex.quote(text)
    inner = (
        f'[ -f {AUDIO_LOG_PATH} ] && [ $(stat -c%s {AUDIO_LOG_PATH}) -gt {AUDIO_LOG_MAX_BYTES} ] && : > {AUDIO_LOG_PATH}; '
        f'echo {safe_text} | '
        f'{PIPER_PATH} --model {PIPER_MODEL} --length_scale 0.85 --output-raw | '
        f'tee -a {AUDIO_LOG_PATH} | '
        f'aplay -D default -r 22050 -f S16_LE -t raw -c 1 2>/dev/null'
    )

    custom_env = os.environ.copy()
    custom_env["PULSE_RUNTIME_PATH"] = "/run/user/0/pulse"

    # Аргументы передаются списком (а не одной shell-строкой), чтобы safe_text
    # не ломался при повторном оборачивании во flock -c '...'.
    if sync:
        subprocess.run(["flock", AUDIO_LOCK_PATH, "-c", inner], env=custom_env)
    else:
        subprocess.Popen(["flock", AUDIO_LOCK_PATH, "-c", inner], env=custom_env)


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


def direction_to_russian(direction):
    mapping = {
        "left": "налево", "right": "направо", "straight": "прямо",
        "slight left": "немного левее", "slight right": "немного правее",
        "sharp left": "резко налево", "sharp right": "резко направо", "uturn": "развернитесь"
    }
    return mapping.get(direction, "прямо")


def listen_and_transcribe(model, seconds):
    """Захватывает звук с гарнитуры и возвращает распознанный текст (или пустую строку)."""
    cap_log_file(MIC_LOG_PATH, MIC_LOG_MAX_BYTES)

    rec = KaldiRecognizer(model, 16000)
    cmd = ['ffmpeg', '-loglevel', 'quiet', '-f', 'pulse', '-i', 'default',
           '-ar', '16000', '-ac', '1', '-f', 's16le', '-t', str(seconds), '-']

    custom_env = os.environ.copy()
    custom_env["PULSE_RUNTIME_PATH"] = "/run/user/0/pulse"
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, env=custom_env)

    text = ""
    try:
        while True:
            data = proc.stdout.read(4000)
            if not data: break

            with open(MIC_LOG_PATH, "ab") as f_mic:
                f_mic.write(data)
            if rec.AcceptWaveform(data):
                res = json.loads(rec.Result())
                if res['text']:
                    text = res['text']
                    break
    finally:
        proc.terminate()

    return text


def confirm_address(model, address_name):
    """Голосовое подтверждение найденного адреса перед стартом маршрута — защита
    от того, что нечёткий поиск (Левенштейн/difflib) подберёт похожий, но не тот адрес."""
    switch_bt_profile("handsfree_head_unit")
    time.sleep(0.5)
    speak(f"Вы имели в виду {address_name}? Скажите да или нет.", sync=True)

    answer = listen_and_transcribe(model, 4)

    switch_bt_profile("a2dp_sink")
    time.sleep(1.0)

    # Сравнение по целым словам, а не подстрокой: "да" — подстрока в "туда",
    # "погода", "надо" и т.п., из-за чего отказ ("нет, не туда") ошибочно
    # засчитывался как подтверждение маршрута.
    return "да" in answer.lower().split()


def navigation_worker(model, addresses_db, address_keys):
    global NAV_ACTIVE

    speak("Слушаю адрес", sync=True)
    switch_bt_profile("handsfree_head_unit")
    time.sleep(0.5)

    print("[STT] Говорите...")
    user_input = listen_and_transcribe(model, 7)

    switch_bt_profile("a2dp_sink")
    time.sleep(2.0)

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

    speak(f"Маршрут до {address_name} построен. Расстояние {int(dist)} метров.", sync=True)

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

            if direction not in ["straight", "slight left", "slight right"]:
                if step_dist < 30 and loc_str != last_announced_loc:
                    dir_ru = direction_to_russian(direction)
                    speak(f"Через {step_dist} метров {dir_ru}", sync=True)
                    last_announced_loc = loc_str # Запоминаем, что уже сказали

        for _ in range(10):
            if not NAV_ACTIVE: break
            time.sleep(0.5)


def main():
    global NAV_ACTIVE
    print("[Daemon] Инициализация моделей в память...")
    write_nav_status(False)
    switch_bt_profile("a2dp_sink")
    time.sleep(1.0)

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
