## BlindNav: Edge AI Navigation System
---
BlindNav — это программно-аппаратный комплекс автономной навигации для людей с нарушениями зрения. Система работает полностью Offline (Edge AI), объединяя компьютерное зрение на NPU, спутниковую навигацию и нейросетевой синтез речи в едином носимом устройстве.

Разработано в рамках выпускной квалификационной работы (ВКР) в Санкт-Петербургском государственном университете аэрокосмического приборостроения (ГУАП).

### Основные возможности

1. Hardware-Accelerated Vision: Инференс модели YOLOv11-Nano (INT8) на NPU Rockchip RK3566 — 25 FPS на потоке кадров, ~18 FPS сквозного конвейера с камерой. Замеры скорости и точности — в [METRICS.md](METRICS.md).

2. Семантический анализ среды: Распознавание 10 классов городских объектов (люди, транспорт, инфраструктура) с расчетом дистанции и приоритезацией опасности.

3. Локальный Neural TTS: Мгновенное оповещение через нейросетевой движок Piper (ONNX).

4. Offline Геопозиционирование: Маршрутизация на базе OSRM и локальных OSM-графов без доступа к интернету.

5. Headless-ready: Автоматизация запуска всего комплекса через systemd.

### Техническая реализация

| Компонент | Технологии |
| :--- | :--- |
| **CV Core** | C++17, RKNN API, OpenCV |
| **AI Model** | YOLOv11-Nano (квантованная) |
| **Routing** | OSRM (в Docker), OpenStreetMap (`.pbf`) |
| **TTS/STT** | Piper (ONNX), Vosk |
| **Deployment** | Docker (только OSRM), systemd (остальные сервисы) |

### Архитектура комплекса

```text
diplom-cpp/
├── blind_nav/
│   ├── src/             # C++ ядро: камера -> NPU -> оценка угрозы
│   ├── model/           # Скомпилированные веса YOLO (INT8)
│   ├── map/             # Оффлайн навигация (SPB OSM-граф)
│   └── build/           # Бинарные файлы и systemd-службы
├── python/
│   └── voice_nav_daemon.py     # Код службы макронавигации
│
├── piper/               # Нейросетевой синтез речи
└── vosk/                # Локальное распознавание команд
```
### Быстрый старт (Orange Pi, чистая плата)

```bash
git clone <repo> /root/diplom-cpp && cd /root/diplom-cpp

# 1. Системные сервисы (systemd-юниты + автозапуск)
sudo bash deploy/install.sh

# 2. Сборка C++ ядра
cd blind_nav && mkdir -p build && cd build
cmake .. && make -j4
cd ../..

# 3. OSRM (Docker) — граф уже в git, ничего собирать не нужно
sudo systemctl start osrm.service

# 4. Остальные сервисы
sudo systemctl start bt_keeper.service nav_daemon.service blind_nav_main.service
```

Подробности и чек-лист ручной проверки — в [TESTING.md](TESTING.md); сборка
и пересборка графа OSRM — в [deploy/osrm/README.md](deploy/osrm/README.md).

---
Проект демонстрирует эффективность применения NPU в мобильных встраиваемых системах для решения социально-значимых задач.
