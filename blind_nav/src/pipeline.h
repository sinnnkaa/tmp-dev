#pragma once

// Решающая часть боевого пайплайна: трекинг, оценка угрозы по формуле (4)
// диплома и решение "надо ли сейчас говорить". Вынесена из main.cpp, чтобы
// прогон записанного видео (tests/video_replay.cpp) шёл через тот же самый
// код, а не через его копию: копия неизбежно разъезжается с устройством, и
// демонстрационное видео перестаёт показывать то, что реально произойдёт.
//
// Захват кадров, отрисовка, звук, GPIO и запись видео остаются снаружи — они
// у устройства и у оффлайн-прогона разные по определению.

#include <chrono>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "decode.h"

// --- Геометрия кадра, на которой откалибровано всё остальное ---------------
extern const float FOCAL_LENGTH;
extern const int FRAME_WIDTH;
extern const int FRAME_HEIGHT;

// --- Пороги уверенности ----------------------------------------------------
extern const float CONF_THRESHOLD_MIN;
float conf_threshold_for(int class_id);

// --- Зона внимания и трекинг ----------------------------------------------
extern const float MAX_DANGER_DIST;
extern const float ROI_TOP_MARGIN;
extern const int PERSISTENCE_THRESHOLD;
extern const int TRACK_MAX_LIVES;
extern const float TRACK_POS_TOLERANCE;

// --- Коэффициенты формулы (4) ---------------------------------------------
extern const float DIST_EPS;
extern const float TTC_EPS;
extern const float TTC_K;

struct RenderBox {
    cv::Rect rect;
    std::string label;
    cv::Scalar color;
    int thickness;
};

struct TrackedObject {
    int class_id;
    cv::Point2f center;
    int seen_count = 0;
    int lives = TRACK_MAX_LIVES;
    float last_distance = -1.0f;
    float area = 0.0f;               // площадь рамки на предыдущем подтверждении
    float ttc = -1.0f;                // формула (3): TTC ≈ S*Δt/ΔS, -1 = не оценено
    // Индекс детекции текущего кадра, сопоставленной этому треку (-1 — трек в
    // этом кадре не подтверждён). Раньше расчёт дистанции искал детекцию
    // заново, беря ПЕРВУЮ с совпадающим class_id — при двух объектах одного
    // класса в кадре трек левого человека получал бокс и дистанцию правого,
    // и озвучивалось расстояние не до того объекта, о котором речь.
    int matched_det = -1;
    std::chrono::steady_clock::time_point last_update;
};

struct PipelineOutput {
    std::vector<RenderBox> boxes;   // синие — все детекции, зелёные — опасные
    bool has_danger = false;
    int class_id = -1;
    std::string sector = "прямо";
    float distance = 0.0f;
    float danger_score = -1.0f;

    bool speak = false;             // именно в этом кадре устройство заговорит
    std::string phrase;             // готовая фраза для piper
};

// Хранит состояние трекинга и историю озвучки между кадрами.
class NavPipeline {
public:
    using Clock = std::chrono::steady_clock;

    // results — детекции текущего кадра, уже прошедшие decode() и отсев по
    // пороговым значениям классов (см. filter_by_class_threshold).
    //
    // speech_allowed=false означает "сейчас говорить нельзя" (у устройства —
    // открыт микрофон Vosk, см. MIC_OPEN_PATH в main.cpp). Это именно
    // отсрочка: выдержка озвучки в таком кадре НЕ сбрасывается, поэтому как
    // только запрет снимут, фраза прозвучит на ближайшем же кадре. Если бы
    // вызывающий просто игнорировал out.speak, пайплайн считал бы фразу
    // произнесённой и замолчал бы на следующие 12 секунд.
    PipelineOutput process(const std::vector<Detection>& results, Clock::time_point now,
                           bool speech_allowed = true);

    // Сброс по нажатию кнопки: прошлые треки к новому маршруту отношения не имеют.
    void reset();

    size_t track_count() const { return tracking_list_.size(); }

private:
    struct LastSpoken {
        int class_id = -1;
        std::string sector;
        float distance = -1.0f;
        Clock::time_point timestamp{};
    };

    std::vector<TrackedObject> tracking_list_;
    LastSpoken last_spoken_;
    bool started_ = false;
};

// Отсев детекций по индивидуальному порогу класса. Выполняется ПОСЛЕ NMS:
// если резать до подавления, оно считается по другому набору кандидатов и
// замер порогов (METRICS.md) перестаёт соответствовать поведению устройства.
void filter_by_class_threshold(std::vector<Detection>& results);
