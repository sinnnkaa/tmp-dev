#ifndef RKNN_INFERENCE_H
#define RKNN_INFERENCE_H

#include "rknn_api.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <string>

// Разбивка одного infer() по стадиям, для поиска узкого места FPS (см.
// rknn_video_bench). nullptr по умолчанию — main.cpp не платит за замеры.
struct InferTiming {
    double preprocess_ms = 0.0;  // resize + cvtColor (CPU)
    double input_copy_ms = 0.0;  // передача входа NPU: rknn_inputs_set (обычный путь)
                                 // либо rknn_mem_sync (zero-copy, см. RKNNModel::uses_zero_copy)
    double npu_run_ms = 0.0;     // rknn_run (сам инференс на NPU, блокирующий)
    double dequant_ms = 0.0;     // rknn_outputs_get (want_float=1 — деквантование на CPU)
};

class RKNNModel {
public:
    RKNNModel();
    ~RKNNModel();

    bool load(const std::string& model_path);

    // img ДОЛЖЕН быть в BGR (как отдаёт cv::VideoCapture "сырым") — конвертация
    // в RGB для NPU выполняется внутри, рядом с resize(). Не конвертируйте в RGB
    // до вызова: applied дважды подряд BGR2RGB/RGB2BGR отменяют друг друга и
    // модель молча получит BGR вместо RGB.
    //
    // Возвращает 3 дековантованных выхода модели, либо пустой vector (size() != 3),
    // если инференс не удался или реальные размеры выходов не совпали с ожидаемыми —
    // вызывающий код обязан проверять размер результата перед декодированием.
    std::vector<std::vector<float>> infer(const cv::Mat& img, InferTiming* timing = nullptr);

    rknn_context get_ctx();

    // true, если удалось поднять zero-copy вход (резидентный буфер NPU вместо
    // копирования кадра через rknn_inputs_set). Нужен только для диагностики
    // и бенчмарка — на результат инференса не влияет.
    bool uses_zero_copy() const { return zero_copy; }

private:
    // Пытается выделить резидентный входной буфер NPU и привязать его к модели.
    // Возвращает false, если рантайм/модель не дают этого сделать — тогда infer()
    // молча остаётся на обычном пути через rknn_inputs_set.
    bool setup_zero_copy_input();

    rknn_context ctx;

    // Zero-copy вход: буфер, который NPU читает напрямую, а cv::cvtColor пишет
    // в него результат препроцессинга. Экономит копию кадра на каждом кадре
    // (на RK3566 это было 27% времени кадра, см. TESTING.md).
    rknn_tensor_mem* input_mem = nullptr;
    rknn_tensor_attr input_attr{};
    // cv::Mat поверх input_mem->virt_addr — заголовок без владения памятью.
    cv::Mat input_wrap;
    cv::Mat rgb_scratch;    // промежуточный uint8 RGB перед конверсией в fp16
    size_t input_step = 0;  // шаг строки в буфере NPU, байт (может быть > ширины кадра)
    int input_cv_type = 0;  // CV_8UC3 или CV_16FC3 — под тип входа модели
    bool input_is_fp16 = false;
    bool zero_copy = false;
};

#endif
