#include <iostream>
#include <fstream>
#include <chrono>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <unistd.h>
#include <cmath>
#include <csignal>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <opencv2/opencv.hpp>
#include "rknn_inference.h"
#include "decode.h"
#include "threat_logic.h"

const float FOCAL_LENGTH = 450.0f;
const int FRAME_WIDTH = 640;
const int FRAME_HEIGHT = 480;

// Порог уверенности по классам: цена ошибки у классов разная, поэтому одна
// константа на всех — компромисс не в ничью пользу. Замер на INT8-модели
// (500 кадров val, только зона < 7 м, tools/eval/run_thresh_sweep_rknn.py)
// показал, что смягчение порога окупается ровно у одного класса:
// traffic_light при 0.25 даёт полноту 0.62 против 0.40 и точность 0.57
// против 0.55 — лучше сразу по обоим показателям, а не в обмен.
// У остальных снижение порога только меняет пропуски на ложные срабатывания
// (у pole при 0.10 полнота 0.73, но точность 0.28), поэтому им оставлен 0.30.
// Развёртка целиком — в METRICS.md.
const int CONF_CLASS_COUNT = 10;
const float CONF_THRESHOLD_BY_CLASS[CONF_CLASS_COUNT] = {
    0.30f,  // 0 person
    0.30f,  // 1 car
    0.30f,  // 2 curb
    0.30f,  // 3 pole
    0.30f,  // 4 traffic_sign
    0.25f,  // 5 traffic_light
    0.30f,  // 6 trash_can
    0.30f,  // 7 bench
    0.30f,  // 8 sidewalk
    0.30f,  // 9 crosswalk
};
// Декодируем по самому мягкому из порогов, отсев по классам идёт после NMS.
// Порядок важен: если резать до NMS, подавление считается по другому набору
// кандидатов, и замер порогов перестаёт соответствовать поведению устройства.
const float CONF_THRESHOLD_MIN = 0.25f;

float conf_threshold_for(int class_id) {
    if (class_id < 0 || class_id >= CONF_CLASS_COUNT) return 0.30f;
    return CONF_THRESHOLD_BY_CLASS[class_id];
}
const float MAX_DANGER_DIST = 7.0f;
const float ROI_TOP_MARGIN = 0.30f;

const int PERSISTENCE_THRESHOLD = 3;
const int TRACK_MAX_LIVES = 2;
const float TRACK_POS_TOLERANCE = 0.15f;

// Формула (4) из диплома: S_threat = W_class * W_zone * (1/(D+eps) + k/(TTC+eps))
const float DIST_EPS = 0.1f;
const float TTC_EPS = 0.3f;
const float TTC_K = 1.0f;

// Файл статуса макронавигации: пишет python-демон (voice_nav_daemon.py),
// читает C++ ядро, чтобы подавлять предупреждения ровно на время реального
// маршрута, а не на фиксированное окно.
const char* NAV_STATUS_PATH = "/dev/shm/nav_active";

// Общая блокировка звукового устройства между этим процессом и nav_daemon.service,
// чтобы предупреждения об опасности и голосовые инструкции маршрута не звучали
// одновременно поверх друг друга.
const char* AUDIO_LOCK_PATH = "/run/lock/blind_nav_audio.lock";
const char* AUDIO_LOG_PATH = "/root/diplom-cpp/system_audio.raw";
const long AUDIO_LOG_MAX_BYTES = 20L * 1024 * 1024;

// Квантованная модель: 25.3 FPS против 10.6 у прежней FP16-версии при падении
// полноты в опасной зоне на 2% по машинам и 5% по людям (замеры — METRICS.md).
// Прежняя yolo11_final.rknn оставлена рядом: чтобы вернуться, достаточно
// поменять путь здесь и пересобрать, формат выходов у моделей одинаковый.
const char* MODEL_PATH = "/root/diplom-cpp/blind_nav/model/yolo11_int8.rknn";

const char* VIDEO_PATH_A = "/root/diplom-cpp/output_video_a.avi";
const char* VIDEO_PATH_B = "/root/diplom-cpp/output_video_b.avi";
// Потолок записи — 500 МБ суммарно, поэтому на каждый из двух файлов кольца
// приходится половина. При 90 МБ/час это около пяти с половиной часов истории.
const long VIDEO_TOTAL_MAX_BYTES = 500L * 1024 * 1024;
const long VIDEO_MAX_BYTES = VIDEO_TOTAL_MAX_BYTES / 2;

std::atomic<bool> is_navigating(false);
std::atomic<bool> keep_running(true);

void handle_signal(int) {
    keep_running = false;
}

cv::Mat shared_latest_frame;
std::mutex frame_mutex;

// =========================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ РЕНДЕРИНГА НА 30 FPS
// =========================================================================
struct RenderBox {
    cv::Rect rect;
    std::string label;
    cv::Scalar color;
    int thickness;
};
std::vector<RenderBox> shared_boxes_to_render;
std::string shared_timer_text = "TIME: 00:00";
std::mutex render_mutex;
// =========================================================================

struct TrackedObject {
    int class_id;
    cv::Point2f center;
    int seen_count = 0;
    int lives = TRACK_MAX_LIVES;
    float last_distance = -1.0f;
    float area = 0.0f;               // площадь рамки на предыдущем подтверждении
    float ttc = -1.0f;                // формула (3): TTC ≈ S*Δt/ΔS, -1 = не оценено
    std::chrono::steady_clock::time_point last_update;
};

std::vector<TrackedObject> tracking_list;

struct LastSpokenState {
    int class_id = -1;
    std::string sector = "";
    float distance = -1.0f;
    std::chrono::steady_clock::time_point timestamp;
} last_spoken_state;

// Пишет кадры в output_video_{a,b}.avi по кругу: как только текущий файл
// превышает VIDEO_MAX_BYTES, запись переключается на второй файл и затирает
// его содержимое. Только что заполненный файл остаётся на диске — он и есть
// сохранённая история. Суммарный размер ограничен двумя файлами.
class VideoRotator {
public:
    void write(const cv::Mat& frame) {
        if (!writer.isOpened()) {
            open_new();
        }
        if (writer.isOpened()) {
            writer.write(frame);
        }

        struct stat st;
        if (stat(current_path(), &st) == 0 && st.st_size > VIDEO_MAX_BYTES) {
            use_a = !use_a;
            open_new();
        }
    }

    void close() {
        if (writer.isOpened()) writer.release();
    }

private:
    bool use_a = true;
    cv::VideoWriter writer;

    const char* current_path() const { return use_a ? VIDEO_PATH_A : VIDEO_PATH_B; }

    void open_new() {
        if (writer.isOpened()) writer.release();
        // Файл под запись открывается на усечение, поэтому удалять его руками
        // не нужно. Раньше здесь стоял std::remove(other_path()) — он стирал
        // не старый файл, а только что дописанный, и после каждого
        // переключения история обнулялась.
        writer.open(current_path(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30.0,
                    cv::Size(FRAME_WIDTH, FRAME_HEIGHT));
    }
};

void camera_thread_func(int camera_index) {
    cv::VideoCapture cap(camera_index, cv::CAP_V4L2);
    if (!cap.isOpened()) cap.open(camera_index);

    if (!cap.isOpened()) {
        std::cerr << "[Camera Thread] Ошибка открытия камеры!" << std::endl;
        return;
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH, FRAME_WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, FRAME_HEIGHT);
    cap.set(cv::CAP_PROP_FPS, 30);

    VideoRotator video_out;
    auto last_good_frame = std::chrono::steady_clock::now();
    const float CAMERA_TIMEOUT_SEC = 3.0f;

    while (keep_running) {
        cv::Mat temp_frame;
        cap >> temp_frame;
        if (!temp_frame.empty()) {
            last_good_frame = std::chrono::steady_clock::now();

            // 1. Отдаем чистый кадр нейросети
            {
                std::lock_guard<std::mutex> lock(frame_mutex);
                shared_latest_frame = temp_frame.clone();
            }

            // 2. Накатываем рамки и таймер поверх текущего кадра
            {
                std::lock_guard<std::mutex> lock(render_mutex);

                // Отрисовка всех рамок
                for (const auto& box_info : shared_boxes_to_render) {
                    cv::rectangle(temp_frame, box_info.rect, box_info.color, box_info.thickness);

                    int baseLine;
                    cv::Size labelSize = cv::getTextSize(box_info.label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
                    cv::rectangle(temp_frame, cv::Point(box_info.rect.x, box_info.rect.y - labelSize.height - 5),
                                  cv::Point(box_info.rect.x + labelSize.width, box_info.rect.y),
                                  cv::Scalar(0, 0, 0), cv::FILLED);

                    cv::putText(temp_frame, box_info.label, cv::Point(box_info.rect.x, box_info.rect.y - 5),
                                cv::FONT_HERSHEY_SIMPLEX, 0.5, box_info.color, 1);
                }

                // Отрисовка секундомера
                cv::rectangle(temp_frame, cv::Point(5, 5), cv::Point(190, 45), cv::Scalar(0, 0, 0), cv::FILLED);
                cv::putText(temp_frame, shared_timer_text, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 255), 2);
            }

            // 3. Пишем готовый плавный кадр в файл (с ограничением роста, см. VideoRotator)
            video_out.write(temp_frame);
        } else {
            std::chrono::duration<float> since_good = std::chrono::steady_clock::now() - last_good_frame;
            if (since_good.count() > CAMERA_TIMEOUT_SEC) {
                std::cerr << "\n[Camera Thread] Камера не отвечает " << since_good.count()
                          << " с — переоткрываю устройство..." << std::endl;
                cap.release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                cap.open(camera_index, cv::CAP_V4L2);
                if (!cap.isOpened()) cap.open(camera_index);
                if (cap.isOpened()) {
                    cap.set(cv::CAP_PROP_FRAME_WIDTH, FRAME_WIDTH);
                    cap.set(cv::CAP_PROP_FRAME_HEIGHT, FRAME_HEIGHT);
                    cap.set(cv::CAP_PROP_FPS, 30);
                    std::cerr << "[Camera Thread] Камера переоткрыта." << std::endl;
                } else {
                    std::cerr << "[Camera Thread] Переоткрыть камеру не удалось, повторю позже." << std::endl;
                }
                last_good_frame = std::chrono::steady_clock::now();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    cap.release();
    video_out.close();
}

// Сигнал "кнопка нажата" процессу voice_nav_daemon.py — сознательно TCP по
// loopback, а не AF_UNIX: единственное сообщение на нажатие кнопки (частота
// — секунды, не кадры), выигрыш AF_UNIX в задержке (десятки микросекунд)
// человеку не заметен. Порт 9999 занят исключительно этой парой процессов,
// конфликтов быть не должно.
void send_start_signal() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) >= 0) {
        const char* msg = "START_NAV";
        send(sock, msg, strlen(msg), 0);
    }
    close(sock);
}

void setup_button(int gpio_num) {
    std::ofstream export_file("/sys/class/gpio/export");
    if (export_file) { export_file << gpio_num; export_file.close(); }
    usleep(100000);
    std::ofstream dir_file("/sys/class/gpio/gpio" + std::to_string(gpio_num) + "/direction");
    if (dir_file) { dir_file << "in"; dir_file.close(); }
}

// Синхронизирует is_navigating с реальным статусом макронавигации: python-демон
// пишет "1"/"0" в NAV_STATUS_PATH в момент старта/остановки маршрута, поэтому
// подавление предупреждений об опасности длится ровно столько, сколько идёт
// реальная навигация, а не фиксированные 15 секунд.
void nav_status_watcher() {
    while (keep_running) {
        std::ifstream f(NAV_STATUS_PATH);
        if (f.is_open()) {
            char c = '0';
            f >> c;
            is_navigating = (c == '1');
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

void listen_button() {
    int gpio_num = 118;
    setup_button(gpio_num);
    std::string val_path = "/sys/class/gpio/gpio" + std::to_string(gpio_num) + "/value";

    auto last_press = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (keep_running) {
        std::ifstream val_file(val_path);
        if (val_file.is_open()) {
            int state;
            val_file >> state;
            auto now = std::chrono::steady_clock::now();
            if (state == 1 && std::chrono::duration<float>(now - last_press).count() > 1.0f) {
                last_press = now;
                std::cout << "\n[КНОПКА] Cигнал навигатору..." << std::endl;

                // Мгновенно глушим предупреждения об опасности; реальную
                // длительность подтвердит/снимет python через файл статуса.
                is_navigating = true;
                tracking_list.clear();
                send_start_signal();
            }
            val_file.close();
        }
        usleep(50000);
    }
}

float get_temperature() {
    std::ifstream temp_file("/sys/class/thermal/thermal_zone0/temp");
    if (!temp_file.is_open()) return 0.0f;
    float temp;
    temp_file >> temp;
    return temp / 1000.0f;
}

int main(int argc, char** argv) {
    std::cout << "--- Запуск навигации ---" << std::endl;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    RKNNModel model;
    if (!model.load(MODEL_PATH)) {
        std::cerr << "Ошибка загрузки модели!" << std::endl;
        return -1;
    }

    std::thread btn_thread(listen_button);
    std::thread nav_status_thread(nav_status_watcher);

    int working_camera = -1;
    for (int i = 0; i < 10; i++) {
        cv::VideoCapture temp_cap(i, cv::CAP_V4L2);
        if (!temp_cap.isOpened()) temp_cap.open(i);
        if (temp_cap.isOpened()) {
            working_camera = i;
            temp_cap.release();
            break;
        }
    }

    if (working_camera == -1) {
        std::cerr << "Ошибка: Камера не найдена!" << std::endl;
        keep_running = false;
        btn_thread.join();
        nav_status_thread.join();
        return -1;
    }

    std::thread cam_thread(camera_thread_func, working_camera);

    auto program_start_time = std::chrono::steady_clock::now();
    int frame_count = 0;
    last_spoken_state.timestamp = std::chrono::steady_clock::now();

    while (keep_running) {
        cv::Mat frame;
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            if (!shared_latest_frame.empty()) {
                frame = shared_latest_frame.clone();
            }
        }

        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto start_time = std::chrono::high_resolution_clock::now();

        // ВАЖНО: cv::VideoCapture отдаёт кадр в BGR. Конвертация в RGB выполняется
        // ОДИН раз — внутри model.infer() рядом с resize(). Раньше здесь был ещё
        // один cvtColor(BGR2RGB) до вызова infer(), а внутри infer() — такой же
        // повторно: применённые дважды подряд, они гасят друг друга (BGR2RGB —
        // это просто перестановка каналов R/B, и повторная перестановка отменяет
        // первую), и на NPU в итоге уходил BGR вместо ожидаемого RGB — тихая
        // порча входа модели на каждом кадре без единой ошибки в логах.
        auto raw_out = model.infer(frame);
        if (raw_out.size() != 3) {
            // Инференс не удался в этом кадре (сбой NPU/несовпадение размеров выходов) —
            // пропускаем кадр вместо чтения неинициализированной/чужой памяти.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        auto results = decode(raw_out, 512, 512, frame.cols, frame.rows, CONF_THRESHOLD_MIN);
        results.erase(std::remove_if(results.begin(), results.end(),
                                     [](const Detection& d) {
                                         return d.score < conf_threshold_for(d.class_id);
                                     }),
                      results.end());

        std::vector<RenderBox> current_boxes;

        // Собираем все найденные объекты (синие рамки)
        for (const auto& det : results) {
            RenderBox b;
            b.rect = cv::Rect(det.x, det.y, det.w, det.h);
            b.label = get_class_name_en(det.class_id);
            b.color = cv::Scalar(255, 0, 0);
            b.thickness = 1;
            current_boxes.push_back(b);
        }

        std::vector<bool> result_matched(results.size(), false);
        auto now_ts = std::chrono::steady_clock::now();

        for (auto& track : tracking_list) {
            bool found_match = false;
            for (size_t i = 0; i < results.size(); ++i) {
                if (result_matched[i]) continue;
                if (results[i].class_id != track.class_id) continue;

                cv::Point2f new_center((results[i].x + results[i].w / 2.0f) / FRAME_WIDTH,
                                       (results[i].y + results[i].h / 2.0f) / FRAME_HEIGHT);

                float dx = std::abs(new_center.x - track.center.x);
                float dy = std::abs(new_center.y - track.center.y);

                if (dx < TRACK_POS_TOLERANCE && dy < TRACK_POS_TOLERANCE) {
                    track.center = new_center;
                    track.seen_count++;
                    track.lives = TRACK_MAX_LIVES;

                    // Формула (3) диплома: TTC ≈ S*Δt/ΔS (эффект визуального расширения)
                    float new_area = results[i].w * results[i].h;
                    float dt = std::chrono::duration<float>(now_ts - track.last_update).count();
                    track.ttc = compute_ttc(new_area, track.area, dt);
                    track.area = new_area;
                    track.last_update = now_ts;

                    result_matched[i] = true;
                    found_match = true;
                    break;
                }
            }
            if (!found_match) track.lives--;
        }

        tracking_list.erase(std::remove_if(tracking_list.begin(), tracking_list.end(),
            [](const TrackedObject& t) { return t.lives <= 0; }), tracking_list.end());

        for (size_t i = 0; i < results.size(); ++i) {
            if (result_matched[i]) continue;
            TrackedObject new_track;
            new_track.class_id = results[i].class_id;
            new_track.center = cv::Point2f((results[i].x + results[i].w / 2.0f) / FRAME_WIDTH,
                                          (results[i].y + results[i].h / 2.0f) / FRAME_HEIGHT);
            new_track.seen_count = 1;
            new_track.area = results[i].w * results[i].h;
            new_track.last_update = now_ts;
            tracking_list.push_back(new_track);
        }

        float max_danger = -1.0f;
        int best_class_id = -1;
        float best_distance = 0.0f;
        std::string best_sector = "прямо";

        for (const auto& track : tracking_list) {
            if (track.seen_count < PERSISTENCE_THRESHOLD) continue;

            float distance = 0;
            cv::Rect best_box;

            for (const auto& det : results) {
                if (det.class_id == track.class_id) {
                     float H = get_real_height(det.class_id);
                     float d = compute_distance(FOCAL_LENGTH, H, det.h);
                     if ((det.y + det.h) < (FRAME_HEIGHT * ROI_TOP_MARGIN)) continue;

                     distance = d;
                     best_box = cv::Rect(det.x, det.y, det.w, det.h);
                     break;
                }
            }

            if (distance == 0 || distance > MAX_DANGER_DIST) continue;

            // Если объект опасен - добавляем зеленую рамку поверх синей
            int dist_m = static_cast<int>(distance + 0.5f);
            RenderBox b_green;
            b_green.rect = best_box;
            b_green.label = get_class_name_en(track.class_id) + " " + std::to_string(dist_m) + "m";
            b_green.color = cv::Scalar(0, 255, 0);
            b_green.thickness = 2;
            current_boxes.push_back(b_green);

            std::string current_sector = "прямо";
            float W_pos = 1.0f;

            if (track.center.x < 0.33f) current_sector = "слева";
            else if (track.center.x > 0.66f) current_sector = "справа";
            else { current_sector = "прямо"; W_pos = 1.5f; }

            float W_class = get_class_weight(track.class_id);

            // Формула (4) диплома: S_threat = W_class * W_zone * (1/(D+eps) + k/(TTC+eps))
            float danger_score = compute_danger_score(W_class, W_pos, distance, track.ttc,
                                                        DIST_EPS, TTC_EPS, TTC_K);

            if (danger_score > max_danger) {
                max_danger = danger_score;
                best_class_id = track.class_id;
                best_distance = distance;
                best_sector = current_sector;
            }
        }

        // =====================================================================
        // ОБНОВЛЕНИЕ ДАННЫХ ДЛЯ КАМЕРЫ
        // =====================================================================
        auto current_time_sync = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed_total = current_time_sync - program_start_time;
        float elapsed_sec = elapsed_total.count();

        int minutes = (int)elapsed_sec / 60;
        int seconds = (int)elapsed_sec % 60;
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "TIME: %02d:%02d", minutes, seconds);

        // Мгновенная блокировка для передачи данных в поток видео
        {
            std::lock_guard<std::mutex> lock(render_mutex);
            shared_boxes_to_render = current_boxes;
            shared_timer_text = std::string(time_str);
        }
        // =====================================================================

        std::chrono::duration<float> elapsed_since_speech = current_time_sync - last_spoken_state.timestamp;

        if (!is_navigating && max_danger > 0) {
            bool should_speak = false;

            if (best_class_id != last_spoken_state.class_id || best_sector != last_spoken_state.sector) {
                if (elapsed_since_speech.count() > 3.0f) should_speak = true;
            } else {
                if ((last_spoken_state.distance - best_distance) > 1.2f) {
                    if (elapsed_since_speech.count() > 1.5f) should_speak = true;
                } else {
                    if (elapsed_since_speech.count() > 12.0f) should_speak = true;
                }
            }

            if (should_speak) {
                int dist_m = static_cast<int>(best_distance + 0.5f);
                std::string text = get_class_name_ru(best_class_id) + " " + best_sector + ", " + std::to_string(dist_m) + " " + get_plural_meters(dist_m);

                // Проигрывание идёт через flock на общий с nav_daemon.service лок-файл,
                // чтобы предупреждение об опасности и голос маршрута не звучали одновременно;
                // guard перед echo ограничивает рост system_audio.raw.
                std::string audio_log(AUDIO_LOG_PATH);
                std::string command = std::string("flock ") + AUDIO_LOCK_PATH + " -c '"
                    "[ -f " + audio_log + " ] && [ $(stat -c%s " + audio_log + ") -gt " +
                    std::to_string(AUDIO_LOG_MAX_BYTES) + " ] && : > " + audio_log + "; "
                    "echo \"" + text + "\" | /root/diplom-cpp/piper/piper/piper "
                    "--model /root/diplom-cpp/piper/ru_RU-irina-medium.onnx --length_scale 0.85 --output-raw | "
                    "tee -a " + audio_log + " | "
                    "aplay -D default -r 22050 -f S16_LE -t raw -c 1 2>/dev/null' &";
                std::system(command.c_str());

                last_spoken_state.class_id = best_class_id;
                last_spoken_state.sector = best_sector;
                last_spoken_state.distance = best_distance;
                last_spoken_state.timestamp = current_time_sync;
            }
        }

        // Сохраняем чистый кадр (без графики) для веб-стриминга (если нужно)
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 70};
        cv::imwrite("/dev/shm/stream_tmp.jpg", frame, params);
        std::rename("/dev/shm/stream_tmp.jpg", "/dev/shm/stream.jpg");

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> total_ms = end_time - start_time;

        if (frame_count % 10 == 0) {
            std::string priority_info = (max_danger > 0) ? get_class_name_ru(best_class_id) : "Чисто";
            std::cout << "\r[Кадр " << frame_count << "] Треков: " << tracking_list.size()
                      << " | Темп: " << get_temperature() << " C"
                      << " | Цель: " << priority_info
                      << " | Инференс: " << total_ms.count() << " ms      " << std::flush;
        }
        frame_count++;
    }

    std::cout << "\n[Main] Получен сигнал остановки, завершаю потоки..." << std::endl;
    cam_thread.join();
    btn_thread.join();
    nav_status_thread.join();
    return 0;
}
