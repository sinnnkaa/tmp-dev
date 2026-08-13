#include "rknn_inference.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>

RKNNModel::RKNNModel() : ctx(0) {}

RKNNModel::~RKNNModel() {
    if (ctx > 0) rknn_destroy(ctx);
}

static unsigned char* load_model_file(const char* filename, int* model_size) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return nullptr;
    *model_size = file.tellg();
    file.seekg(0, std::ios::beg);
    unsigned char* data = (unsigned char*)malloc(*model_size);
    file.read((char*)data, *model_size);
    file.close();
    return data;
}

bool RKNNModel::load(const std::string& model_path) {
    int model_len = 0;
    unsigned char* model_data = load_model_file(model_path.c_str(), &model_len);
    if (!model_data) {
        std::cerr << "Failed to load model file: " << model_path << std::endl;
        return false;
    }

    int ret = rknn_init(&ctx, model_data, model_len, 0, NULL);
    free(model_data);

    if (ret < 0) {
        std::cerr << "rknn_init error ret=" << ret << std::endl;
        return false;
    }

    rknn_sdk_version version;
    rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    std::cout << "RKNN SDK Version: " << version.api_version << std::endl;

    return true;
}

std::vector<std::vector<float>> RKNNModel::infer(const cv::Mat& img) {
    cv::Mat resized, rgb;
    cv::resize(img, resized, cv::Size(512, 512));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = 512 * 512 * 3;
    inputs[0].buf = rgb.data;

    int ret = rknn_inputs_set(ctx, 1, inputs);
    if (ret < 0) {
        std::cerr << "rknn_inputs_set error ret=" << ret << std::endl;
        return {};
    }

    ret = rknn_run(ctx, nullptr);
    if (ret < 0) {
        std::cerr << "rknn_run error ret=" << ret << std::endl;
        return {};
    }

    rknn_output outputs[3];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1;
    outputs[1].want_float = 1;
    outputs[2].want_float = 1;

    ret = rknn_outputs_get(ctx, 3, outputs, nullptr);
    if (ret < 0) {
        std::cerr << "rknn_outputs_get error ret=" << ret << std::endl;
        return {};
    }

    // Ожидаемые размеры 3-х выходов для imgsz=512 (архитектура yolo11_final.rknn):
    // P3 (мелкие объекты): 74 * 64 * 64 = 303104
    // P4 (средние объекты): 74 * 32 * 32 = 75776
    // P5 (крупные объекты): 74 * 16 * 16 = 18944
    const size_t expected_sizes[3] = {74 * 64 * 64, 74 * 32 * 32, 74 * 16 * 16};

    std::vector<std::vector<float>> dequantized_outputs(3);
    bool valid = true;

    for (int i = 0; i < 3; i++) {
        if (!outputs[i].buf) {
            std::cerr << "rknn output " << i << " buffer is null" << std::endl;
            valid = false;
            continue;
        }
        size_t actual_floats = outputs[i].size / sizeof(float);
        if (actual_floats != expected_sizes[i]) {
            // Реальный размер выхода не совпадает с ожидаемым (например, подставлена
            // модель с другой архитектурой) — читать буфер по старому размеру нельзя,
            // это привело бы к чтению/переполнению за пределами выделенной памяти.
            std::cerr << "rknn output " << i << " size mismatch: expected "
                      << expected_sizes[i] << " floats, got " << actual_floats << std::endl;
            valid = false;
            continue;
        }
        float* float_buf = (float*)outputs[i].buf;
        dequantized_outputs[i].assign(float_buf, float_buf + actual_floats);
    }

    rknn_outputs_release(ctx, 3, outputs);

    if (!valid) {
        return {};
    }
    return dequantized_outputs;
}

rknn_context RKNNModel::get_ctx() {
    return ctx;
}
