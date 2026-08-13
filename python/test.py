import onnxruntime as ort
import cv2
import numpy as np

# Загрузка модели
session = ort.InferenceSession("/root/diplom-cpp/weights/best.onnx")

# Загрузка оригинального фото
img_path = "/root/diplom-cpp/blind_nav/-0GQmYRienNVqEKiQ0Mkyw.jpg"
original_img = cv2.imread(img_path)
if original_img is None:
    print("Ошибка: Картинка не найдена!")
    exit()

h_orig, w_orig = original_img.shape[:2]

# Подготовка (YOLO ждет 512x512)
img_resized = cv2.resize(original_img, (512, 512))
img_rgb = cv2.cvtColor(img_resized, cv2.COLOR_BGR2RGB)
blob = img_rgb.transpose(2, 0, 1)
blob = np.expand_dims(blob, axis=0).astype(np.float32) / 255.0

# Инференс
outputs = session.run(None, {session.get_inputs()[0].name: blob})
output = outputs[0][0]  # Убираем лишнюю размерность -> [14, 5376]

# Параметры
threshold = 0.5
num_classes = 10

# Декодирование
# output[0:4] - координаты (cx, cy, w, h)
# output[4:14] - скоры классов
boxes = output[:4, :].T        # [5376, 4]
scores = output[4:, :].T       # [5376, 10]

# Ищем максимальный скор для каждого анкора
class_ids = np.argmax(scores, axis=1)
confidences = np.max(scores, axis=1)

# Фильтр по порогу
mask = confidences > threshold
boxes = boxes[mask]
confidences = confidences[mask]
class_ids = class_ids[mask]

print(f"Найдено объектов: {len(boxes)}")

for i in range(len(boxes)):
    cx, cy, w, h = boxes[i]
    conf = confidences[i]
    cls = class_ids[i]

    # Пересчет координат из 512x512 в оригинальный размер
    # (Упрощенно, без учета Letterbox пока что)
    x1 = int((cx - w/2) * w_orig / 512)
    y1 = int((cy - h/2) * h_orig / 512)
    x2 = int((cx + w/2) * w_orig / 512)
    y2 = int((cy + h/2) * h_orig / 512)

    # Рисуем
    cv2.rectangle(original_img, (x1, y1), (x2, y2), (0, 255, 0), 2)
    label = f"ID:{cls} {conf:.2f}"
    cv2.putText(original_img, label, (x1, y1 - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)
    
    print(f"Объект: Класс {cls}, Уверенность {conf:.4f} в [{x1}, {y1}, {x2}, {y2}]")

# Сохранение
cv2.imwrite("onnx_result.jpg", original_img)
print("Результат сохранен в onnx_result.jpg")

