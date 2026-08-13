// Юнит-тесты чистой логики (decode.cpp, threat_logic.cpp) без зависимостей
// от OpenCV/RKNN — собираются и запускаются напрямую (см. tests/README.md
// или CMakeLists.txt с опцией BUILD_TESTS).
#include "../src/decode.h"
#include "../src/threat_logic.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) < (double)(eps))

// ------------------------- threat_logic.cpp -------------------------

static void test_compute_ttc() {
    // Объект приближается: площадь растёт со 100 до 200 px^2 за 0.1с.
    CHECK_NEAR(compute_ttc(200.0f, 100.0f, 0.1f), 0.2f, 1e-4f);

    // Объект удаляется (площадь падает) — TTC неприменим.
    CHECK(compute_ttc(100.0f, 200.0f, 0.1f) == -1.0f);

    // Первое наблюдение (нет предыдущей площади).
    CHECK(compute_ttc(100.0f, 0.0f, 0.1f) == -1.0f);
    CHECK(compute_ttc(100.0f, -5.0f, 0.1f) == -1.0f);

    // Интервал между кадрами слишком мал для устойчивой оценки.
    CHECK(compute_ttc(200.0f, 100.0f, 0.0005f) == -1.0f);

    // Площадь практически не изменилась (d_area <= 1.0) — тоже не считаем.
    CHECK(compute_ttc(100.5f, 100.0f, 0.1f) == -1.0f);
}

static void test_compute_distance() {
    CHECK_NEAR(compute_distance(450.0f, 1.7f, 100.0f), 7.65f, 1e-3f);
    // Вдвое меньшая рамка на кадре -> вдвое большая дистанция.
    CHECK_NEAR(compute_distance(450.0f, 1.7f, 50.0f), 15.3f, 1e-3f);
}

static void test_compute_danger_score() {
    // Без TTC (объект не приближается или не оценён).
    float score_no_ttc = compute_danger_score(2.0f, 1.5f, 5.0f, -1.0f, 0.1f, 0.3f, 1.0f);
    CHECK_NEAR(score_no_ttc, 2.0f * 1.5f * (1.0f / 5.1f), 1e-4f);

    // С TTC — угроза должна вырасти.
    float score_with_ttc = compute_danger_score(2.0f, 1.5f, 5.0f, 0.5f, 0.1f, 0.3f, 1.0f);
    CHECK(score_with_ttc > score_no_ttc);

    // Монотонность по дистанции: ближе -> опаснее.
    float score_far = compute_danger_score(1.0f, 1.0f, 10.0f, -1.0f, 0.1f, 0.3f, 1.0f);
    float score_near = compute_danger_score(1.0f, 1.0f, 1.0f, -1.0f, 0.1f, 0.3f, 1.0f);
    CHECK(score_near > score_far);

    // Монотонность по TTC: меньше времени до столкновения -> опаснее.
    float score_ttc_far = compute_danger_score(1.0f, 1.0f, 5.0f, 5.0f, 0.1f, 0.3f, 1.0f);
    float score_ttc_near = compute_danger_score(1.0f, 1.0f, 5.0f, 0.1f, 0.1f, 0.3f, 1.0f);
    CHECK(score_ttc_near > score_ttc_far);
}

static void test_class_helpers() {
    CHECK_NEAR(get_real_height(0), 1.70f, 1e-6f);   // Человек
    CHECK_NEAR(get_real_height(3), 3.00f, 1e-6f);   // Столб
    CHECK_NEAR(get_real_height(99), 1.00f, 1e-6f);  // неизвестный класс -> дефолт

    CHECK_NEAR(get_class_weight(1), 3.0f, 1e-6f);   // Машина — самый опасный класс
    CHECK_NEAR(get_class_weight(8), 0.5f, 1e-6f);   // Тротуар — наименее опасный
    CHECK_NEAR(get_class_weight(-1), 1.0f, 1e-6f);  // неизвестный класс -> дефолт

    CHECK(get_class_name_en(0) == "Person");
    CHECK(get_class_name_en(5) == "Traffic Light");
    CHECK(get_class_name_en(42) == "Object");

    CHECK(get_class_name_ru(0) == std::string("Человек"));
    CHECK(get_class_name_ru(42) == std::string("Объект"));

    // Русское склонение "метр/метра/метров".
    CHECK(get_plural_meters(1) == "метр");
    CHECK(get_plural_meters(2) == "метра");
    CHECK(get_plural_meters(5) == "метров");
    CHECK(get_plural_meters(11) == "метров");   // исключение: x11 всегда "метров"
    CHECK(get_plural_meters(12) == "метров");
    CHECK(get_plural_meters(21) == "метр");
    CHECK(get_plural_meters(100) == "метров");
    CHECK(get_plural_meters(101) == "метр");
}

// ------------------------------ decode.cpp ------------------------------

static void test_iou_and_nms() {
    Detection a{0, 0.9f, 0.0f, 0.0f, 10.0f, 10.0f};   // [0,0]-[10,10]
    Detection b{0, 0.8f, 5.0f, 5.0f, 10.0f, 10.0f};   // [5,5]-[15,15], пересекается с a
    Detection c{0, 0.7f, 100.0f, 100.0f, 10.0f, 10.0f}; // далеко, не пересекается

    // Площадь пересечения a/b = 5*5=25, объединение = 100+100-25=175 -> IoU ~0.1428
    CHECK_NEAR(calculate_iou(a, b), 25.0f / 175.0f, 1e-4f);
    CHECK_NEAR(calculate_iou(a, c), 0.0f, 1e-6f);

    std::vector<Detection> dets = {a, b, c};
    apply_nms(dets, 0.1f); // порог ниже IoU(a,b) -> b должен быть подавлен как дубль a
    CHECK(dets.size() == 2);
    // Остаться должны наивысший по score (a) и непересекающийся (c).
    bool has_a = false, has_c = false;
    for (auto& d : dets) {
        if (d.score == a.score) has_a = true;
        if (d.score == c.score) has_c = true;
    }
    CHECK(has_a);
    CHECK(has_c);

    // Разные классы с сильным пересечением не должны гаситься NMS.
    Detection d0{0, 0.9f, 0.0f, 0.0f, 10.0f, 10.0f};
    Detection d1{1, 0.85f, 0.0f, 0.0f, 10.0f, 10.0f};
    std::vector<Detection> mixed = {d0, d1};
    apply_nms(mixed, 0.1f);
    CHECK(mixed.size() == 2);
}

// Синтезирует один "уверенный" выход NPU (74 канала: 4*16 DFL + 10 классов)
// на сетке grid_size x grid_size и проверяет, что decode() находит ровно
// одну рамку ожидаемого класса и координат — сквозная проверка сигмоиды,
// DFL-декодирования, масштабирования под исходный кадр и клиппинга по границам.
static std::vector<float> make_confident_output(int grid_size, int grid_x, int grid_y,
                                                  int class_id, int dfl0, int dfl1,
                                                  int dfl2, int dfl3) {
    int area = grid_size * grid_size;
    int channels = 4 * 16 + 10;
    std::vector<float> out((size_t)channels * area, 0.0f);
    int i0 = grid_y * grid_size + grid_x;

    out[(size_t)(64 + class_id) * area + i0] = 10.0f; // sigmoid(10) ~ 0.99995

    int bins[4] = {dfl0, dfl1, dfl2, dfl3};
    for (int k = 0; k < 4; k++) {
        out[(size_t)(k * 16 + bins[k]) * area + i0] = 20.0f; // softmax почти = 1 в этом бине
    }
    return out;
}

static void test_decode_end_to_end() {
    const int input_size = 512;
    // conf_thresh=0.9 отсекает фон: sigmoid(0)=0.5 на всех остальных ячейках/масштабах.
    const float conf_thresh = 0.9f;

    std::vector<std::vector<float>> outputs(3);
    outputs[0] = make_confident_output(64, 10, 10, /*class_id=*/0, 2, 2, 2, 2); // stride 8
    outputs[1] = std::vector<float>((size_t)74 * 32 * 32, 0.0f);               // stride 16, пусто
    outputs[2] = std::vector<float>((size_t)74 * 16 * 16, 0.0f);               // stride 32, пусто

    auto dets = decode(outputs, input_size, input_size, /*orig_w=*/640, /*orig_h=*/480, conf_thresh);

    CHECK(dets.size() == 1);
    if (dets.size() == 1) {
        // В сетке-512 центр ячейки (10,10) при stride=8: (10.5*8, 10.5*8) = (84, 84);
        // с dfl=[2,2,2,2] рамка [68,68]-[100,100] (32x32) до масштабирования.
        // Масштаб на 640x480: scale_x=1.25, scale_y=0.9375.
        CHECK(dets[0].class_id == 0);
        CHECK_NEAR(dets[0].x, 68.0f * 1.25f, 0.5f);
        CHECK_NEAR(dets[0].y, 68.0f * 0.9375f, 0.5f);
        CHECK_NEAR(dets[0].w, 32.0f * 1.25f, 0.5f);
        CHECK_NEAR(dets[0].h, 32.0f * 0.9375f, 0.5f);
    }
}

static void test_decode_rejects_low_confidence() {
    const int input_size = 512;
    std::vector<std::vector<float>> outputs(3);
    // Все выходы нулевые -> sigmoid(0)=0.5 везде; порог 0.9 должен отсеять всё.
    outputs[0] = std::vector<float>((size_t)74 * 64 * 64, 0.0f);
    outputs[1] = std::vector<float>((size_t)74 * 32 * 32, 0.0f);
    outputs[2] = std::vector<float>((size_t)74 * 16 * 16, 0.0f);

    auto dets = decode(outputs, input_size, input_size, 640, 480, 0.9f);
    CHECK(dets.empty());
}

int main() {
    test_compute_ttc();
    test_compute_distance();
    test_compute_danger_score();
    test_class_helpers();
    test_iou_and_nms();
    test_decode_end_to_end();
    test_decode_rejects_low_confidence();

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures > 0) {
        std::fprintf(stderr, "%d FAILURES\n", g_failures);
        return 1;
    }
    return 0;
}
