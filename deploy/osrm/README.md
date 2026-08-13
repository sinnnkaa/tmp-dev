# OSRM в Docker

Поднимает `osrm-routed` на `127.0.0.1:5000` — туда стучится
`python/voice_nav_daemon.py` (`OSRM_URL`) за маршрутами и пошаговыми
инструкциями. Раньше этот процесс нигде не был описан и не имел
автозапуска/автовосстановления, в отличие от остальных сервисов системы —
теперь под тем же systemd-supervision, что и всё остальное (`osrm.service`).

## Быстрый старт (граф уже собран, обычный случай)

Обработанный граф (`blind_nav/map/spb.osrm*`, алгоритм MLD) уже лежит в
git — ничего собирать не нужно, просто поднять сервер:

```bash
cd deploy/osrm
docker compose up -d
curl "http://127.0.0.1:5000/route/v1/foot/30.315,59.939;30.320,59.941?steps=true"
```

Через `deploy/install.sh` это делает `osrm.service` (`systemctl enable/start
osrm.service`) — контейнер поднимается при загрузке платы и перезапускается
докером при падении (`restart: unless-stopped` в `docker-compose.yml`).

Требуется Docker с поддержкой Compose v2 (`docker compose`, не `docker-compose`).
Официальный образ `ghcr.io/project-osrm/osrm-backend` мультиархитектурный
(amd64 + arm64) — на плате (aarch64) тянется родной образ, без эмуляции.

## Пересборка графа из `map/spb.pbf` (если обновилась карта)

Три шага тем же образом, что раздаёт `osrm-routed` — профиль `foot` (пеший),
он же использовался при первой сборке (`voice_nav_daemon.py` ходит в
`/route/v1/foot/`):

```bash
cd <корень репозитория>
docker run --rm -v "$(pwd)/map:/data" -v "$(pwd)/blind_nav/map:/out" \
  ghcr.io/project-osrm/osrm-backend osrm-extract -p /opt/foot.lua /data/spb.pbf

# osrm-extract пишет результат рядом с исходником (/data), переносим в /out —
# в blind_nav/map/ лежит только обработанный граф, сырой .pbf там не нужен
cp map/spb.osrm* blind_nav/map/

docker run --rm -v "$(pwd)/blind_nav/map:/data" \
  ghcr.io/project-osrm/osrm-backend osrm-partition /data/spb.osrm
docker run --rm -v "$(pwd)/blind_nav/map:/data" \
  ghcr.io/project-osrm/osrm-backend osrm-customize /data/spb.osrm

docker compose -f deploy/osrm/docker-compose.yml restart
```

`map/spb.pbf` — сырой источник (OSM-экстракт Санкт-Петербурга), не трогаем
после экстракции. `blind_nav/map/spb.osrm*` — то, что реально грузит
`osrm-routed`; именно эти файлы примонтированы в контейнер.
