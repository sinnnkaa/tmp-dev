#!/bin/bash

sleep 5

cd /root/diplom-cpp/blind_nav/build

echo "--- Запуск C++ модуля зрения ---"

exec ./blind_nav
