#include <iostream>
#include <fstream>
#include <sstream>
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
#include "pipeline.h"

// Константы пайплайна (фокусное, пороги классов, параметры трекинга и
// коэффициенты формулы (4)) живут в pipeline.cpp — их разделяет с этим
// файлом оффлайн-прогон видео, и разъезжаться им нельзя.

// Файл статуса макронавигации: пишет python-демон (voice_nav_daemon.py),
// читает C++ ядро, чтобы подавлять предупреждения ровно на время реального
// маршрута, а не на фиксированное окно.
const char* NAV_STATUS_PATH = "/dev/shm/nav_active";

// Флаг "микрофон гарнитуры открыт": nav_daemon.service держит его на время
// захвата речи через Vosk. Предупреждение об опасности в этот момент звучит в
// тот же наушник, с микрофона которого идёт запись, поэтому TTS попадает в
// распознаватель и портит адрес — а пользователь в этот момент стоит и
// диктует, то есть непосредственной опасности от движения нет. Окно короткое
// и жёстко ограничено длительностью захвата (4-7 секунд), после чего
// предупреждение прозвучит на следующем же кадре: это отсрочка, а не
// подавление.
const char* MIC_OPEN_PATH = "/dev/shm/nav_mic_open";

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

// Запрос на сброс трекинга от потока кнопки. Раньше listen_button() дёргал
// tracking_list.clear() прямо у себя, пока главный поток итерировал этот же
// std::vector и делал в него push_back — неcинхронизированный доступ к
// контейнеру из двух потоков это UB, а практически — испорченная куча или
// падение ровно в момент нажатия кнопки при видимых объектах. Теперь поток
// кнопки только поднимает флаг, а очищает список сам владелец — главный цикл.
std::atomic<bool> tracking_reset_requested(false);

// См. MIC_OPEN_PATH: на время записи адреса предупреждения откладываются.
std::atomic<bool> mic_is_open(false);

void handle_signal(int) {
    keep_running = false;
}

cv::Mat shared_latest_frame;
std::mutex frame_mutex;

// =========================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ РЕНДЕРИНГА НА 30 FPS
// =========================================================================
// RenderBox объявлен в pipeline.h.
std::vector<RenderBox> shared_boxes_to_render;
std::string shared_timer_text = "TIME: 00:00";
std::mutex render_mutex;
// =========================================================================

// Трекинг и история озвучки инкапсулированы в NavPipeline (pipeline.h).
NavPipeline pipeline;

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

// Без явного FOURCC V4L2-бэкенд может договориться с камерой на YUYV вместо
// MJPG, а на части USB-камер (в т.ч. на нашей — см. даташит) YUYV на 640x480
// снят с потолком 8 кадров/с против 20 у MJPG, тогда как cap.set(CAP_PROP_FPS,
// 30) ниже ничего не гарантирует и молча не сработает. Поэтому фиксируем
// формат явно и логируем то, что камера реально согласовала, а не то, что
// запрошено — расхождение будет видно в логе, а не всплывёт только на
// реальном устройстве.
void apply_camera_props(cv::VideoCapture& cap) {
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, FRAME_WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, FRAME_HEIGHT);
    cap.set(cv::CAP_PROP_FPS, 30);

    int fourcc = static_cast<int>(cap.get(cv::CAP_PROP_FOURCC));
    char fourcc_str[5] = {
        static_cast<char>(fourcc & 0xFF),
        static_cast<char>((fourcc >> 8) & 0xFF),
        static_cast<char>((fourcc >> 16) & 0xFF),
        static_cast<char>((fourcc >> 24) & 0xFF),
        '\0'
    };
    std::cerr << "[Camera Thread] Согласовано с камерой: " << fourcc_str << " "
              << cap.get(cv::CAP_PROP_FRAME_WIDTH) << "x" << cap.get(cv::CAP_PROP_FRAME_HEIGHT)
              << " @ " << cap.get(cv::CAP_PROP_FPS) << " fps (запрошено "
              << FRAME_WIDTH << "x" << FRAME_HEIGHT << " @ 30 fps)" << std::endl;
}

void camera_thread_func(int camera_index) {
    cv::VideoCapture cap(camera_index, cv::CAP_V4L2);
    if (!cap.isOpened()) cap.open(camera_index);

    if (!cap.isOpened()) {
        std::cerr << "[Camera Thread] Ошибка открытия камеры!" << std::endl;
        return;
    }

    apply_camera_props(cap);

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
                    apply_camera_props(cap);
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

        std::ifstream m(MIC_OPEN_PATH);
        if (m.is_open()) {
            char c = '0';
            m >> c;
            mic_is_open = (c == '1');
        } else {
            mic_is_open = false;
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
                tracking_reset_requested = true;
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

        // Сброс трекинга по нажатию кнопки выполняется здесь, в потоке-владельце
        // пайплайна (см. tracking_reset_requested).
        if (tracking_reset_requested.exchange(false)) {
            pipeline.reset();
        }

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
        filter_by_class_threshold(results);

        // Трекинг, формулы (3)-(4) и решение "говорить ли сейчас" — в
        // NavPipeline (pipeline.cpp). Тот же объект использует оффлайн-прогон
        // записанного видео, поэтому демонстрационный ролик показывает ровно
        // то поведение, которое будет у устройства.
        auto now_ts = std::chrono::steady_clock::now();
        PipelineOutput decision = pipeline.process(results, now_ts, !mic_is_open);
        const std::vector<RenderBox>& current_boxes = decision.boxes;

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

        // Предупреждение об опасности имеет абсолютный приоритет над голосом
        // маршрута: сначала обрываем любую уже звучащую фразу (aplay
        // навигационной подсказки, если она есть), затем захватываем общий с
        // nav_daemon.service flock-лок — так подсказка маршрута не звучит
        // поверх предупреждения и не заставляет его ждать. Голос маршрута
        // (Python, speak()) симметрично никого не обрывает — только ждёт лок,
        // поэтому приоритет всегда у предупреждения об опасности.
        //
        // Раньше здесь стояла проверка !is_navigating — предупреждения
        // полностью подавлялись на время GPS-навигации. По техзаданию это
        // неверно: предупреждение об опасности должно звучать всегда.
        if (decision.speak) {
            // guard перед echo ограничивает рост system_audio.raw.
            std::string audio_log(AUDIO_LOG_PATH);
            // Шаблон pkill намеренно не привязан к "-D default": голос
            // маршрута (voice_nav_daemon.py, speak()) адресуется конкретному
            // bluez-синку ("-D pulse:bluez_sink.<MAC>.<профиль>"), потому что
            // указатель default у PulseAudio на этой плате сползает на
            // встроенный кодек при каждой смене BT-профиля. Прежний дословный
            // шаблон после той правки перестал бы находить процесс, и
            // предупреждение молча потеряло бы право обрывать подсказку.
            std::string command = std::string("pkill -f 'aplay -D [^ ]+ -r 22050' >/dev/null 2>&1; flock ") + AUDIO_LOCK_PATH + " -c '"
                "[ -f " + audio_log + " ] && [ $(stat -c%s " + audio_log + ") -gt " +
                std::to_string(AUDIO_LOG_MAX_BYTES) + " ] && : > " + audio_log + "; "
                "echo \"" + decision.phrase + "\" | /root/diplom-cpp/piper/piper/piper "
                "--model /root/diplom-cpp/piper/ru_RU-irina-medium.onnx --length_scale 0.85 --output-raw | "
                "tee -a " + audio_log + " | "
                "aplay -D default -r 22050 -f S16_LE -t raw -c 1 2>/dev/null' &";
            std::system(command.c_str());
        }

        // Сохраняем чистый кадр (без графики) для веб-стриминга (если нужно)
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 70};
        cv::imwrite("/dev/shm/stream_tmp.jpg", frame, params);
        std::rename("/dev/shm/stream_tmp.jpg", "/dev/shm/stream.jpg");

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> total_ms = end_time - start_time;

        // Строка состояния. В терминале она перерисовывается на месте через \r,
        // но под systemd терминала нет: journald считает возврат каретки
        // управляющим символом, не закрывает запись до перевода строки и
        // копит всё в один кусок — в журнале это выглядело как повторяющееся
        // "[48.0K blob data]" и давало около 20 МБ записи в сутки. Поэтому в
        // журнал пишем обычными строками и на два порядка реже.
        static const bool stdout_is_tty = isatty(fileno(stdout));
        const int status_every = stdout_is_tty ? 10 : 750;  // ~0.4 с и ~30 с при 25 FPS
        if (frame_count % status_every == 0) {
            std::string priority_info = decision.has_danger ? get_class_name_ru(decision.class_id) : "Чисто";
            std::ostringstream status;
            status << "[Кадр " << frame_count << "] Треков: " << pipeline.track_count()
                   << " | Темп: " << get_temperature() << " C"
                   << " | Цель: " << priority_info
                   << " | Инференс: " << total_ms.count() << " ms";
            if (stdout_is_tty) {
                std::cout << "\r" << status.str() << "      " << std::flush;
            } else {
                std::cout << status.str() << std::endl;
            }
        }
        frame_count++;
    }

    std::cout << "\n[Main] Получен сигнал остановки, завершаю потоки..." << std::endl;
    cam_thread.join();
    btn_thread.join();
    nav_status_thread.join();
    return 0;
}
