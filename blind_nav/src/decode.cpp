#include "decode.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <iostream>

inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

float calculate_iou(const Detection& a, const Detection& b) {
    float x1 = std::max(a.x, b.x); float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w); float y2 = std::min(a.y + a.h, b.y + b.h);
    float w = std::max(0.0f, x2 - x1); float h = std::max(0.0f, y2 - y1);
    return (w * h) / (a.w * a.h + b.w * b.h - w * h + 1e-6f);
}

void apply_nms(std::vector<Detection>& input, float threshold) {
    std::sort(input.begin(), input.end(), [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<bool> removed(input.size(), false);
    std::vector<Detection> result;
    for (size_t i = 0; i < input.size(); i++) {
        if (removed[i]) continue;
        result.push_back(input[i]);
        for (size_t j = i + 1; j < input.size(); j++) {
            if (input[i].class_id == input[j].class_id && calculate_iou(input[i], input[j]) > threshold) removed[j] = true;
        }
    }
    input = result;
}

inline float compute_dfl(const float* tensor, int step) {
    float max_val = -10000.0f;
    for (int i = 0; i < 16; i++) {
        max_val = std::max(max_val, tensor[i * step]);
    }
    
    float sum = 0.0f;
    float exp_vals[16];
    for (int i = 0; i < 16; i++) {
        exp_vals[i] = std::exp(tensor[i * step] - max_val);
        sum += exp_vals[i];
    }
    
    float res = 0.0f;
    for (int i = 0; i < 16; i++) {
        res += (exp_vals[i] / sum) * i;
    }
    return res;
}

void decode_single_output(const float* output, int grid_size, int stride, 
                          float conf_thresh, std::vector<Detection>& proposals) {
    int num_classes = 10;
    int reg_max = 16;
    int area = grid_size * grid_size;
    int channels = 4 * reg_max + num_classes;

    for (int i = 0; i < area; i++) {
        int grid_y = i / grid_size;
        int grid_x = i % grid_size;
        
        float max_conf = -1.0f;
        int class_id = -1;

        for (int c = 0; c < num_classes; c++) {
            float s = output[(64 + c) * area + i]; 
            float conf = sigmoid(s);
            if (conf > max_conf) {
                max_conf = conf;
                class_id = c;
            }
        }

        if (max_conf > conf_thresh) {
            float dfl[4]; 
            for (int k = 0; k < 4; k++) {
                dfl[k] = compute_dfl(output + (k * 16) * area + i, area);
            }
            float x0 = (grid_x + 0.5f - dfl[0]) * stride;
            float y0 = (grid_y + 0.5f - dfl[1]) * stride;
            float x1 = (grid_x + 0.5f + dfl[2]) * stride;
            float y1 = (grid_y + 0.5f + dfl[3]) * stride;

            proposals.push_back({class_id, max_conf, x0, y0, x1 - x0, y1 - y0});
        }
    }
}
std::vector<Detection> decode(const std::vector<std::vector<float>>& outputs, 
                              int input_w, int input_h,
                              int orig_w, int orig_h, float threshold) {
    std::vector<Detection> all_dets;
    
    int strides[] = {8, 16, 32};
    int grid_sizes[] = {input_w / 8, input_w / 16, input_w / 32};

    for (int i = 0; i < 3; i++) {
        decode_single_output(outputs[i].data(), grid_sizes[i], strides[i], threshold, all_dets);
    }

    float scale_x = (float)orig_w / (float)input_w; 
    float scale_y = (float)orig_h / (float)input_h;

    std::vector<Detection> final_dets;
    for (auto& det : all_dets) {
        float real_x = det.x * scale_x;
        float real_y = det.y * scale_y;
        float real_w = det.w * scale_x;
        float real_h = det.h * scale_y;

        if (real_x >= 0 && real_y >= 0 && (real_x + real_w) <= orig_w && (real_y + real_h) <= orig_h) {
            final_dets.push_back({det.class_id, det.score, real_x, real_y, real_w, real_h});
        }
    }

    apply_nms(final_dets, 0.45f);
    
    return final_dets;
}
