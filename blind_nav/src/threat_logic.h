#pragma once
#include <string>

// Формула (3) диплома: TTC ≈ S*Δt/ΔS (эффект визуального расширения объекта).
// Возвращает -1.0f, если TTC неприменим (нет предыдущей площади, объект не
// приближается или интервал между кадрами слишком мал для устойчивой оценки).
float compute_ttc(float new_area, float prev_area, float dt);

// Проективная оценка дистанции по известной реальной высоте объекта и высоте
// его рамки в пикселях: D = (focal_length * real_height) / box_height_px.
float compute_distance(float focal_length, float real_height, float box_height_px);

// Формула (4) диплома: S_threat = W_class * W_zone * (1/(D+eps) + k/(TTC+eps)).
// ttc <= 0 означает "не оценено" — слагаемое TTC в этом случае обнуляется.
float compute_danger_score(float w_class, float w_pos, float distance, float ttc,
                            float dist_eps, float ttc_eps, float ttc_k);

// Является ли класс препятствием, о котором имеет смысл предупреждать голосом.
//
// sidewalk и crosswalk — разметка и покрытие под ногами, а не объекты на пути:
// налететь на тротуар нельзя, по нему идут. При этом оценка дистанции для них
// заведомо бессмысленна — compute_distance делит на высоту рамки, а "реальная
// высота" покрытия задана как 0.05 м, поэтому широкая полоса асфальта внизу
// кадра всегда выглядит как объект в полуметре и выигрывает конкурс угроз.
// На прогоне реального ролика (tools/replay_video.sh) это дало в эфир фразу
// "Тротуар прямо, 3 метра" вместо предупреждения о чём-то настоящем.
//
// Детектироваться и рисоваться эти классы продолжают — исключены они только из
// голосовых предупреждений об опасности.
bool is_obstacle_class(int class_id);

float get_real_height(int class_id);
float get_class_weight(int class_id);
std::string get_class_name_en(int class_id);
std::string get_class_name_ru(int class_id);
std::string get_plural_meters(int dist);
