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
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <ctime>
#include <iomanip>
#include <opencv2/opencv.hpp>
#include "rknn_inference.h"
#include "decode.h"
#include "threat_logic.h"
#include "pipeline.h"
#include "device_phrases.h"

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

// Как часто ядро отмечает в session_<тег>.frames, какой кадр когда снят.
//
// Реальный темп камеры за прогулку плавает — на записи 19.08.2026 он прошёл от
// 14.4 до 16.2 кадра/с, — а монтаж переклеивал всю запись под один постоянный
// fps из meta. К концу той прогулки звук разъехался с картинкой на 23 секунды:
// предупреждение «бордюр прямо» звучало через двадцать секунд после того, как
// бордюр проехал по кадру. Одного среднего fps для этого мало в принципе,
// нужна отметка времени по ходу записи. Раз в 30 кадров — это строка раз в две
// секунды, около четырёхсот строк на прогулку.
const long FRAME_STAMP_EVERY = 30;

// --- Сторож «кадры идут, а картинки в них нет» -----------------------------
//
// Отказ 19.08.2026: вебкамера мгновенно, между двумя соседними кадрами, начала
// отдавать равномерно-чёрные MJPG — и делала это ещё 7 минут 44 секунды, с тем
// же темпом, валидными кадрами и без единой ошибки USB в ядре. Проверка
// temp_frame.empty() отвечает на вопрос «вернул ли read() буфер», а не «есть
// ли в буфере изображение», поэтому не заметила ничего: устройство молча
// перестало предупреждать о препятствиях, а человек продолжал идти и ещё
// дважды пытался строить маршрут, не зная, что зрения у него больше нет.
//
// Смотрим на разброс яркости, а не на её среднее: у живой картинки разброс не
// бывает нулевым даже ночью — шум сенсора и блочность JPEG никуда не деваются,
// — а у равномерной заливки он ровно ноль, каким бы ни был её цвет. Порог с
// запасом ниже любого реального кадра.
const double CAMERA_BLANK_STDDEV = 1.5;

// Сколько подряд пустых кадров терпеть, прежде чем объявить отказ. Секунда-две
// однотонного кадра — это нормально (небо в объективе, стена вплотную);
// четыре секунды подряд у идущего человека — уже нет.
const float CAMERA_BLANK_TIMEOUT_SEC = 4.0f;

// Как часто повторять сообщение, пока камера не видит. Человек в этот момент
// идёт и слушает улицу — первую фразу он мог просто не разобрать, а молчание
// прибора неотличимо от «вокруг чисто».
const float CAMERA_BLIND_REANNOUNCE_SEC = 30.0f;

// Как часто пытаться оживить ослепшую камеру и после скольких безуспешных
// переоткрытий переходить к ресету USB-порта.
const float CAMERA_RECOVERY_INTERVAL_SEC = 15.0f;
const int CAMERA_REOPEN_TRIES_BEFORE_USB_RESET = 2;

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

        if (frames_ % FRAME_STAMP_EVERY == 0) write_stamp();

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
    std::ofstream stamps_;

    // Занят ли тег. Проверять один только capture_<тег>.avi мало: смонтированную
    // прогулку монтаж стирает, а meta, speech_<тег>/ и готовый ролик оставляет.
    // Второй выход в тот же день после автомонтажа получал бы тот же тег и
    // дописывал реплики в чужой speech.tsv, склеивал микрофон с прошлой
    // прогулкой через >> и терялся для монтажа из-за уже стоящей метки
    // .montaged — то есть готового ролика по второй прогулке не вышло бы вовсе.
    static bool tag_taken(const std::string& tag) {
        const std::string dir = std::string(VIDEO_DIR) + "/";
        return path_exists(dir + "capture_" + tag + ".avi")
            || path_exists(dir + "session_" + tag + ".meta")
            || path_exists(dir + "speech_" + tag)
            || path_exists(dir + "ambient_" + tag + ".mp3")
            || path_exists(dir + "full_video_" + tag + ".mp4");
    }

    bool open_new() {
        mkdir(VIDEO_DIR, 0755);

        std::string base = date_tag();
        tag_ = base;
        for (int n = 2; n < 1000; n++) {
            if (!tag_taken(tag_)) break;
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

        // Летопись «кадр N снят в момент T» для монтажа (см. FRAME_STAMP_EVERY).
        stamps_.close();
        stamps_.clear();
        stamps_.open(std::string(VIDEO_DIR) + "/session_" + tag_ + ".frames", std::ios::trunc);
        write_stamp();

        write_meta(false);
        publish_tag(tag_);
        std::cerr << "[Запись] Пишу " << capture_path_ << std::endl;
        return true;
    }

    // Отметка «кадр N снят в момент T». Сбрасывается на карту сразу: прогулка
    // может кончиться срывом питания, и недописанный хвост летописи стоит
    // ровно столько же, сколько её отсутствие. Строка короткая, раз в две
    // секунды — против записи самих кадров это ничто.
    void write_stamp() {
        if (!stamps_.is_open()) return;
        stamps_ << frames_ << " " << std::fixed << std::setprecision(3) << epoch_now() << "\n";
        stamps_.flush();
    }

    void finalize() {
        if (writer_.isOpened()) writer_.release();
        // Последняя отметка: без неё хвост отрезка от предыдущей отметки до
        // конца записи остался бы без времени, и монтаж досчитывал бы его по
        // среднему — то самое, от чего летопись и заводилась.
        write_stamp();
        stamps_.close();
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
        //
        // Одного переименования, однако, мало. ext4 с отложенным размещением
        // фиксирует само переименование раньше, чем данные файла, и после
        // срыва питания meta читается как файл правильного размера, целиком
        // забитый нулями — прогулка 19_08_2026_3 потерялась для монтажа
        // именно так, при полностью записанных ста мегабайтах видео.
        // Поэтому данные сбрасываются на карту до переименования, а каталог —
        // после: без второго fsync запись каталога сама может не пережить
        // обрыв. Цена — принудительная фиксация журнала раз в 300 кадров
        // (десятки миллисекунд на SD), что дешевле потерянной прогулки.
        int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
        std::rename(tmp.c_str(), path.c_str());
        int dir_fd = ::open(VIDEO_DIR, O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
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

// Произнести фразу немедленно, вытеснив всё, что сейчас звучит.
//
// Зовут отсюда двое: предупреждение об опасности из главного цикла и сообщение
// об отказе камеры из потока захвата. Обоим нужен один и тот же тракт, и
// второй экземпляр этих pkill-шаблонов разъехался бы с первым при первой же
// правке звука — а промах шаблона здесь не отказ, а тишина, которую никто не
// заметит.
void speak_urgent(const std::string& phrase) {
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
    // Вытеснение — единственная часть, которой нужна оболочка: это четыре
    // команды подряд, и никаких данных извне в них нет, только литералы.
    static const char* KILL_PLAYING =
        "pkill -f '^flock .*/tools/say[.]sh' >/dev/null 2>&1; "
        "pkill -f '^/bin/bash /root/diplom-cpp/tools/say[.]sh' >/dev/null 2>&1; "
        "pkill -f '^/root/diplom-cpp/piper/piper/piper .*--output-raw' >/dev/null 2>&1; "
        "pkill -f '^aplay .*piper/cache' >/dev/null 2>&1";

    // Результат проверяется не для порядка: -1 означает, что форк не
    // удался (кончились процессы или память) — предупреждение в этот
    // момент просто не прозвучит, и знать об этом надо из журнала, а
    // не гадать потом, почему устройство молчало.
    if (std::system(KILL_PLAYING) == -1) {
        std::cerr << "[Звук] Не удалось снять играющую озвучку" << std::endl;
    }

    // А вот сама фраза уходит отдельным аргументом execvp, а не подставляется
    // в shell-строку. Раньше здесь было ...say.sh "<фраза>" core &, и держалось
    // это только на том, что словарь предупреждений закрыт и собирается кодом.
    // На python-стороне от ровно такой подстановки ушли осознанно (см. speak()
    // в voice_nav_daemon.py); асимметрия пережила бы того, кто помнит, почему
    // фразы здесь «заведомо безопасные». Заодно исчезает кавычка как класс
    // проблемы: любой апостроф в русской формулировке ломал бы команду.
    //
    // Двойной fork вместо "&" в конце shell-строки: без него внук остался бы
    // ребёнком ядра, а ядро за 15 минут прогулки не делает wait ни разу —
    // накопились бы сотни зомби. После _exit промежуточного процесса внука
    // усыновляет init, он же его и похоронит.
    pid_t mid = fork();
    if (mid < 0) {
        std::cerr << "[Звук] Не удалось запустить озвучку фразы: "
                  << phrase << std::endl;
        return;
    }
    if (mid == 0) {
        if (fork() == 0) {
            const char* argv[] = {"flock", AUDIO_LOCK_PATH, SAY_SCRIPT,
                                  phrase.c_str(), "core", nullptr};
            execvp("flock", const_cast<char* const*>(argv));
            // execvp вернулся — значит не запустился. _exit, а не exit:
            // это форк главного цикла, и atexit-обработчики с буферами
            // потоков здесь трогать нельзя.
            _exit(127);
        }
        _exit(0);
    }
    // Ждём только промежуточный процесс — он завершается сразу, озвучка идёт
    // своим чередом.
    int status = 0;
    waitpid(mid, &status, 0);
}

// Ищет узел /dev/videoN, который действительно отдаёт кадры.
//
// Раньше индекс камеры искался один раз на старте по одному признаку
// «устройство открылось», и восстановление после отказа переиспользовало этот
// же индекс. Неверно и то и другое. UVC-камера регистрирует несколько узлов (у
// нашей — video0 и video1), и открывается не только тот из них, что отдаёт
// видео; а после переподключения по USB номер узла может смениться — тогда
// переоткрытие вечно долбилось бы в мёртвый индекс, считая, что лечит.
// Поэтому кандидат проверяется реальным кадром, а не фактом open(), и ищется
// заново при каждом восстановлении.
int find_camera_index(int probe_limit = 10) {
    for (int i = 0; i < probe_limit; i++) {
        cv::VideoCapture cap(i, cv::CAP_V4L2);
        if (!cap.isOpened()) cap.open(i);
        if (!cap.isOpened()) continue;

        apply_camera_props(cap);

        // Первые кадры после открытия камера может не отдать: идёт
        // согласование формата и раскачка экспозиции.
        for (int attempt = 0; attempt < 20 && keep_running; attempt++) {
            cv::Mat probe;
            cap >> probe;
            if (!probe.empty()) {
                cap.release();
                return i;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        cap.release();

        // Остановку проверяем и здесь: перебор десяти мёртвых узлов занимает
        // до десяти секунд, а systemd ждёт завершения пятнадцать (см.
        // TimeoutStopSec) — вместе с ресетом USB в это уже не уложиться.
        if (!keep_running) break;
    }
    return -1;
}

// USB-порт, за которым сидит /dev/videoN — «5-1» и подобное.
// /sys/class/video4linux/videoN/device — символическая ссылка на USB-интерфейс
// (".../usb5/5-1/5-1:1.0"); имя самого устройства — то же без ":интерфейс".
std::string camera_usb_port(int index) {
    std::string link = "/sys/class/video4linux/video" + std::to_string(index) + "/device";
    char buf[512];
    ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';

    std::string target(buf);
    size_t slash = target.find_last_of('/');
    std::string leaf = (slash == std::string::npos) ? target : target.substr(slash + 1);

    // Ни двоеточия, ни дефиса — это не USB-интерфейс (например, камера на
    // MIPI-шине платы). Ресетить там нечего, и гадать не будем.
    size_t colon = leaf.find(':');
    if (colon == std::string::npos) return "";
    std::string port = leaf.substr(0, colon);
    if (port.find('-') == std::string::npos) return "";
    return port;
}

// Логический ресет USB-устройства камеры: отвязать и привязать драйвер.
//
// Обычного переоткрытия /dev/videoN мало, когда встал сам модуль камеры:
// close() не перезагружает его прошивку, и после open() продолжает идти та же
// чернота. unbind/bind заставляет ядро заново провести устройство через probe
// — для сенсора это равносильно передёргиванию разъёма, только без рук.
//
// Побочный эффект: у той же вебкамеры отвалится и аудио-интерфейс, то есть
// оборвётся непрерывная запись микрофона. Это предусмотрено —
// tools/record_session.sh следит за своим ffmpeg и поднимает его заново,
// дописывая в тот же mp3 (см. mic_alive там же).
bool usb_port_reset(const std::string& port) {
    if (port.empty()) return false;

    auto write_to = [&port](const char* path) {
        std::ofstream f(path);
        if (!f) return false;
        f << port;
        f.flush();
        return static_cast<bool>(f);
    };

    std::cerr << "[Camera Thread] Ресет USB-порта " << port << "..." << std::endl;
    if (!write_to("/sys/bus/usb/drivers/usb/unbind")) {
        std::cerr << "[Camera Thread] Не удалось отвязать " << port << "." << std::endl;
        return false;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (!write_to("/sys/bus/usb/drivers/usb/bind")) {
        std::cerr << "[Camera Thread] Не удалось привязать " << port
                  << " обратно — камера осталась отключённой!" << std::endl;
        return false;
    }
    // Перечисление устройства и регистрация узлов /dev/videoN занимают
    // заметное время: без паузы поиск камеры не найдёт ничего и решит, что
    // ресет не помог.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return true;
}

// Кадр без картинки — равномерная заливка любого цвета. См. CAMERA_BLANK_STDDEV.
//
// Прореживание именно INTER_NEAREST, а не усреднение: усреднение сгладило бы
// шум сенсора, по которому живой тёмный кадр как раз и отличается от мёртвого,
// и ночная съёмка стала бы поводом объявить камеру ослепшей.
//
// Считать надо по кадру ДО отрисовки рамок и секундомера: жёлтые цифры на
// чёрном дают разброс сами по себе, и сторож по такому кадру не сработал бы
// никогда. Ровно так выглядит запись 19.08 — чёрный экран с бегущим таймером.
bool frame_is_blank(const cv::Mat& frame) {
    if (frame.empty()) return true;

    cv::Mat small;
    cv::resize(frame, small, cv::Size(32, 24), 0, 0, cv::INTER_NEAREST);
    if (small.channels() > 1) cv::cvtColor(small, small, cv::COLOR_BGR2GRAY);

    cv::Scalar mean, stddev;
    cv::meanStdDev(small, mean, stddev);
    return stddev[0] < CAMERA_BLANK_STDDEV;
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

    // Состояние сторожа «кадры идут, а картинки в них нет».
    bool blank_run = false;                                  // серия пустых кадров идёт
    std::chrono::steady_clock::time_point blank_since{};     // с какого момента
    bool camera_blind = false;                               // отказ объявлен
    std::chrono::steady_clock::time_point blind_announced{}; // когда сказали вслух
    std::chrono::steady_clock::time_point next_recovery{};   // когда лечить
    int reopen_tries = 0;

    // Переоткрытие с повторным поиском узла: после переподключения по USB
    // камера может вернуться под другим номером (см. find_camera_index).
    auto reopen_camera = [&]() {
        cap.release();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        int found = find_camera_index();
        if (found >= 0) {
            camera_index = found;
            cap.open(camera_index, cv::CAP_V4L2);
            if (!cap.isOpened()) cap.open(camera_index);
        }

        if (cap.isOpened()) {
            apply_camera_props(cap);
            std::cerr << "[Camera Thread] Камера переоткрыта: /dev/video" << camera_index
                      << std::endl;
        } else {
            std::cerr << "[Camera Thread] Переоткрыть камеру не удалось, повторю позже."
                      << std::endl;
        }

        // Серия пустых кадров шла до переоткрытия — засчитывать её заново
        // открытой камере нельзя, иначе отказ объявится повторно мгновенно.
        blank_run = false;
        last_good_frame = std::chrono::steady_clock::now();
    };

    while (keep_running) {
        cv::Mat temp_frame;
        cap >> temp_frame;
        auto now = std::chrono::steady_clock::now();

        if (!temp_frame.empty()) {
            last_good_frame = now;

            // 0. Сторож содержимого кадра — обязательно ДО отрисовки: рамки и
            // секундомер сами по себе дают разброс яркости, и по кадру с ними
            // сторож не сработал бы никогда.
            if (frame_is_blank(temp_frame)) {
                if (!blank_run) {
                    blank_run = true;
                    blank_since = now;
                }
            } else {
                blank_run = false;
                if (camera_blind) {
                    std::cerr << "[Camera Thread] Картинка вернулась." << std::endl;
                    speak_urgent(PHRASE_CAMERA_BACK);
                    camera_blind = false;
                    reopen_tries = 0;
                }
            }

            if (!camera_blind && blank_run &&
                std::chrono::duration<float>(now - blank_since).count() > CAMERA_BLANK_TIMEOUT_SEC) {
                std::cerr << "[Camera Thread] Кадры идут, но картинки в них нет — камера ослепла."
                          << std::endl;
                camera_blind = true;
                blind_announced = now;
                next_recovery = now;   // лечить немедленно
                reopen_tries = 0;
                speak_urgent(PHRASE_CAMERA_BLIND);
            }

            if (camera_blind &&
                std::chrono::duration<float>(now - blind_announced).count() > CAMERA_BLIND_REANNOUNCE_SEC) {
                blind_announced = now;
                speak_urgent(PHRASE_CAMERA_BLIND);
            }

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
            std::chrono::duration<float> since_good = now - last_good_frame;
            if (since_good.count() > CAMERA_TIMEOUT_SEC) {
                std::cerr << "\n[Camera Thread] Камера не отвечает " << since_good.count()
                          << " с — переоткрываю устройство..." << std::endl;
                reopen_camera();
            }
        }

        // Лечение ослепшей камеры. Здесь, а не в ветке кадра: при этом отказе
        // кадры как раз идут, просто пустые, и ветка «камера не отвечает» на
        // него не выйдет никогда.
        if (camera_blind && now >= next_recovery) {
            // Порт узнаём до отвязки: после неё узла /dev/videoN уже нет.
            std::string port = camera_usb_port(camera_index);

            // Сначала просто переоткрываем — этого хватает, если встал
            // драйвер, а не сам модуль. Если два раза подряд не помогло,
            // дёргаем устройство через unbind/bind: прошивку сенсора
            // перезагружает только это.
            if (reopen_tries >= CAMERA_REOPEN_TRIES_BEFORE_USB_RESET && !port.empty()) {
                cap.release();
                usb_port_reset(port);
                reopen_tries = 0;
            }
            reopen_tries++;

            reopen_camera();

            // Отсчёт до следующей попытки — от её конца, а не от начала:
            // ресет USB вместе с перечислением устройства съедает шесть
            // секунд, и от начала пауза между попытками вышла бы вдвое
            // короче задуманной.
            next_recovery = std::chrono::steady_clock::now() + std::chrono::seconds(
                static_cast<long long>(CAMERA_RECOVERY_INTERVAL_SEC));
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

    // Поиск узла, который реально отдаёт кадры (см. find_camera_index).
    // Прежний перебор считал камерой первое устройство, которое просто
    // открылось, — а у UVC-камеры открывается и служебный узел без видео.
    int working_camera = find_camera_index();

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
        // Инференс замеряется отдельно от витка. Раньше в строке состояния
        // печаталось «Инференс: <total_ms>», где total_ms охватывал весь виток
        // целиком — инференс, decode, пайплайн, запуск озвучки и кодирование
        // кадра для веб-стрима. По этой строке в METRICS.md попала цифра
        // «инференс 41 мс», то есть NPU записали в отчёт медленнее, чем он есть.
        auto infer_start = std::chrono::high_resolution_clock::now();
        auto raw_out = model.infer(frame);
        std::chrono::duration<float, std::milli> infer_ms =
            std::chrono::high_resolution_clock::now() - infer_start;
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
            speak_urgent(decision.phrase);
        }

        // Здесь на каждом кадре кодировался JPEG 640x480 в /dev/shm/stream.jpg
        // «для веб-стриминга (если нужно)». Читать его было некому:
        // python/web_stream.py не поднимался ни systemd-юнитом, ни install.sh и
        // не упоминался в документации, а выключателя у записи не было. Платил
        // за неё главный цикл — тот самый, чья задержка означает предупреждение,
        // прозвучавшее позже, чем нужно.
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> loop_ms = end_time - start_time;

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
                   << " | Инференс: " << infer_ms.count() << " ms"
                   << " | Виток: " << loop_ms.count() << " ms";
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
