// Прогоняет боевой инференс-код (RKNNModel::infer + decode()) на статичных
// фото вместо камеры — единственный способ проверить реальный NPU-выход на
// голой плате без камеры/аудио/GPS. main.cpp читает кадры только с
// cv::VideoCapture, поэтому здесь отдельный маленький бинарник, использующий
// те же src/rknn_inference.cpp, src/decode.cpp, src/threat_logic.cpp без
// изменений в main.cpp.
//
// Запуск на плате:
//   ./rknn_image_test <путь_к_модели.rknn> <фото1.jpg> [фото2.jpg ...]
//
// Для каждого фото печатает список детекций (класс, уверенность, дистанция)
// и сохраняет рядом файл <имя>_detected.jpg с отрисованными боксами — можно
// сравнить с blind_nav/tests/rgb_bug_demo/*_correct.jpg (та же модель, но
// прогнанная через best.onnx на PC) и убедиться, что классы и расположение
// объектов совпадают.

#include "../src/rknn_inference.h"
#include "../src/decode.h"
#include "../src/threat_logic.h"

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>

namespace {

constexpr int INPUT_SIZE = 512;
constexpr float CONF_THRESHOLD = 0.30f;
constexpr float FOCAL_LENGTH = 450.0f;  // как в main.cpp — калибровка под камеру платы

void process_image(RKNNModel& model, const std::string& path) {
    cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);  // BGR, как отдаёт VideoCapture
    if (img.empty()) {
        std::cerr << "!! Не удалось загрузить " << path << "\n";
        return;
    }

    auto raw_out = model.infer(img);
    if (raw_out.size() != 3) {
        std::cerr << "!! Инференс не удался для " << path
                  << " (получено " << raw_out.size() << " выходных тензоров, ожидалось 3)\n";
        return;
    }

    auto detections = decode(raw_out, INPUT_SIZE, INPUT_SIZE, img.cols, img.rows, CONF_THRESHOLD);

    std::cout << "\n=== " << path << " (" << img.cols << "x" << img.rows << ") ===\n";
    std::cout << "  объектов после NMS: " << detections.size() << "\n";

    cv::Mat canvas = img.clone();
    for (const auto& det : detections) {
        float real_height = get_real_height(det.class_id);
        float distance_m = compute_distance(FOCAL_LENGTH, real_height, det.h);

        std::cout << "    " << get_class_name_ru(det.class_id)
                  << "  conf=" << det.score
                  << "  box=(" << det.x << ", " << det.y << ", " << det.w << ", " << det.h << ")"
                  << "  dist~" << distance_m << "m\n";

        cv::Rect box(static_cast<int>(det.x), static_cast<int>(det.y),
                     static_cast<int>(det.w), static_cast<int>(det.h));
        cv::rectangle(canvas, box, cv::Scalar(0, 255, 0), 2);
        std::string label = get_class_name_en(det.class_id) + " " +
                             std::to_string(det.score).substr(0, 4);
        cv::putText(canvas, label, cv::Point(box.x, std::max(15, box.y - 8)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
    }

    std::string out_path = path;
    auto dot = out_path.find_last_of('.');
    out_path = (dot == std::string::npos ? out_path : out_path.substr(0, dot)) + "_detected.jpg";
    cv::imwrite(out_path, canvas);
    std::cout << "  сохранено: " << out_path << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Использование: " << argv[0] << " <модель.rknn> <фото1.jpg> [фото2.jpg ...]\n";
        return 1;
    }

    RKNNModel model;
    if (!model.load(argv[1])) {
        std::cerr << "!! Не удалось загрузить модель: " << argv[1] << "\n";
        return 1;
    }
    std::cout << "Модель загружена: " << argv[1] << "\n";

    for (int i = 2; i < argc; ++i) {
        process_image(model, argv[i]);
    }

    return 0;
}
