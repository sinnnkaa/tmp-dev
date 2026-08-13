# RKNN runtime (вендорено в репозиторий)

Раньше сборка C++ ядра и `pip install` зависели от файлов вне репозитория —
`/root/diplom/rknn-toolkit2/...` на плате. Это ломало требование «клонировать
на плату без доработок»: если такого каталога на плате не было, ни сборка,
ни `pip install -r requirements.txt` не проходили.

Здесь лежат ровно те файлы, которые реально нужны для сборки/тестов, взятые
из официального репозитория [airockchip/rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2),
тег **v2.3.2** — та же версия, что зашита в `blind_nav/model/yolo11_final.rknn`
(проверено через `strings` на файле модели).

```
third_party/rknn/
├── include/          # rknn_api.h и сопутствующие заголовки (C API)
├── lib/aarch64/       # librknnrt.so — рантайм NPU для aarch64 (RK3566)
└── python/            # rknn_toolkit_lite2 wheel под Python 3.10 (cp310)
```

- `include/` + `lib/aarch64/librknnrt.so` — используются `blind_nav/CMakeLists.txt`
  при сборке C++ ядра и тестового инструмента (см. `blind_nav/tests/`).
- `python/*.whl` — используется `python/requirements.txt` для `rknn-toolkit-lite2`
  (лёгкий инференс-only пакет для запуска `.rknn` на самой плате из Python,
  не путать с полным `rknn-toolkit2` — тот нужен только на PC для конвертации
  моделей и сюда не вендорится, ставится через `pip install rknn-toolkit2==2.3.2`
  при необходимости).

Если на плате Python другой версии (не 3.10) — нужен другой `.whl` из того же
релиза (cp37/cp38/cp39/cp311/cp312 доступны в upstream-репозитории по тому же
тегу v2.3.2), текущий вендорится под cp310, т.к. именно под неё был собран
исходный `requirements.txt` (снят через `pip freeze` на плате).
