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
#include <sys/statvfs.h>
#include <ctime>
#include <opencv2/opencv.hpp>
#include "rknn_inference.h"
#include "decode.h"
#include "threat_logic.h"
#include "pipeline.h"

// Константы пайплайна (фокусное, пороги классов, параметры трекинга и
// коэффициенты формулы (4)) живут в pipeline.cpp — их разделяет с этим
// файлом оффлайн-прогон видео, и разъезжаться им нельзя.

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

// Единый синтез+воспроизведение фразы. Раньше конвейер piper|aplay был
// выписан здесь строкой для std::system и второй раз в voice_nav_daemon.py;
// теперь обе стороны зовут один скрипт, и он же сохраняет реплику с меткой
// времени для автомонтажа ролика (tools/montage_session.sh).
const char* SAY_SCRIPT = "/root/diplom-cpp/tools/say.sh";

// Квантованная модель: 25.3 против 10.6 FPS у прежней FP16-версии при падении
// полноты в опасной зоне на 2% по машинам и 5% по людям (замеры — METRICS.md).
// Оба числа — со стенда оценки на подготовленных изображениях, а не темп
// устройства: на плате с камерой живой прогон даёт около 18 кадров/с
// (инференс 41 мс), остальное съедают захват кадра, отрисовка и запись видео.
// Прежняя yolo11_final.rknn оставлена рядом: чтобы вернуться, достаточно
// поменять путь здесь и пересобрать, формат выходов у моделей одинаковый.
const char* MODEL_PATH = "/root/diplom-cpp/blind_nav/model/yolo11_int8.rknn";

// Каталог записей прогулок. Внутри на каждый отрезок записи заводится:
//   capture_<тег>.avi      — кадры с рамками, MJPG (пишет этот процесс)
//   session_<тег>.meta     — начало, конец, число кадров и реальный FPS
//   speech_<тег>/          — реплики piper с метками времени (пишет say.sh)
//   micro_sound_<тег>.mp3  — непрерывный микрофон (пишет record_session.sh)
// Из них tools/montage_session.sh собирает full_video_<тег>.mp4.
const char* VIDEO_DIR = "/root/diplom-cpp/videos";

// Тег текущего отрезка записи. Через этот файл say.sh (а значит и python-демон)
// узнаёт, в какой каталог складывать реплики, не договариваясь с этим
// процессом напрямую. Пусто — запись не идёт.
const char* SESSION_TAG_PATH = "/dev/shm/nav_session";

// Потолок одного файла. Раньше здесь было кольцо из двух файлов по 250 МБ,
// затиравшее историю; теперь по достижении потолка начинается следующий
// отрезок, а прежний остаётся целиком — он нужен для монтажа.
const long VIDEO_SEGMENT_MAX_BYTES = 500L * 1024 * 1024;

// Ниже этого запаса запись прекращается. Забитый под ноль корневой раздел на
// этой плате означает не "нет нового видео", а неработающее устройство:
// перестают писаться и tmpfs-флаги, и журналы, и лог озвучки. Видео —
// вспомогательная функция, она обязана уступить первой.
const long VIDEO_MIN_FREE_BYTES = 2L * 1024 * 1024 * 1024;

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

// Вспомогательные функции записи прогулки.
static std::string date_tag() {
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%d_%m_%Y", &tm_buf);
    return std::string(buf);
}

static double epoch_now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

static bool path_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static long long free_bytes(const char* dir) {
    struct statvfs vfs;
    if (statvfs(dir, &vfs) != 0) return -1;
    return static_cast<long long>(vfs.f_bavail) * static_cast<long long>(vfs.f_frsize);
}

// Пишет кадры с рамками в videos/capture_<тег>.avi. По достижении
// VIDEO_SEGMENT_MAX_BYTES закрывает отрезок и начинает следующий — прежний
// файл остаётся целиком, из него потом собирается ролик.
//
// Тег — дата в формате ДД_ММ_ГГГГ; если запись за этот день уже есть, к тегу
// добавляется номер (19_08_2026_2), иначе второй прогон дня затёр бы первый.
class SessionRecorder {
public:
    void write(const cv::Mat& frame) {
        if (disabled_) return;

        // Запись приостановлена из-за нехватки места. Проверяем не чаще раза в
        // полминуты и возобновляем, как только место появилось: автомонтаж
        // (tools/montage_session.sh) удаляет исходные MJPG после сборки ролика
        // и освобождает по полгигабайта за раз — без этой проверки первая же
        // заполнившаяся карта означала бы "видео больше не пишется никогда",
        // хотя место уже есть.
        if (paused_) {
            if (std::chrono::steady_clock::now() < resume_check_) return;
            resume_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            long long avail = free_bytes(VIDEO_DIR);
            if (avail >= 0 && avail < VIDEO_MIN_FREE_BYTES) return;
            std::cerr << "[Запись] Место на разделе освободилось — возобновляю запись." << std::endl;
            paused_ = false;
        }

        if (!writer_.isOpened() && !open_new()) {
            disabled_ = true;
            return;
        }

        writer_.write(frame);
        frames_++;

        // Проверки раз в ~секунду, а не на каждом кадре: stat/statvfs на
        // SD-карте стоят заметно дороже самой записи кадра.
        if (frames_ % 30 != 0) return;

        struct stat st;
        if (stat(capture_path_.c_str(), &st) == 0 && st.st_size > VIDEO_SEGMENT_MAX_BYTES) {
            std::cerr << "[Запись] Отрезок " << tag_ << " заполнен, начинаю следующий." << std::endl;
            finalize();
            if (!open_new()) disabled_ = true;
            return;
        }

        long long avail = free_bytes(VIDEO_DIR);
        if (avail >= 0 && avail < VIDEO_MIN_FREE_BYTES) {
            std::cerr << "[Запись] Осталось меньше " << (VIDEO_MIN_FREE_BYTES >> 20)
                      << " МБ на разделе — останавливаю запись видео, чтобы не забить карту."
                      << std::endl;
            finalize();
            paused_ = true;
            resume_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            return;
        }

        // Раз в ~10 секунд обновляем meta незакрытого отрезка. Если питание
        // снимут рубильником, финализация не выполнится никогда — и без этой
        // страховки монтаж не узнал бы ни реального FPS, ни момента старта.
        if (frames_ % 300 == 0) write_meta(false);
    }

    void close() {
        if (writer_.isOpened()) finalize();
        publish_tag("");
    }

private:
    // disabled_ — отказ, из которого сам процесс не выйдет (не открывается
    // файл); paused_ — временная остановка по месту на карте, снимается сама.
    bool disabled_ = false;
    bool paused_ = false;
    std::chrono::steady_clock::time_point resume_check_{};
    cv::VideoWriter writer_;
    std::string tag_;
    std::string capture_path_;
    long long frames_ = 0;
    double start_epoch_ = 0.0;

    bool open_new() {
        mkdir(VIDEO_DIR, 0755);

        std::string base = date_tag();
        tag_ = base;
        for (int n = 2; n < 1000; n++) {
            if (!path_exists(std::string(VIDEO_DIR) + "/capture_" + tag_ + ".avi")) break;
            tag_ = base + "_" + std::to_string(n);
        }

        capture_path_ = std::string(VIDEO_DIR) + "/capture_" + tag_ + ".avi";
        mkdir((std::string(VIDEO_DIR) + "/speech_" + tag_).c_str(), 0755);

        // FPS в заголовке AVI заведомо номинальный: реальный темп камеры
        // плавает и известен только по факту. Монтаж пересчитывает дорожку по
        // fps из meta, иначе звук уезжает от картинки тем сильнее, чем длиннее
        // прогулка.
        writer_.open(capture_path_, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30.0,
                     cv::Size(FRAME_WIDTH, FRAME_HEIGHT));
        if (!writer_.isOpened()) {
            std::cerr << "[Запись] Не удалось открыть " << capture_path_
                      << " — запись видео выключена." << std::endl;
            return false;
        }

        frames_ = 0;
        start_epoch_ = epoch_now();
        write_meta(false);
        publish_tag(tag_);
        std::cerr << "[Запись] Пишу " << capture_path_ << std::endl;
        return true;
    }

    void finalize() {
        if (writer_.isOpened()) writer_.release();
        // Отдельного маркера "отрезок закрыт" рядом нет намеренно: закрытость
        // видна по final=1 в самой meta, а монтаж и без него знает, какой
        // отрезок пишется прямо сейчас — по /dev/shm/nav_session. Файл,
        // который никто не читает, рано или поздно начинают считать рабочим.
        write_meta(true);
        publish_tag("");
    }

    void write_meta(bool final_) {
        double now = epoch_now();
        double span = now - start_epoch_;
        double fps = (span > 0.5 && frames_ > 1) ? (frames_ / span) : 30.0;

        std::string path = std::string(VIDEO_DIR) + "/session_" + tag_ + ".meta";
        std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp);
            if (!f) return;
            f << "tag=" << tag_ << "\n"
              << "capture=capture_" << tag_ << ".avi\n"
              << "start_epoch=" << std::fixed << start_epoch_ << "\n"
              << "end_epoch=" << now << "\n"
              << "frames=" << frames_ << "\n"
              << "fps=" << fps << "\n"
              << "final=" << (final_ ? 1 : 0) << "\n";
        }
        // Через переименование: обрыв питания посреди записи meta оставил бы
        // монтажу обрезанный файл вместо предыдущей целой версии.
        std::rename(tmp.c_str(), path.c_str());
    }

    void publish_tag(const std::string& tag) {
        std::ofstream f(SESSION_TAG_PATH, std::ios::trunc);
        if (f) f << tag;
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

    SessionRecorder video_out;
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

            // 3. Пишем готовый плавный кадр в файл (нарезка по отрезкам — см. SessionRecorder)
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

// Следит за флагом открытого микрофона (см. MIC_OPEN_PATH). Раньше поток
// назывался nav_status_watcher и следил ещё и за статусом маршрута — имя
// осталось бы описанием того, чего он больше не делает.
//
// Здесь же раньше читался /dev/shm/nav_active в переменную is_navigating —
// остаток от подавления предупреждений на время GPS-маршрута. Подавление
// убрано по техзаданию (предупреждение об опасности должно звучать всегда), и
// переменную с тех пор никто не читал: два потока её писали, решений по ней не
// принимал никто. Мёртвое состояние опаснее отсутствующего — рано или поздно
// кто-нибудь начинает считать его рабочим. Демон продолжает писать
// /dev/shm/nav_active, но теперь это чисто диагностический признак «идёт
// маршрут», а не вход какой-либо логики.
void mic_status_watcher() {
    while (keep_running) {
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
    std::thread mic_status_thread(mic_status_watcher);

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
        mic_status_thread.join();
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
        // маршрута: сначала снимаем всё, что сейчас звучит или синтезируется,
        // затем захватываем общий с nav_daemon.service flock-лок. Голос
        // маршрута (voice_nav_daemon.py, speak()) симметрично никого не
        // обрывает — только ждёт лок, поэтому приоритет всегда у
        // предупреждения.
        //
        // Раньше здесь стояла проверка !is_navigating — предупреждения
        // полностью подавлялись на время GPS-навигации. По техзаданию это
        // неверно: предупреждение об опасности должно звучать всегда.
        if (decision.speak) {
            // Снимаем всё, что сейчас звучит или синтезируется: скрипт
            // озвучки, держащий лок flock, сам скрипт, синтез (он остаётся
            // только на промахах кэша) и воспроизведение.
            //
            // Каждый шаблон привязан к НАЧАЛУ командной строки — и это не
            // косметика. pkill -f сверяет шаблон с командными строками всех
            // процессов, включая ту оболочку, которую std::system подняла ради
            // этой самой команды; а в её строке дальше стоит запуск
            // "/root/diplom-cpp/tools/say.sh", то есть шаблон без якоря
            // находит сам себя. Оболочка убивала себя первым же pkill, и до
            // запуска озвучки дело не доходило вообще — предупреждения молчали
            // полностью. Оболочка всегда начинается с "/bin/sh -c", поэтому
            // якорь на реальное начало каждой цели её исключает.
            //
            // Реальные командные строки целей (сверено на плате):
            //   flock /run/lock/blind_nav_audio.lock /root/.../say.sh <фраза> core
            //   /bin/bash /root/diplom-cpp/tools/say.sh <фраза> core
            //   /root/diplom-cpp/piper/piper/piper --model ... --output-raw
            //   aplay -D <устройство> -r 22050 ... /root/.../piper/cache/<ключ>.raw
            //
            // Шаблон piper сужен до "--output-raw": предварительная сборка
            // кэша (tools/build_voice_cache.sh) зовёт его с --json-input, и без
            // этого первое же предупреждение обрывало бы её на середине.
            // Сигнал готовности микрофона играется не из кэша и тоже уцелеет.
            std::string command =
                std::string("pkill -f '^flock .*/tools/say[.]sh' >/dev/null 2>&1; ")
                + "pkill -f '^/bin/bash /root/diplom-cpp/tools/say[.]sh' >/dev/null 2>&1; "
                + "pkill -f '^/root/diplom-cpp/piper/piper/piper .*--output-raw' >/dev/null 2>&1; "
                + "pkill -f '^aplay .*piper/cache' >/dev/null 2>&1; "
                + "flock " + AUDIO_LOCK_PATH + " " + SAY_SCRIPT
                + " \"" + decision.phrase + "\" core &";

            // Результат проверяется не для порядка: -1 означает, что форк не
            // удался (кончились процессы или память) — предупреждение в этот
            // момент просто не прозвучит, и знать об этом надо из журнала, а
            // не гадать потом, почему устройство молчало.
            if (std::system(command.c_str()) == -1) {
                std::cerr << "[Звук] Не удалось запустить озвучку фразы: "
                          << decision.phrase << std::endl;
            }
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
    mic_status_thread.join();
    return 0;
}
