cmake_minimum_required(VERSION 3.10)
project(blind_nav)

set(CMAKE_CXX_STANDARD 17)

# Без явного типа сборки CMake по умолчанию не добавляет флаги оптимизации
# (де-факто эквивалент -O0) — критично для конвейера реального времени
# (захват кадра, препроцессинг, декодирование NMS на каждый кадр).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# RKNN
# Рантайм вендорен прямо в репозиторий (third_party/rknn/, см. README там) —
# раньше путь указывал на /root/diplom/rknn-toolkit2 вне репозитория, из-за
# чего сборка ломалась на чистом клоне без ручной подготовки платы.
set(RKNN_API_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rknn/lib/aarch64/librknnrt.so)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rknn/include
)

# OpenCV
find_package(OpenCV REQUIRED)

add_executable(blind_nav
    src/main.cpp
    src/rknn_inference.cpp
    src/decode.cpp
    src/threat_logic.cpp
)

target_link_libraries(blind_nav
    ${OpenCV_LIBS}
    ${RKNN_API_PATH}
)

# Юнит-тесты для decode.cpp/threat_logic.cpp — чистая логика без OpenCV/RKNN,
# собирается отдельным лёгким бинарником без аппаратных зависимостей.
# Запуск: cmake -DBUILD_TESTS=ON .. && make && ctest --output-on-failure
option(BUILD_TESTS "Собрать юнит-тесты чистой логики (decode/threat_logic)" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_executable(blind_nav_tests
        tests/test_logic.cpp
        src/decode.cpp
        src/threat_logic.cpp
    )
    add_test(NAME blind_nav_tests COMMAND blind_nav_tests)
endif()

# Инструмент для проверки реального NPU-инференса на статичных фото — нужен
# на плате без камеры (main.cpp читает кадры только с cv::VideoCapture).
# Требует настоящий librknnrt.so, поэтому собирается только по явному флагу
# и обычно только на самой плате (aarch64 + подключённый NPU).
# Запуск: cmake -DBUILD_IMAGE_TEST=ON .. && make rknn_image_test
#         ./rknn_image_test ../model/yolo11_final.rknn фото1.jpg фото2.jpg
option(BUILD_IMAGE_TEST "Собрать rknn_image_test — прогон инференса на статичных фото" OFF)
if(BUILD_IMAGE_TEST)
    add_executable(rknn_image_test
        tests/rknn_image_test.cpp
        src/rknn_inference.cpp
        src/decode.cpp
        src/threat_logic.cpp
    )
    target_link_libraries(rknn_image_test
        ${OpenCV_LIBS}
        ${RKNN_API_PATH}
    )
endif()

# Замер реального FPS боевого пайплайна на видео (не на одном кадре) с
# разбивкой по стадиям (препроцессинг / копирование в NPU / сам инференс /
# деквантование / decode()) — для поиска узкого места производительности.
# Запуск: cmake -DBUILD_VIDEO_BENCH=ON .. && make rknn_video_bench
#         ./rknn_video_bench ../model/yolo11_final.rknn видео.mp4
option(BUILD_VIDEO_BENCH "Собрать rknn_video_bench — замер FPS на видео с разбивкой по стадиям" OFF)
if(BUILD_VIDEO_BENCH)
    add_executable(rknn_video_bench
        tests/rknn_video_bench.cpp
        src/rknn_inference.cpp
        src/decode.cpp
    )
    target_link_libraries(rknn_video_bench
        ${OpenCV_LIBS}
        ${RKNN_API_PATH}
    )
endif()
