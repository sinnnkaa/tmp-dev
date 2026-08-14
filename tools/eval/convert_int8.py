"""
Конвертация best_3head.onnx -> INT8 .rknn под RK3566.

Текущая yolo11_final.rknn собрана без квантования (вход и все выходы FP16,
scale=1.0, zp=0 — проверено через rknn_query на плате), поэтому NPU считает
модель в FP16 и rknn_run занимает ~56 мс на кадр. INT8 — основной резерв
скорости; цена — падение точности, которое меряется отдельно на той же
подвыборке val, что и FP16.

mean/std подобраны так, чтобы модель принимала СЫРОЙ uint8-кадр 0..255, как
её кормит боевой C++ (нормализацию делает сам рантайм NPU): ONNX ожидает
вход 0..1, значит std=255.
"""
import sys
from rknn.api import RKNN

ONNX = sys.argv[1]
DATASET_TXT = sys.argv[2]
OUT = sys.argv[3]
QUANT = (len(sys.argv) < 5) or (sys.argv[4] != "fp16")
# normal — быстрый подбор шкал по min/max; mmse — подбор, минимизирующий
# ошибку квантования послойно (конверсия дольше, точность обычно выше).
ALGO = sys.argv[5] if len(sys.argv) > 5 else "normal"

rknn = RKNN(verbose=False)
rknn.config(
    mean_values=[[0, 0, 0]],
    std_values=[[255, 255, 255]],
    target_platform="rk3566",
    quantized_dtype="asymmetric_quantized-8",
    quantized_algorithm=ALGO,
    optimization_level=3,
)

assert rknn.load_onnx(model=ONNX) == 0, "load_onnx failed"
assert rknn.build(do_quantization=QUANT, dataset=DATASET_TXT) == 0, "build failed"
assert rknn.export_rknn(OUT) == 0, "export_rknn failed"
print("saved:", OUT, "quantized:", QUANT)
rknn.release()
