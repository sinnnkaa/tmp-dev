// Печатает все фразы предупреждений, какие боевой пайплайн вообще способен
// произнести. Нужен для предварительного синтеза (tools/build_voice_cache.sh):
// загрузка модели piper стоит около 3.5 секунды на запуск, поэтому фразы
// синтезируются заранее и на устройстве только проигрываются.
//
// Список именно генерируется этим кодом, а не выписывается в скрипте: русские
// названия классов и склонение слова "метр" живут в threat_logic.cpp, и второй
// их экземпляр в shell разъехался бы с боевым при первой же правке — кэш тогда
// молча перестал бы совпадать, а устройство вернулось бы к пятисекундному
// ожиданию синтеза, ничего об этом не сообщив.
//
// Сборка: cmake -DBUILD_VOICE_PHRASES=ON .. && make voice_phrases

#include <iostream>
#include <string>

#include "threat_logic.h"
#include "pipeline.h"
#include "device_phrases.h"

int main() {
    // Ровно то, что собирает NavPipeline::process():
    //   <класс_ru> <сектор>, <N> <метр|метра|метров>
    const std::string sectors[] = {"слева", "прямо", "справа"};

    // Дистанция округляется до целого, а объекты дальше MAX_DANGER_DIST в
    // конкурс опасности не попадают вовсе — отсюда верхняя граница.
    const int max_meters = static_cast<int>(MAX_DANGER_DIST + 0.5f);

    for (int class_id = 0; class_id < 10; class_id++) {
        // Тротуар и переход — покрытие под ногами, о них не предупреждают.
        if (!is_obstacle_class(class_id)) continue;

        for (const std::string& sector : sectors) {
            for (int meters = 0; meters <= max_meters; meters++) {
                std::cout << get_class_name_ru(class_id) << " " << sector << ", "
                          << meters << " " << get_plural_meters(meters) << "\n";
            }
        }
    }

    // Сообщения о состоянии самого устройства. Синтезировать их заранее важнее,
    // чем предупреждения: они звучат ровно тогда, когда что-то отказало, и
    // пятисекундная пауза на загрузку модели piper пришлась бы именно на тот
    // момент, когда человек ещё не знает, что прибор перестал видеть.
    for (const std::string& phrase : device_phrases()) {
        std::cout << phrase << "\n";
    }

    return 0;
}
