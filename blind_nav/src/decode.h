#pragma once
#include <vector>
#include <cstdint>

struct Detection {
    int class_id;
    float score;
    float x, y, w, h;
};

// clipped_out (необязателен) — сколько кандидатов было отброшено фильтром
// "бокс целиком внутри кадра". Счётчик нужен, чтобы измерить цену этого
// фильтра на реальном материале: у близких объектов рамка часто упирается в
// край кадра, и такие детекции теряются целиком — а это как раз самые опасные.
std::vector<Detection> decode(const std::vector<std::vector<float>>& outputs,
                              int input_w, int input_h,
                              int orig_w, int orig_h, float threshold,
                              int* clipped_out = nullptr);

// Открыты для юнит-тестов (см. tests/test_logic.cpp) — сами по себе не
// зависят от OpenCV/RKNN.
float calculate_iou(const Detection& a, const Detection& b);
void apply_nms(std::vector<Detection>& input, float threshold);
