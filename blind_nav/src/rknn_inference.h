#ifndef RKNN_INFERENCE_H
#define RKNN_INFERENCE_H

#include "rknn_api.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <string>

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
    std::vector<std::vector<float>> infer(const cv::Mat& img);

    rknn_context get_ctx();

private:
    rknn_context ctx;
};

#endif
