#include "threat_logic.h"

float compute_ttc(float new_area, float prev_area, float dt) {
    if (prev_area <= 0.0f) return -1.0f;
    float d_area = new_area - prev_area;
    if (dt > 1e-3f && d_area > 1.0f) {
        return (new_area * dt) / d_area;
    }
    return -1.0f;
}

float compute_distance(float focal_length, float real_height, float box_height_px) {
    return (focal_length * real_height) / box_height_px;
}

float compute_danger_score(float w_class, float w_pos, float distance, float ttc,
                            float dist_eps, float ttc_eps, float ttc_k) {
    float ttc_term = 0.0f;
    if (ttc > 0.0f) {
        ttc_term = ttc_k / (ttc + ttc_eps);
    }
    return w_class * w_pos * (1.0f / (distance + dist_eps) + ttc_term);
}

float get_real_height(int class_id) {
    switch(class_id) {
        case 0: return 1.70f;
        case 1: return 1.50f;
        case 2: return 0.15f;
        case 3: return 3.00f;
        case 4: return 0.70f;
        case 5: return 0.80f;
        case 6: return 0.60f;
        case 7: return 0.60f;
        case 8: return 0.05f;
        case 9: return 0.05f;
        default: return 1.00f;
    }
}

float get_class_weight(int class_id) {
    switch(class_id) {
        case 1: return 3.0f;
        case 5: return 2.5f;
        case 0: return 2.0f;
        case 4: return 1.8f;
        case 2: return 1.5f;
        case 3: return 1.2f;
        case 6: return 1.2f;
        case 7: return 1.0f;
        case 9: return 1.0f;
        case 8: return 0.5f;
        default: return 1.0f;
    }
}

std::string get_class_name_en(int class_id) {
    const char* names[] = {"Person", "Car", "Curb", "Pole", "Sign", "Traffic Light", "Trash Can", "Bench", "Sidewalk", "Crosswalk"};
    if (class_id >= 0 && class_id <= 9) return names[class_id];
    return "Object";
}

std::string get_class_name_ru(int class_id) {
    const char* names[] = {"Человек", "Машина", "Бордюр", "Столб", "Знак", "Светофор", "Урна", "Скамья", "Тротуар", "Переход"};
    if (class_id >= 0 && class_id <= 9) return names[class_id];
    return "Объект";
}

std::string get_plural_meters(int dist) {
    if (dist % 10 == 1 && dist % 100 != 11) return "метр";
    if (dist % 10 >= 2 && dist % 10 <= 4 && (dist % 100 < 10 || dist % 100 >= 20)) return "метра";
    return "метров";
}
