#include "pipeline.h"

#include <algorithm>
#include <cmath>

#include "threat_logic.h"

// Измерено по шагу 1 CALIBRATION.md: калибровка по шахматной доске (9x6
// внутренних углов, клетка 26мм) через tools/calib/, 640x480 MJPG — тот же
// тракт, что в бою. fy = 761.5px, стабильно между двумя независимыми
// съёмками (770.7 и 761.5, отсев смазанных кадров по личной ошибке
// репроекции). RMS репроекции ~2.4px выше "хорошего" порога в 2px из-за
// заметной бочкообразной дисторсии дешёвого широкоугольника (k1≈-0.46,
// тоже стабильно между съёмками) — pinhole-модель без undistort не может
// вписаться в неё идеально, но сама оценка f сходится уверенно. cx/cy и
// коэффициенты дисторсии не используются: compute_distance по высоте бокса
// не требует принципиальной точки. H_cam и угол наклона (шаги 2-3
// CALIBRATION.md) ещё не измерены — см. TODO там же.
const float FOCAL_LENGTH = 761.5f;
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
namespace {
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
}  // namespace

// Декодируем по самому мягкому из порогов, отсев по классам идёт после NMS.
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

void filter_by_class_threshold(std::vector<Detection>& results) {
    results.erase(std::remove_if(results.begin(), results.end(),
                                 [](const Detection& d) {
                                     return d.score < conf_threshold_for(d.class_id);
                                 }),
                  results.end());
}

void NavPipeline::reset() {
    tracking_list_.clear();
    last_spoken_.class_id = -1;
    last_spoken_.sector.clear();
    last_spoken_.distance = -1.0f;
}

PipelineOutput NavPipeline::process(const std::vector<Detection>& results, Clock::time_point now,
                                    bool speech_allowed) {
    PipelineOutput out;

    if (!started_) {
        last_spoken_.timestamp = now;
        started_ = true;
    }

    // Все найденные объекты — синие рамки.
    for (const auto& det : results) {
        RenderBox b;
        b.rect = cv::Rect(det.x, det.y, det.w, det.h);
        b.label = get_class_name_en(det.class_id);
        b.color = cv::Scalar(255, 0, 0);
        b.thickness = 1;
        out.boxes.push_back(b);
    }

    // ---- Сопоставление существующих треков с детекциями кадра -------------
    std::vector<bool> result_matched(results.size(), false);

    for (auto& track : tracking_list_) {
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
                float dt = std::chrono::duration<float>(now - track.last_update).count();
                track.ttc = compute_ttc(new_area, track.area, dt);
                track.area = new_area;
                track.last_update = now;

                track.matched_det = static_cast<int>(i);
                result_matched[i] = true;
                found_match = true;
                break;
            }
        }
        if (!found_match) {
            track.matched_det = -1;
            track.lives--;
        }
    }

    tracking_list_.erase(std::remove_if(tracking_list_.begin(), tracking_list_.end(),
        [](const TrackedObject& t) { return t.lives <= 0; }), tracking_list_.end());

    for (size_t i = 0; i < results.size(); ++i) {
        if (result_matched[i]) continue;
        TrackedObject new_track;
        new_track.class_id = results[i].class_id;
        new_track.center = cv::Point2f((results[i].x + results[i].w / 2.0f) / FRAME_WIDTH,
                                      (results[i].y + results[i].h / 2.0f) / FRAME_HEIGHT);
        new_track.seen_count = 1;
        new_track.area = results[i].w * results[i].h;
        new_track.matched_det = static_cast<int>(i);
        new_track.last_update = now;
        tracking_list_.push_back(new_track);
    }

    // ---- Выбор самой опасной цели по формуле (4) --------------------------
    float max_danger = -1.0f;

    for (const auto& track : tracking_list_) {
        if (track.seen_count < PERSISTENCE_THRESHOLD) continue;

        // Покрытие и разметка под ногами предупреждением быть не могут
        // (см. is_obstacle_class) — в конкурсе угроз они не участвуют.
        if (!is_obstacle_class(track.class_id)) continue;

        // Трек, не подтверждённый детекцией в этом кадре, доживает по lives
        // ради устойчивости трекинга, но озвучивать по нему нечего: свежей
        // геометрии нет, а брать чужую рамку того же класса — ровно та ошибка,
        // из-за которой появился matched_det.
        if (track.matched_det < 0 || track.matched_det >= static_cast<int>(results.size())) continue;

        const Detection& det = results[track.matched_det];

        // Верхняя часть кадра — небо и верхушки столбов: объект, целиком
        // лежащий выше ROI_TOP_MARGIN, под ногами оказаться не может.
        if ((det.y + det.h) < (FRAME_HEIGHT * ROI_TOP_MARGIN)) continue;

        float distance = compute_distance(FOCAL_LENGTH, get_real_height(det.class_id), det.h);
        if (!(distance > 0.0f) || distance > MAX_DANGER_DIST) continue;

        // Опасный объект — зелёная рамка поверх синей.
        int dist_m = static_cast<int>(distance + 0.5f);
        RenderBox b_green;
        b_green.rect = cv::Rect(det.x, det.y, det.w, det.h);
        b_green.label = get_class_name_en(track.class_id) + " " + std::to_string(dist_m) + "m";
        b_green.color = cv::Scalar(0, 255, 0);
        b_green.thickness = 2;
        out.boxes.push_back(b_green);

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
            out.class_id = track.class_id;
            out.distance = distance;
            out.sector = current_sector;
        }
    }

    out.danger_score = max_danger;
    out.has_danger = (max_danger > 0);

    // ---- Решение "говорить ли сейчас" -------------------------------------
    std::chrono::duration<float> elapsed_since_speech = now - last_spoken_.timestamp;

    if (out.has_danger) {
        bool should_speak = false;

        if (out.class_id != last_spoken_.class_id || out.sector != last_spoken_.sector) {
            if (elapsed_since_speech.count() > 3.0f) should_speak = true;
        } else {
            if ((last_spoken_.distance - out.distance) > 1.2f) {
                if (elapsed_since_speech.count() > 1.5f) should_speak = true;
            } else {
                if (elapsed_since_speech.count() > 12.0f) should_speak = true;
            }
        }

        if (should_speak && speech_allowed) {
            int dist_m = static_cast<int>(out.distance + 0.5f);
            out.speak = true;
            out.phrase = get_class_name_ru(out.class_id) + " " + out.sector + ", " +
                         std::to_string(dist_m) + " " + get_plural_meters(dist_m);

            last_spoken_.class_id = out.class_id;
            last_spoken_.sector = out.sector;
            last_spoken_.distance = out.distance;
            last_spoken_.timestamp = now;
        }
    }

    return out;
}
