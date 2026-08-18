// Замеряет реальный FPS боевого пайплайна (RKNNModel::infer + decode()) на
// непрерывном видео, а не на одном кадре — rknn_image_test проверяет только
// корректность результата, этот бинарник проверяет скорость и качество
// детекций, плюс сохраняет размеченное видео для визуальной проверки.
//
// Запуск на плате:
//   ./rknn_video_bench <модель.rknn> <видео_вход.mp4> <видео_выход.avi> [макс_кадров]

#include "../src/rknn_inference.h"
#include "../src/decode.h"
#include "../src/threat_logic.h"

#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

namespace {

constexpr int INPUT_SIZE = 512;
constexpr float CONF_THRESHOLD = 0.30f;
constexpr float FOCAL_LENGTH = 761.5f;  // как в main.cpp — измерено калибровкой по чек-борду (см. CALIBRATION.md)

struct Accum {
    double sum = 0.0;
    void add(double v) { sum += v; }
    double avg(int n) const { return n > 0 ? sum / n : 0.0; }
};

struct ClassStats {
    int count = 0;
    double conf_sum = 0.0;
    int high_conf = 0;  // score >= 0.5 — уверенная детекция, не пограничная
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Использование: " << argv[0]
                  << " <модель.rknn> <видео_вход.mp4> <видео_выход.avi> [макс_кадров]\n";
        return 1;
    }

    int max_frames = argc >= 5 ? std::stoi(argv[4]) : 1000000;

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

    // Пишем размеченное видео с той же частотой, что источник, — так его
    // можно смотреть в реальном темпе сцены, а не в темпе обработки платой.
    cv::VideoWriter writer(argv[3], cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                            src_fps > 1.0 ? src_fps : 25.0, cv::Size(src_w, src_h));
    if (!writer.isOpened()) {
        std::cerr << "!! Не удалось открыть выходной файл на запись: " << argv[3] << "\n";
        return 1;
    }

    std::cout << "Гоняем инференс кадр за кадром (без ограничения по частоте) на NPU...\n\n";

    using Clock = std::chrono::high_resolution_clock;

    Accum preprocess, input_copy, npu_run, dequant, decode_time, total_frame;
    int n = 0;
    int total_detections = 0;
    std::map<int, ClassStats> class_stats;

    auto bench_start = Clock::now();

    cv::Mat frame;
    while (n < max_frames && cap.read(frame)) {
        if (frame.empty()) break;

        auto t_frame_start = Clock::now();

        InferTiming timing;
        auto raw_out = model.infer(frame, &timing);
        if (raw_out.size() != 3) {
            std::cerr << "  кадр " << n << ": инференс не удался, пропуск\n";
            writer.write(frame);
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

        for (const auto& det : detections) {
            auto& st = class_stats[det.class_id];
            st.count++;
            st.conf_sum += det.score;
            if (det.score >= 0.5f) st.high_conf++;

            cv::Rect box(static_cast<int>(det.x), static_cast<int>(det.y),
                         static_cast<int>(det.w), static_cast<int>(det.h));
            cv::rectangle(frame, box, cv::Scalar(0, 255, 0), 2);
            float dist_m = compute_distance(FOCAL_LENGTH, get_real_height(det.class_id), det.h);
            std::string label = get_class_name_en(det.class_id) + " " +
                                 std::to_string(det.score).substr(0, 4) + " " +
                                 std::to_string(dist_m).substr(0, 4) + "m";
            cv::putText(frame, label, cv::Point(box.x, std::max(15, box.y - 8)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
        }
        char fps_text[64];
        std::snprintf(fps_text, sizeof(fps_text), "frame %d  %.1f FPS", n, 1000.0 / frame_ms);
        cv::putText(frame, fps_text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(0, 255, 255), 2);
        writer.write(frame);

        n++;
        if (n % 30 == 0) {
            std::cout << "  ...обработано " << n << " кадров, текущий FPS="
                      << (1000.0 / frame_ms) << "\n";
        }
    }

    writer.release();
    double wall_s = std::chrono::duration<double>(Clock::now() - bench_start).count();

    if (n == 0) {
        std::cerr << "!! Не удалось обработать ни одного кадра\n";
        return 1;
    }

    std::printf("\n=== Итог: %d кадров, %.2f с реального времени ===\n", n, wall_s);
    std::printf("Средний FPS (по сумме времени кадров): %.2f\n", 1000.0 / total_frame.avg(n));
    std::printf("Средний FPS (по общему wall time, с учётом чтения из VideoCapture): %.2f\n", n / wall_s);
    std::printf("Размеченное видео сохранено: %s\n\n", argv[3]);

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

    std::printf("\nДетекции (после NMS, порог %.2f): всего %d (%.1f/кадр)\n",
                CONF_THRESHOLD, total_detections, static_cast<double>(total_detections) / n);
    std::printf("%-14s %8s %10s %12s\n", "класс", "кол-во", "avg conf", "conf>=0.5");
    for (const auto& [class_id, st] : class_stats) {
        std::printf("%-14s %8d %10.3f %11d%%\n",
                    get_class_name_en(class_id).c_str(), st.count,
                    st.conf_sum / st.count,
                    static_cast<int>(100.0 * st.high_conf / st.count));
    }

    return 0;
}
