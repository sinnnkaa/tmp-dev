import os
import sys
import time
import json
import difflib
import requests
import subprocess
from vosk import Model, KaldiRecognizer

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


def switch_bt_profile(profile):
    """
    a2dp_sink - качественный звук (микрофон выключен)
    handsfree_head_unit - режим рации (микрофон включен)
    """
    try:
        subprocess.run(
            ["pactl", "set-card-profile", BT_CARD, profile], 
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
    except Exception as e:
        print(f"[BT Error] Не удалось переключить профиль: {e}")


def speak(text, sync=False):
    print(f"[Голос]: {text}")
    command = (
        f'echo "{text}" | '
        f'{PIPER_PATH} --model {PIPER_MODEL} --length_scale 0.85 --output-raw | '
        f'aplay -D default -r 22050 -f S16_LE -t raw -c 1 2>/dev/null'
    )
    if sync:
        subprocess.run(command, shell=True)
    else:
        subprocess.Popen(command, shell=True)


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
            
    if current_num > 0:
        result.append(str(current_num))
        
    return " ".join(result)

def find_coordinates(text, address_keys, addresses_db):
    clean_text = normalize_text(text)
    print(f"[DEBUG] Ищем в базе: '{clean_text}'")
    
    if not clean_text: return None, None
    matches = difflib.get_close_matches(clean_text, address_keys, n=1, cutoff=0.4)
    if matches:
        return matches[0], addresses_db[matches[0]]
    return None, None


def get_current_location():
    try:
        session = gps.gps(mode=gps.WATCH_ENABLE | gps.WATCH_NEWSTYLE)
        for _ in range(10):
            report = session.next()
            if report["class"] == "TPV" and hasattr(report, "lat") and hasattr(report, "lon"):
                return report.lat, report.lon
            time.sleep(0.5)
    except Exception as e:
        print(f"Ошибка GPS: {e}")
    return None, None
    # return (59.860038, 30.234853)

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


def main():   
    print("[BT] Инициализация: сброс наушников в режим A2DP...")
    switch_bt_profile("a2dp_sink")
    time.sleep(1.5)

    with open(ADDRESS_DB_PATH, "r", encoding="utf-8") as f:
        addresses_db = json.load(f)
    address_keys = list(addresses_db.keys())

    if not os.path.exists(VOSK_MODEL_PATH):
        speak("Ошибка: не найдена модель распознавания речи", sync=True)
        return

    model = Model(VOSK_MODEL_PATH)
    rec = KaldiRecognizer(model, 16000)

    speak("Слушаю адрес", sync=True)

    switch_bt_profile("handsfree_head_unit")
    time.sleep(0.5)
    
    cmd = ['ffmpeg', '-loglevel', 'quiet', '-f', 'pulse', '-i', 'default',
           '-ar', '16000', '-ac', '1', '-f', 's16le', '-t', '7', '-']
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    
    user_input = ""
    print("[STT] Говорите...")
    try:
        while True:
            data = proc.stdout.read(4000)
            if not data: break
            if rec.AcceptWaveform(data):
                res = json.loads(rec.Result())
                if res['text']:
                    user_input = res['text']
                    break
    finally:
        proc.terminate()

    time.sleep(0.5)
    switch_bt_profile("a2dp_sink")

    print("[BT] Ожидание стабилизации звука...")
    time.sleep(2.5)

    if not user_input:
        speak("Адрес не распознан", sync=True)
        return

    print(f"Вы сказали: {user_input}")
    
    address_name, end_coords = find_coordinates(user_input, address_keys, addresses_db)
    if not end_coords:
        speak("Не могу найти этот адрес", sync=True)
        return

    print(f"[OSRM] Ищу маршрут до {address_name}")
    
    start_lat, start_lon = get_current_location()
    
    if start_lat is None:
        start_lat, start_lon = 59.9390, 30.3158

    distance, steps = get_route(start_lat, start_lon, end_coords[0], end_coords[1])
    
    if distance is None:
        speak("Ошибка построения маршрута", sync=True)
        return

    dist_m = int(distance)

    full_speech = f"Маршрут до {address_name} построен. Расстояние {dist_m} метров. "

    if len(steps) > 1:
        first_step = steps[1]
        direction = first_step.get("maneuver", {}).get("modifier", "straight")
        step_distance = int(first_step.get("distance", 0))
        direction_ru = direction_to_russian(direction)
        full_speech += f"Двигайтесь {direction_ru} примерно {step_distance} метров."

    speak(full_speech, sync=True)

if __name__ == "__main__":
    main()