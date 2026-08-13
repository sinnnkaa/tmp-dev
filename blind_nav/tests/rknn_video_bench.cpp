// Замеряет реальный FPS боевого пайплайна (RKNNModel::infer + decode()) на
// непрерывном видео, а не на одном кадре — rknn_image_test проверяет только
// корректность результата, этот бинарник проверяет скорость. Печатает
// разбивку по стадиям (препроцессинг / копирование в NPU / сам инференс на
// NPU / деквантование выхода / decode()), чтобы понять, что именно ест
// время в кадре, а не гадать.
//
// Запуск на плате:
//   ./rknn_video_bench <модель.rknn> <видео.mp4> [макс_кадров]

#include "../src/rknn_inference.h"
#include "../src/decode.h"

#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

constexpr int INPUT_SIZE = 512;
constexpr float CONF_THRESHOLD = 0.30f;

struct Accum {
    double sum = 0.0;
    void add(double v) { sum += v; }
    double avg(int n) const { return n > 0 ? sum / n : 0.0; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Использование: " << argv[0] << " <модель.rknn> <видео.mp4> [макс_кадров]\n";
        return 1;
    }

    int max_frames = argc >= 4 ? std::stoi(argv[3]) : 1000000;

    RKNNModel model;
    if (!model.load(argv[1])) {
        std::cerr << "!! Не удалось загрузить модель: " << argv[1] << "\n";
        return 1;
    }

    cv::VideoCapture cap(argv[2]);
    if (!cap.isOpened()) {
        std::cerr << "!! Не удалось открыть видео: " << argv[2] << "\n";
        return 1;
    }

    double src_fps = cap.get(cv::CAP_PROP_FPS);
    int src_w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int src_h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    std::cout << "Видео: " << src_w << "x" << src_h << " @ " << src_fps << " fps (источник)\n";
    std::cout << "Гоняем инференс кадр за кадром (без ограничения по частоте) на NPU...\n\n";

    using Clock = std::chrono::high_resolution_clock;

    Accum preprocess, input_copy, npu_run, dequant, decode_time, total_frame;
    int n = 0;
    int total_detections = 0;

    auto bench_start = Clock::now();

    cv::Mat frame;
    while (n < max_frames && cap.read(frame)) {
        if (frame.empty()) break;

        auto t_frame_start = Clock::now();

        InferTiming timing;
        auto raw_out = model.infer(frame, &timing);
        if (raw_out.size() != 3) {
            std::cerr << "  кадр " << n << ": инференс не удался, пропуск\n";
            continue;
        }

        auto t_decode_start = Clock::now();
        auto detections = decode(raw_out, INPUT_SIZE, INPUT_SIZE, frame.cols, frame.rows, CONF_THRESHOLD);
        double decode_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_decode_start).count();

        double frame_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_frame_start).count();

        preprocess.add(timing.preprocess_ms);
        input_copy.add(timing.input_copy_ms);
        npu_run.add(timing.npu_run_ms);
        dequant.add(timing.dequant_ms);
        decode_time.add(decode_ms);
        total_frame.add(frame_ms);
        total_detections += static_cast<int>(detections.size());

        n++;
        if (n % 30 == 0) {
            std::cout << "  ...обработано " << n << " кадров, текущий FPS="
                      << (1000.0 / frame_ms) << "\n";
        }
    }

    double wall_s = std::chrono::duration<double>(Clock::now() - bench_start).count();

    if (n == 0) {
        std::cerr << "!! Не удалось обработать ни одного кадра\n";
        return 1;
    }

    std::printf("\n=== Итог: %d кадров, %.2f с реального времени ===\n", n, wall_s);
    std::printf("Средний FPS (по сумме времени кадров): %.2f\n", 1000.0 / total_frame.avg(n));
    std::printf("Средний FPS (по общему wall time, с учётом чтения из VideoCapture): %.2f\n", n / wall_s);
    std::printf("Всего детекций (после NMS, порог %.2f): %d (%.1f/кадр)\n\n",
                CONF_THRESHOLD, total_detections, static_cast<double>(total_detections) / n);

    std::printf("Разбивка среднего времени на кадр (мс) — из чего складывается %.1f мс:\n", total_frame.avg(n));
    std::printf("  resize+cvtColor (препроцессинг, CPU):        %7.2f мс  (%.0f%%)\n",
                preprocess.avg(n), 100.0 * preprocess.avg(n) / total_frame.avg(n));
    std::printf("  rknn_inputs_set (копирование в буфер NPU):   %7.2f мс  (%.0f%%)\n",
                input_copy.avg(n), 100.0 * input_copy.avg(n) / total_frame.avg(n));
    std::printf("  rknn_run (сам инференс на NPU):              %7.2f мс  (%.0f%%)\n",
                npu_run.avg(n), 100.0 * npu_run.avg(n) / total_frame.avg(n));
    std::printf("  rknn_outputs_get (деквантование, CPU):       %7.2f мс  (%.0f%%)\n",
                dequant.avg(n), 100.0 * dequant.avg(n) / total_frame.avg(n));
    std::printf("  decode() (sigmoid+DFL+NMS, CPU):             %7.2f мс  (%.0f%%)\n",
                decode_time.avg(n), 100.0 * decode_time.avg(n) / total_frame.avg(n));

    return 0;
}
