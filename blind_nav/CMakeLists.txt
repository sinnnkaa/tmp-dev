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
# link_directories(/usr/lib)

set(RKNN_API_PATH /root/diplom/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so)

include_directories(
    /root/diplom/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/include
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
