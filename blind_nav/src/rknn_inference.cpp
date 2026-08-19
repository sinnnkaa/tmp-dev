#include "rknn_inference.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <chrono>

// Входное разрешение обеих моделей (yolo11_int8.rknn — боевая,
// yolo11_final.rknn — прежняя FP16): геометрия у них одинаковая.
static const int INPUT_W = 512;
static const int INPUT_H = 512;
static const int INPUT_C = 3;

RKNNModel::RKNNModel() : ctx(0) {}

RKNNModel::~RKNNModel() {
    if (input_mem && ctx > 0) rknn_destroy_mem(ctx, input_mem);
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

    zero_copy = setup_zero_copy_input();
    std::cout << "Вход NPU: " << (zero_copy ? "zero-copy (резидентный буфер)"
                                            : "rknn_inputs_set (копирование)") << std::endl;

    return true;
}

bool RKNNModel::setup_zero_copy_input() {
    // Аварийный выключатель для замеров A/B (см. TESTING.md): позволяет сравнить
    // оба пути передачи входа на одном и том же бинарнике и одном прогреве платы.
    if (const char* off = std::getenv("BLIND_NAV_NO_ZERO_COPY")) {
        if (off[0] == '1') return false;
    }

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) < 0 || io_num.n_input != 1) {
        return false;
    }

    memset(&input_attr, 0, sizeof(input_attr));
    input_attr.index = 0;
    if (rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &input_attr, sizeof(input_attr)) < 0) {
        return false;
    }

    // Тип входа спрашивается у самой модели, а не предполагается: боевая
    // yolo11_int8.rknn принимает UINT8, прежняя yolo11_final.rknn — FP16, и
    // переключение MODEL_PATH в main.cpp не должно требовать правок здесь.
    // Для FP16-модели rknn_inputs_set с type=UINT8 каждый кадр гонял бы
    // конверсию uint8->fp16 на 786432 значения — это и есть те 25 мс; поэтому
    // конвертируем сами (OpenCV, NEON) прямо в буфер NPU.
    if (input_attr.fmt != RKNN_TENSOR_NHWC) return false;
    if (input_attr.type == RKNN_TENSOR_FLOAT16) {
        input_is_fp16 = true;
    } else if (input_attr.type == RKNN_TENSOR_UINT8) {
        input_is_fp16 = false;
    } else {
        return false;
    }
    if (input_attr.n_dims != 4) return false;
    if ((int)input_attr.dims[1] != INPUT_H || (int)input_attr.dims[2] != INPUT_W ||
        (int)input_attr.dims[3] != INPUT_C) {
        return false;
    }

    input_mem = rknn_create_mem(ctx, input_attr.size_with_stride);
    if (!input_mem || !input_mem->virt_addr) {
        input_mem = nullptr;
        return false;
    }

    if (rknn_set_io_mem(ctx, input_mem, &input_attr) < 0) {
        rknn_destroy_mem(ctx, input_mem);
        input_mem = nullptr;
        return false;
    }

    // w_stride == 0 означает "шаг строки равен ширине" (см. rknn_api.h). Если
    // рантайм выравнивает строки, cv::Mat должен знать реальный шаг в байтах,
    // иначе препроцессинг положит пиксели со сдвигом.
    uint32_t w_stride = input_attr.w_stride ? input_attr.w_stride : (uint32_t)INPUT_W;
    size_t elem_size = input_is_fp16 ? 2 : 1;
    size_t step = (size_t)w_stride * INPUT_C * elem_size;
    if (step * INPUT_H > input_attr.size_with_stride) {
        rknn_destroy_mem(ctx, input_mem);
        input_mem = nullptr;
        return false;
    }

    input_step = step;
    input_cv_type = input_is_fp16 ? CV_16FC3 : CV_8UC3;
    input_wrap = cv::Mat(INPUT_H, INPUT_W, input_cv_type, input_mem->virt_addr, step);
    return true;
}

std::vector<std::vector<float>> RKNNModel::infer(const cv::Mat& img, InferTiming* timing) {
    using Clock = std::chrono::high_resolution_clock;
    auto ms_since = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };

    int ret = 0;

    if (zero_copy) {
        // cvtColor пишет результат сразу в буфер, который читает NPU: лишней
        // копии кадра (rknn_inputs_set) на горячем пути нет.
        auto t0 = Clock::now();
        cv::Mat resized;
        cv::resize(img, resized, cv::Size(INPUT_W, INPUT_H));
        if (input_is_fp16) {
            // BGR->RGB в промежуточный uint8-буфер, затем один проход
            // convertTo (NEON) укладывает fp16 прямо в память NPU.
            cv::cvtColor(resized, rgb_scratch, cv::COLOR_BGR2RGB);
            rgb_scratch.convertTo(input_wrap, CV_16F);
        } else {
            cv::cvtColor(resized, input_wrap, cv::COLOR_BGR2RGB);
        }

        // Страховка: приёмник не должен переаллоцироваться (размер и тип
        // совпадают), но если это когда-нибудь случится, кадр уйдёт мимо буфера
        // NPU и модель будет молча инференсить предыдущий кадр. Тогда честно
        // копируем результат в буфер и восстанавливаем заголовок.
        if (input_wrap.data != (unsigned char*)input_mem->virt_addr) {
            cv::Mat npu_buf(INPUT_H, INPUT_W, input_cv_type, input_mem->virt_addr, input_step);
            input_wrap.copyTo(npu_buf);
            input_wrap = npu_buf;
        }
        if (timing) timing->preprocess_ms = ms_since(t0);

        // Память кэшируемая: без сброса кэша NPU может прочитать старый кадр.
        auto t1 = Clock::now();
        ret = rknn_mem_sync(ctx, input_mem, RKNN_MEMORY_SYNC_TO_DEVICE);
        if (timing) timing->input_copy_ms = ms_since(t1);
        if (ret < 0) {
            std::cerr << "rknn_mem_sync error ret=" << ret << std::endl;
            return {};
        }
    } else {
        auto t0 = Clock::now();
        cv::Mat resized, rgb;
        cv::resize(img, resized, cv::Size(INPUT_W, INPUT_H));
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
        if (timing) timing->preprocess_ms = ms_since(t0);

        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].size = INPUT_W * INPUT_H * INPUT_C;
        inputs[0].buf = rgb.data;

        auto t1 = Clock::now();
        ret = rknn_inputs_set(ctx, 1, inputs);
        if (timing) timing->input_copy_ms = ms_since(t1);
        if (ret < 0) {
            std::cerr << "rknn_inputs_set error ret=" << ret << std::endl;
            return {};
        }
    }

    auto t2 = Clock::now();
    ret = rknn_run(ctx, nullptr);
    if (timing) timing->npu_run_ms = ms_since(t2);
    if (ret < 0) {
        std::cerr << "rknn_run error ret=" << ret << std::endl;
        return {};
    }

    rknn_output outputs[3];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1;
    outputs[1].want_float = 1;
    outputs[2].want_float = 1;

    auto t3 = Clock::now();
    ret = rknn_outputs_get(ctx, 3, outputs, nullptr);
    if (timing) timing->dequant_ms = ms_since(t3);
    if (ret < 0) {
        std::cerr << "rknn_outputs_get error ret=" << ret << std::endl;
        return {};
    }

    // Ожидаемые размеры 3-х выходов для imgsz=512 (архитектура одинакова у
    // обеих моделей — они собраны из одних весов):
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
