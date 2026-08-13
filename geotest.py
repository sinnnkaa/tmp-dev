import requests
import sys

def test_osrm_route(start_lat, start_lon, end_lat, end_lon):
    print("🔄 Отправка запроса к локальному серверу OSRM...")
    
    # ВНИМАНИЕ: OSRM ожидает формат {lon},{lat}
    # Используем профиль foot (пешеходный)
    url = f"http://127.0.0.1:5000/route/v1/foot/{start_lon},{start_lat};{end_lon},{end_lat}"
    
    # Параметр steps=true заставляет OSRM вернуть массив маневров (поворотов)
    params = {
        "steps": "true",
        "geometries": "geojson",
        "overview": "full"
    }

    try:
        response = requests.get(url, params=params, timeout=5)
        response.raise_for_status()
        data = response.json()

        if data.get("code") != "Ok":
            print(f"❌ Ошибка OSRM: {data.get('code')}")
            return

        routes = data.get("routes", [])
        if not routes:
            print("❌ Сервер ответил Ok, но маршрут не найден (возможно, точки слишком далеко от дорог).")
            return

        route = routes[0]
        total_distance = route['distance']
        total_duration = route['duration']

        print(f"✅ Маршрут успешно построен!")
        print(f"📍 Старт: {start_lat}, {start_lon}")
        print(f"🏁 Финиш: {end_lat}, {end_lon}")
        print(f"📏 Общая дистанция: {total_distance:.1f} м.")
        print(f"⏱  Расчетное время: {total_duration / 60:.1f} мин.\n")
        
        print("-" * 50)
        print("📋 ПОШАГОВЫЙ ПЛАН МАНЕВРОВ:")
        print("-" * 50)

        # Извлекаем "шаги" маршрута из первого (и единственного) участка (leg)
        steps = route['legs'][0]['steps']

        for i, step in enumerate(steps):
            maneuver = step['maneuver']
            
            # OSRM возвращает location как [Долгота, Широта]. Переворачиваем обратно для вывода.
            m_lon, m_lat = maneuver['location']
            
            maneuver_type = maneuver['type']
            modifier = maneuver.get('modifier', '')
            name = step.get('name', 'Без названия (двор/тропинка)')
            step_distance = step['distance']

            # Формируем читаемую инструкцию
            action_ru = translate_maneuver(maneuver_type, modifier)
            
            print(f"[{i+1}] Координаты маневра: {m_lat:.6f}, {m_lon:.6f}")
            print(f"    Действие: {action_ru} -> ул. {name}")
            
            if step_distance > 0:
                print(f"    Двигаться: {step_distance:.1f} м. до следующей точки.\n")
            else:
                print("    🏁 Вы достигли пункта назначения!\n")

    except requests.exceptions.ConnectionError:
        print("❌ ОШИБКА: Не удалось подключиться к OSRM.")
        print("Убедитесь, что сервер запущен командой:")
        print("osrm-routed --algorithm ch map.osrm")
    except Exception as e:
        print(f"❌ Произошла ошибка: {e}")

def translate_maneuver(m_type, modifier):
    """Простой переводчик терминов OSRM на русский язык"""
    directions = {
        'left': 'налево',
        'right': 'направо',
        'slight left': 'левее',
        'slight right': 'правее',
        'straight': 'прямо',
        'uturn': 'разворот'
    }
    
    types = {
        'turn': 'Поверните',
        'new name': 'Продолжайте движение',
        'depart': 'Начните движение',
        'arrive': 'Прибытие',
        'continue': 'Продолжайте движение',
        'roundabout': 'Круговое движение'
    }
    
    action = types.get(m_type, m_type)
    direction = directions.get(modifier, modifier)
    
    if m_type == 'depart':
        return f"{action} {direction}".strip()
    elif m_type == 'arrive':
        return action
    else:
        return f"{action} {direction}"

if __name__ == "__main__":
    # Координаты Кировского/Красносельского района СПб
    START_LAT, START_LON = 59.842879, 30.149940
    END_LAT, END_LON = 59.841740, 30.146554

    test_osrm_route(START_LAT, START_LON, END_LAT, END_LON)