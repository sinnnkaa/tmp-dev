// Прогоняет записанное видео через боевой пайплайн BlindNav: тот же
// RKNNModel::infer, тот же decode(), тот же NavPipeline с теми же константами,
// что и на устройстве (main.cpp). Отличается только источник кадров (файл
// вместо камеры) и приёмник голоса (список реплик вместо aplay).
//
// Зачем: показать работу системы на материале, который нельзя переснять
// вживую, и проверять пороги, не выходя на улицу. Логика намеренно не
// продублирована — она общая, в src/pipeline.cpp, иначе демонстрационный
// ролик со временем начал бы показывать не то, что делает устройство.
//
// Кадры приводятся к геометрии устройства: центральный кроп до 4:3 и ресайз
// до 640x480. Без кропа широкий кадр 16:9 сжимался бы по горизонтали, высоты
// боксов "поехали" бы вместе с ним, а по высоте бокса считается дистанция.
//
// Часы — виртуальные, привязанные к таймлайну видео, а не к wall clock: иначе
// выдержки озвучки (3с/1.5с/12с в NavPipeline) зависели бы от того, насколько
// быстро плата успевает считать, и результат был бы невоспроизводим.
//
// Запуск:
//   ./video_replay <модель.rknn> <вход.mp4> <выход.avi> <события.tsv> [fps] [макс_кадров]
//
// Озвучивает результат tools/replay_video.sh — он берёт события.tsv и
// монтирует piper-реплики на дорожку в те же секунды.

#include "../src/rknn_inference.h"
#include "../src/decode.h"
#include "../src/threat_logic.h"
#include "../src/pipeline.h"

#include <opencv2/opencv.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

namespace {

// Реальный темп устройства: INT8-модель даёт ~25 FPS (METRICS.md). Прогон по
// умолчанию прореживает исходные 30 кадров/с до этого темпа, чтобы трекинг и
// TTC работали на той же частоте обновления, что в бою.
constexpr double DEFAULT_FPS = 25.0;

// Центральный кроп до пропорций устройства (4:3) с последующим ресайзом.
cv::Mat to_device_geometry(const cv::Mat& src) {
    const double target_ar = static_cast<double>(FRAME_WIDTH) / FRAME_HEIGHT;
    const double src_ar = static_cast<double>(src.cols) / src.rows;

    cv::Rect roi(0, 0, src.cols, src.rows);
    if (src_ar > target_ar) {
        int w = static_cast<int>(std::lround(src.rows * target_ar));
        roi = cv::Rect((src.cols - w) / 2, 0, w, src.rows);
    } else if (src_ar < target_ar) {
        int h = static_cast<int>(std::lround(src.cols / target_ar));
        roi = cv::Rect(0, (src.rows - h) / 2, src.cols, h);
    }

    cv::Mat out;
    cv::resize(src(roi), out, cv::Size(FRAME_WIDTH, FRAME_HEIGHT), 0, 0, cv::INTER_AREA);
    return out;
}

// Та же отрисовка, что в camera_thread_func (main.cpp): синие рамки на всех
// детекциях, зелёные — на опасных, чёрная плашка с таймером в углу.
void draw_overlay(cv::Mat& frame, const std::vector<RenderBox>& boxes, const std::string& timer_text) {
    for (const auto& box_info : boxes) {
        cv::rectangle(frame, box_info.rect, box_info.color, box_info.thickness);

        int baseLine;
        cv::Size labelSize = cv::getTextSize(box_info.label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
        cv::rectangle(frame, cv::Point(box_info.rect.x, box_info.rect.y - labelSize.height - 5),
                      cv::Point(box_info.rect.x + labelSize.width, box_info.rect.y),
                      cv::Scalar(0, 0, 0), cv::FILLED);
        cv::putText(frame, box_info.label, cv::Point(box_info.rect.x, box_info.rect.y - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, box_info.color, 1);
    }

    cv::rectangle(frame, cv::Point(5, 5), cv::Point(190, 45), cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(frame, timer_text, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                cv::Scalar(0, 255, 255), 2);
}

// Полоса внизу кадра с последней произнесённой фразой: в самом устройстве её
// нет (пользователь незрячий), но в демонстрационном ролике зритель должен
// видеть, что именно система сказала и в какой момент.
void draw_subtitle(cv::Mat& frame, const std::string& latin_phrase) {
    if (latin_phrase.empty()) return;
    int baseLine;
    cv::Size sz = cv::getTextSize(latin_phrase, cv::FONT_HERSHEY_SIMPLEX, 0.6, 2, &baseLine);
    int y = frame.rows - 15;
    cv::rectangle(frame, cv::Point(5, y - sz.height - 8), cv::Point(15 + sz.width, y + 8),
                  cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(frame, latin_phrase, cv::Point(10, y), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 255), 2);
}

// cv::putText не умеет кириллицу (только Latin-1 в HERSHEY-шрифтах), поэтому
// для экранной подписи фраза дублируется латиницей. В озвучку (события.tsv)
// уходит настоящий русский текст — его произносит piper.
std::string subtitle_for(int class_id, const std::string& sector, int dist_m) {
    static const std::map<std::string, std::string> sectors = {
        {"слева", "left"}, {"справа", "right"}, {"прямо", "ahead"}};
    auto it = sectors.find(sector);
    std::string s = (it != sectors.end()) ? it->second : sector;
    return get_class_name_en(class_id) + " " + s + ", " + std::to_string(dist_m) + " m";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Использование: " << argv[0]
                  << " <модель.rknn> <вход.mp4> <выход.avi> <события.tsv> [fps] [макс_кадров]\n";
        return 1;
    }

    const std::string model_path = argv[1];
    const std::string in_path = argv[2];
    const std::string out_path = argv[3];
    const std::string events_path = argv[4];
    const double target_fps = (argc >= 6) ? std::stod(argv[5]) : DEFAULT_FPS;
    const int max_frames = (argc >= 7) ? std::stoi(argv[6]) : 1000000;

    if (!(target_fps > 0.0)) {
        std::cerr << "!! fps должен быть положительным\n";
        return 1;
    }

    RKNNModel model;
    if (!model.load(model_path)) {
        std::cerr << "!! Не удалось загрузить модель: " << model_path << "\n";
        return 1;
    }

    cv::VideoCapture cap(in_path);
    if (!cap.isOpened()) {
        std::cerr << "!! Не удалось открыть видео: " << in_path << "\n";
        return 1;
    }

    const double src_fps = cap.get(cv::CAP_PROP_FPS);
    const double total_src = cap.get(cv::CAP_PROP_FRAME_COUNT);
    if (!(src_fps > 0.0)) {
        std::cerr << "!! Не удалось определить fps исходного видео\n";
        return 1;
    }

    std::cout << "Вход:  " << in_path << "  " << cap.get(cv::CAP_PROP_FRAME_WIDTH) << "x"
              << cap.get(cv::CAP_PROP_FRAME_HEIGHT) << " @ " << src_fps << " fps, "
              << static_cast<long>(total_src) << " кадров\n";
    std::cout << "Прогон: " << FRAME_WIDTH << "x" << FRAME_HEIGHT << " @ " << target_fps
              << " fps (геометрия и темп устройства)\n";

    cv::VideoWriter writer(out_path, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), target_fps,
                           cv::Size(FRAME_WIDTH, FRAME_HEIGHT));
    if (!writer.isOpened()) {
        std::cerr << "!! Не удалось открыть на запись: " << out_path << "\n";
        return 1;
    }

    std::ofstream events(events_path);
    if (!events) {
        std::cerr << "!! Не удалось открыть на запись: " << events_path << "\n";
        return 1;
    }
    events << "# секунда\tфраза\n";

    NavPipeline pipeline;
    // База виртуальных часов. Нужна именно точка отсчёта steady_clock, потому
    // что NavPipeline меряет выдержки между кадрами; сами моменты синтезируются
    // из номера кадра, поэтому от реальной скорости платы результат не зависит.
    const auto clock_base = std::chrono::steady_clock::now();

    std::map<int, int> class_counts;
    int out_frames = 0;
    int spoken = 0;
    int dropped_by_bounds = 0;
    int total_dets = 0;
    std::string last_subtitle;
    double last_subtitle_until = -1.0;

    cv::Mat src;
    long src_index = -1;
    double next_due = 0.0;  // время следующего нужного кадра в секундах

    auto wall_start = std::chrono::steady_clock::now();

    while (out_frames < max_frames) {
        if (!cap.read(src) || src.empty()) break;
        src_index++;

        // Прореживание: берём ближайший исходный кадр к очередному моменту
        // выходного таймлайна. Проще и честнее, чем брать каждый N-й, когда
        // src_fps/target_fps не целое.
        const double src_time = src_index / src_fps;
        if (src_time + 1e-9 < next_due) continue;
        next_due += 1.0 / target_fps;

        cv::Mat frame = to_device_geometry(src);

        auto raw_out = model.infer(frame);
        if (raw_out.size() != 3) {
            std::cerr << "\n!! Инференс не удался на кадре " << src_index << ", пропускаю\n";
            continue;
        }

        auto results = decode(raw_out, 512, 512, frame.cols, frame.rows, CONF_THRESHOLD_MIN,
                              &dropped_by_bounds);
        filter_by_class_threshold(results);
        total_dets += static_cast<int>(results.size());
        for (const auto& d : results) class_counts[d.class_id]++;

        const double t_sec = out_frames / target_fps;
        const auto now_ts = clock_base + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                             std::chrono::duration<double>(t_sec));

        PipelineOutput decision = pipeline.process(results, now_ts);

        if (decision.speak) {
            spoken++;
            events << std::fixed << std::setprecision(3) << t_sec << "\t" << decision.phrase << "\n";
            std::cout << "\n[" << std::fixed << std::setprecision(2) << t_sec << "с] "
                      << decision.phrase << std::endl;
            last_subtitle = subtitle_for(decision.class_id, decision.sector,
                                         static_cast<int>(decision.distance + 0.5f));
            // Подпись держится на экране, пока фраза примерно звучит.
            last_subtitle_until = t_sec + 2.0;
        }

        char time_str[32];
        snprintf(time_str, sizeof(time_str), "TIME: %02d:%02d", static_cast<int>(t_sec) / 60,
                 static_cast<int>(t_sec) % 60);

        draw_overlay(frame, decision.boxes, time_str);
        if (t_sec <= last_subtitle_until) draw_subtitle(frame, last_subtitle);

        writer.write(frame);
        out_frames++;

        if (out_frames % 50 == 0) {
            std::cout << "\rКадров: " << out_frames << "  треков: " << pipeline.track_count()
                      << "  реплик: " << spoken << "   " << std::flush;
        }
    }

    writer.release();
    events.close();

    std::chrono::duration<double> wall = std::chrono::steady_clock::now() - wall_start;

    std::cout << "\n\n=== Итог прогона ===\n";
    std::cout << "Кадров обработано: " << out_frames << " (" << std::fixed << std::setprecision(1)
              << (out_frames / target_fps) << " с видео)\n";
    std::cout << "Реальное время прогона: " << wall.count() << " с ("
              << (wall.count() > 0 ? out_frames / wall.count() : 0.0) << " кадр/с на этой плате)\n";
    std::cout << "Детекций после порогов: " << total_dets << "\n";
    std::cout << "Отброшено фильтром границ кадра в decode(): " << dropped_by_bounds << "\n";
    std::cout << "Реплик озвучено: " << spoken << "\n";
    std::cout << "По классам:\n";
    for (const auto& kv : class_counts) {
        std::cout << "  " << std::setw(14) << std::left << get_class_name_en(kv.first) << kv.second << "\n";
    }
    std::cout << "\nВидео:   " << out_path << "\nСобытия: " << events_path << "\n";
    return 0;
}
