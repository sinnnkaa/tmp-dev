#!/bin/bash

echo "Начинаю захват аудио на Orange Pi..."

SINK_MONITOR=$(pactl info | grep "Default Sink" | awk '{print $3}').monitor

DEFAULT_MIC=$(pactl info | grep "Default Source" | awk '{print $3}')

parecord --device=$SINK_MONITOR --file-format=wav /root/diplom-cpp/systemd/system_audio.wav &
SPEAKER_PID=$!

parecord --device=$DEFAULT_MIC --file-format=wav /root/diplom-cpp/systemd/mic_audio.wav &
MIC_PID=$!

echo "✅ Запись идет! Иди на тест."
echo "Нажми ENTER, когда закончишь прогулку, чтобы сохранить файлы..."
read

kill $SPEAKER_PID
kill $MIC_PID

echo "💾 Файлы system_audio.wav и mic_audio.wav сохранены!"
