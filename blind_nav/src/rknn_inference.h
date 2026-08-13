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
    double input_copy_ms = 0.0;  // rknn_inputs_set (копирование в буфер NPU)
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

private:
    rknn_context ctx;
};

#endif
