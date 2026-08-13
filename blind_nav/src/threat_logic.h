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

float get_real_height(int class_id);
float get_class_weight(int class_id);
std::string get_class_name_en(int class_id);
std::string get_class_name_ru(int class_id);
std::string get_plural_meters(int dist);
