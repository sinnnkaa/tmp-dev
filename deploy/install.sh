#!/bin/bash
# Разворачивает системные части BlindNav после git clone на Orange Pi:
# systemd-юниты и bt_keeper.sh. Запускать один раз от root, после того как
# репозиторий склонирован ровно в /root/diplom-cpp — все *.service и
# python-скрипты используют этот путь как абсолютный.
set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$REPO_DIR" != "/root/diplom-cpp" ]; then
    echo "ВНИМАНИЕ: репозиторий склонирован не в /root/diplom-cpp (сейчас: $REPO_DIR)."
    echo "systemd-юниты и voice_nav_daemon.py используют абсолютные пути"
    echo "/root/diplom-cpp/... — переклонируйте туда либо правьте пути вручную."
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Запустите от root: sudo bash deploy/install.sh"
    exit 1
fi

echo "==> Копирую systemd-юниты в /etc/systemd/system/"
cp "$REPO_DIR"/systemd/*.service /etc/systemd/system/

echo "==> Симлинкую /root/bt_keeper.sh -> $REPO_DIR/systemd/bt_keeper.sh"
ln -sf "$REPO_DIR/systemd/bt_keeper.sh" /root/bt_keeper.sh

echo "==> systemctl daemon-reload"
systemctl daemon-reload

echo "==> Включаю автозапуск сервисов (без старта прямо сейчас)"
systemctl enable bt_keeper.service nav_daemon.service blind_nav_main.service osrm.service

cat <<'EOF'

Готово. Что нужно сделать вручную перед первым запуском:
  1. Собрать C++ ядро:
       cd blind_nav && mkdir -p build && cd build
       cmake .. && make -j4
  2. Проверить, что модель на месте: blind_nav/model/yolo11_final.rknn
  3. Проверить камеру, микрофон/наушники, GPS.
  4. Поставить Docker (нужен для osrm.service) — см. deploy/osrm/README.md.
  5. Запустить сервисы:
       systemctl start osrm.service bt_keeper.service nav_daemon.service blind_nav_main.service

См. TESTING.md — чек-лист ручной проверки на плате.
EOF
