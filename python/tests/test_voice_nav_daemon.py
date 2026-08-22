"""
Юнит-тесты для voice_nav_daemon.py.

Модули vosk, gps и requests на машине разработки не установлены — перед
импортом демона они подменяются заглушками (см. _stub_hardware_modules),
поэтому тесты покрывают только чистую логику (парсинг адреса, ротация логов,
построение команд, сценарии navigation_worker) с замоканными STT/TTS/GPS/OSRM.
Требует только стандартной библиотеки + unittest.mock — pytest не нужен:

    python3 -m unittest discover -s python/tests -v
"""
import os
import sys
import tempfile
import types
import unittest
from unittest import mock


# Внешние зависимости демона, которых на машине разработки нет. Список именно
# полный: пропущенный в нём requests уронил импорт демона на ModuleNotFoundError
# ещё до первого теста, и весь набор молча перестал выполняться — про запуск
# тестов при этом было написано, что он работает без железа.
def _stub_hardware_modules():
    """Подставляет заглушки vosk/gps/requests в sys.modules, если их нет."""
    if "requests" not in sys.modules:
        try:
            import requests  # noqa: F401
        except ImportError:
            requests_stub = types.ModuleType("requests")
            requests_stub.get = mock.MagicMock()
            requests_stub.post = mock.MagicMock()
            sys.modules["requests"] = requests_stub

    if "vosk" not in sys.modules:
        try:
            import vosk  # noqa: F401
        except ImportError:
            vosk_stub = types.ModuleType("vosk")
            vosk_stub.Model = mock.MagicMock
            vosk_stub.KaldiRecognizer = mock.MagicMock
            sys.modules["vosk"] = vosk_stub

    if "gps" not in sys.modules:
        try:
            import gps  # noqa: F401
        except ImportError:
            gps_stub = types.ModuleType("gps")
            gps_stub.gps = mock.MagicMock
            gps_stub.WATCH_ENABLE = 1
            gps_stub.WATCH_NEWSTYLE = 2
            sys.modules["gps"] = gps_stub


_stub_hardware_modules()

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
import voice_nav_daemon as vnd  # noqa: E402
import phrases  # noqa: E402


class NormalizeTextTests(unittest.TestCase):
    def test_removes_filler_words(self):
        self.assertEqual(vnd.normalize_text("улица Ленина"), "ленина")

    def test_converts_number_words_to_digits(self):
        self.assertEqual(vnd.normalize_text("дом двадцать пять"), "25")

    def test_mixed_number_and_word_suffix(self):
        # "пять" -> "5", затем "б" не является числом и остаётся отдельным словом.
        self.assertEqual(vnd.normalize_text("дом пять б"), "5 б")

    def test_empty_input(self):
        self.assertEqual(vnd.normalize_text(""), "")

    def test_compound_number(self):
        # тридцать + три = 33
        self.assertEqual(vnd.normalize_text("проспект тридцать три"), "33")


class FindCoordinatesTests(unittest.TestCase):
    def setUp(self):
        self.db = {
            "ленина 25": (59.93, 30.31),
            "невский проспект 1": (59.93, 30.35),
        }
        self.keys = list(self.db.keys())

    def test_exact_match(self):
        name, coords = vnd.find_coordinates("улица ленина 25", self.keys, self.db)
        self.assertEqual(name, "ленина 25")
        self.assertEqual(coords, (59.93, 30.31))

    def test_no_match_below_cutoff(self):
        name, coords = vnd.find_coordinates("совершенно другой текст без адреса", self.keys, self.db)
        self.assertIsNone(name)
        self.assertIsNone(coords)

    def test_empty_input_no_match(self):
        name, coords = vnd.find_coordinates("", self.keys, self.db)
        self.assertIsNone(name)


class LogCapTests(unittest.TestCase):
    def setUp(self):
        # Временный каталог, а не фиксированный путь: тесты должны проходить
        # и на машине разработки, и на плате.
        self._tmpdir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self._tmpdir.name, "test_cap_log.raw")

    def tearDown(self):
        self._tmpdir.cleanup()

    def test_truncates_when_over_limit(self):
        with open(self.path, "wb") as f:
            f.write(b"x" * 100)
        vnd.cap_log_file(self.path, max_bytes=50)
        self.assertEqual(os.path.getsize(self.path), 0)

    def test_leaves_file_when_under_limit(self):
        with open(self.path, "wb") as f:
            f.write(b"x" * 10)
        vnd.cap_log_file(self.path, max_bytes=50)
        self.assertEqual(os.path.getsize(self.path), 10)

    def test_missing_file_does_not_raise(self):
        missing = self.path + ".missing"
        vnd.cap_log_file(missing, max_bytes=50)  # не должно бросить исключение


class WriteNavStatusTests(unittest.TestCase):
    def test_writes_1_for_active(self):
        m = mock.mock_open()
        with mock.patch("builtins.open", m):
            vnd.write_nav_status(True)
        m().write.assert_called_once_with("1")

    def test_writes_0_for_inactive(self):
        m = mock.mock_open()
        with mock.patch("builtins.open", m):
            vnd.write_nav_status(False)
        m().write.assert_called_once_with("0")

    def test_does_not_raise_on_io_error(self):
        with mock.patch("builtins.open", side_effect=OSError("no such device")):
            vnd.write_nav_status(True)  # не должно бросить исключение


class SpeakCommandSafetyTests(unittest.TestCase):
    """Текст в speak() приходит из распознанного голоса, то есть снаружи.

    Раньше здесь проверялось экранирование через shlex.quote внутри строки
    `flock -c '...'`. Потом speak() перешёл на argv-список без оболочки — это
    строго надёжнее, — а тест остался проверять `argv[2] == "-c"` и с тех пор
    просто падал. Сторож, который стабильно красный, не охраняет ничего:
    вернись подстановка в shell обратно, никто бы этого не заметил. Поэтому
    проверяется нынешняя гарантия — оболочки в цепочке нет вовсе, а текст
    уходит отдельным аргументом, где разбирать его некому.
    """

    SHELL_LAUNCHERS = {"-c", "sh", "bash", "/bin/sh", "/bin/bash"}

    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_text_passed_as_own_argv_element(self, mock_popen):
        malicious = "тест'; rm -rf / #"
        vnd.speak(malicious, sync=False)
        argv, _kwargs = mock_popen.call_args[0][0], mock_popen.call_args[1]
        # Фраза лежит отдельным элементом ровно как есть: её не склеивали
        # с соседями и не экранировали — экранировать нечему, разбора нет.
        self.assertIn(malicious, argv)
        self.assertEqual(argv[0], "flock")
        self.assertEqual(argv[2], vnd.SAY_SCRIPT)

    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_no_shell_anywhere_in_chain(self, mock_popen):
        vnd.speak("тест'; rm -rf / #", sync=False)
        args, kwargs = mock_popen.call_args
        argv = args[0]
        self.assertIsInstance(argv, list)
        # Ни shell=True у Popen, ни оболочки среди аргументов flock.
        self.assertFalse(kwargs.get("shell", False))
        self.assertFalse(self.SHELL_LAUNCHERS.intersection(argv))

    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_passes_argv_list_not_shell_string(self, mock_popen):
        vnd.speak("обычный текст", sync=False)
        args, kwargs = mock_popen.call_args
        self.assertIsInstance(args[0], list)
        self.assertFalse(kwargs.get("shell", False))

    @mock.patch("voice_nav_daemon.subprocess.run")
    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_sync_uses_run_not_popen(self, mock_popen, mock_run):
        mock_run.return_value.returncode = 0
        vnd.speak("текст", sync=True)
        mock_run.assert_called_once()
        mock_popen.assert_not_called()


class SpeakResultTests(unittest.TestCase):
    """say.sh теперь отличает "прозвучало" от "не прозвучало" кодом возврата.
    Пока он всегда возвращал успех, проверять было нечего, и немота гарнитуры
    выглядела в демоне как обычная работа."""

    @mock.patch("voice_nav_daemon.subprocess.run")
    def test_returns_true_when_played(self, mock_run):
        mock_run.return_value.returncode = 0
        self.assertTrue(vnd.speak("текст", sync=True))

    @mock.patch("voice_nav_daemon.subprocess.run")
    def test_returns_false_when_silent(self, mock_run):
        mock_run.return_value.returncode = 1
        self.assertFalse(vnd.speak("текст", sync=True))

    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_async_reports_launched(self, _mock_popen):
        # Результата ещё нет — ждать его в цикле навигации нельзя.
        self.assertTrue(vnd.speak("текст", sync=False))


class DirectionToRussianTests(unittest.TestCase):
    """Перевод названия манёвра. Живёт в phrases.py, а не в демоне: оттуда его
    берёт и сборщик кэша озвучки. Тесты долго звали vnd.direction_to_russian и
    падали на AttributeError с самого переезда."""

    def test_known_directions(self):
        self.assertEqual(phrases.direction_to_russian("left"), "налево")
        self.assertEqual(phrases.direction_to_russian("sharp right"), "резко направо")

    def test_unknown_direction_defaults_to_straight(self):
        self.assertEqual(phrases.direction_to_russian("teleport"), "прямо")


class GetRouteTests(unittest.TestCase):
    @mock.patch("voice_nav_daemon.requests.get")
    def test_returns_distance_and_steps_on_success(self, mock_get):
        mock_get.return_value.json.return_value = {
            "code": "Ok",
            "routes": [{"distance": 123.4, "legs": [{"steps": ["a", "b"]}]}],
        }
        dist, steps = vnd.get_route(59.9, 30.3, 59.91, 30.31)
        self.assertEqual(dist, 123.4)
        self.assertEqual(steps, ["a", "b"])

    @mock.patch("voice_nav_daemon.requests.get")
    def test_returns_none_on_non_ok_code(self, mock_get):
        mock_get.return_value.json.return_value = {"code": "NoRoute"}
        dist, steps = vnd.get_route(59.9, 30.3, 59.91, 30.31)
        self.assertIsNone(dist)
        self.assertIsNone(steps)

    @mock.patch("voice_nav_daemon.requests.get", side_effect=Exception("network down"))
    def test_returns_none_on_network_error(self, _mock_get):
        dist, steps = vnd.get_route(59.9, 30.3, 59.91, 30.31)
        self.assertIsNone(dist)
        self.assertIsNone(steps)


class SilentAudioTestCase(unittest.TestCase):
    """База для сценарных тестов: глушит всё, что реально трогает звук.

    play_ready_tone() до этого не мокали ни в одном сценарии, и он честно
    выполнялся: subprocess.run(["flock", ...]) с конвейером ffmpeg|aplay
    внутри. На машине разработки это FileNotFoundError и восемь красных
    тестов, а на плате — куда хуже: тест молча проходил, но по дороге
    переставлял громкость bluez-синка и играл сигнал в гарнитуру живого
    устройства. Тест, который дёргает боевое железо, — уже не тест.
    """

    def setUp(self):
        super().setUp()
        patcher = mock.patch("voice_nav_daemon.play_ready_tone")
        self.mock_ready_tone = patcher.start()
        self.addCleanup(patcher.stop)


class ConfirmAddressTests(SilentAudioTestCase):
    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="да все верно")
    def test_confirms_on_yes(self, _listen, _speak, _switch, _sleep):
        self.assertTrue(vnd.confirm_address(mock.MagicMock(), "Ленина 25"))

    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="нет, не туда")
    def test_rejects_on_no(self, _listen, _speak, _switch, _sleep):
        self.assertFalse(vnd.confirm_address(mock.MagicMock(), "Ленина 25"))

    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="")
    def test_rejects_on_silence(self, _listen, _speak, _switch, _sleep):
        self.assertFalse(vnd.confirm_address(mock.MagicMock(), "Ленина 25"))


class NavigationWorkerTests(SilentAudioTestCase):
    """Сценарии основного цикла макронавигации с полностью замоканными
    STT/TTS/GPS/OSRM — проверяем только переходы состояния NAV_ACTIVE."""

    def setUp(self):
        super().setUp()
        vnd.NAV_ACTIVE = True

    @mock.patch("voice_nav_daemon.write_nav_status")
    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="")
    def test_empty_stt_result_cancels_navigation(self, _listen, mock_speak, _switch, _sleep, _status):
        vnd.navigation_worker(mock.MagicMock(), {}, [])
        self.assertFalse(vnd.NAV_ACTIVE)
        mock_speak.assert_any_call("Адрес не распознан", sync=True)

    @mock.patch("voice_nav_daemon.write_nav_status")
    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="улица неизвестная")
    def test_unknown_address_cancels_navigation(self, _listen, mock_speak, _switch, _sleep, _status):
        vnd.navigation_worker(mock.MagicMock(), {}, [])
        self.assertFalse(vnd.NAV_ACTIVE)
        mock_speak.assert_any_call("Не могу найти этот адрес", sync=True)

    @mock.patch("voice_nav_daemon.write_nav_status")
    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.confirm_address", return_value=False)
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="ленина двадцать пять")
    def test_declined_confirmation_cancels_navigation(self, _listen, _confirm, mock_speak, _switch, _sleep, _status):
        db = {"ленина 25": (59.9, 30.3)}
        vnd.navigation_worker(mock.MagicMock(), db, list(db.keys()))
        self.assertFalse(vnd.NAV_ACTIVE)
        mock_speak.assert_any_call("Хорошо, отменяю", sync=True)

    @mock.patch("voice_nav_daemon.write_nav_status")
    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.confirm_address", return_value=True)
    @mock.patch("voice_nav_daemon.get_current_location", return_value=(None, None))
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="ленина двадцать пять")
    def test_missing_gps_fix_cancels_navigation(self, _listen, _loc, _confirm, mock_speak, _switch, _sleep, _status):
        db = {"ленина 25": (59.9, 30.3)}
        vnd.navigation_worker(mock.MagicMock(), db, list(db.keys()))
        self.assertFalse(vnd.NAV_ACTIVE)
        mock_speak.assert_any_call("ДЖИПИЭС координаты не доступны!", sync=True)

    @mock.patch("voice_nav_daemon.write_nav_status")
    @mock.patch("voice_nav_daemon.time.sleep")
    @mock.patch("voice_nav_daemon.switch_bt_profile")
    @mock.patch("voice_nav_daemon.speak")
    @mock.patch("voice_nav_daemon.confirm_address", return_value=True)
    @mock.patch("voice_nav_daemon.get_current_location", return_value=(59.9, 30.3))
    @mock.patch("voice_nav_daemon.get_route", return_value=(10.0, []))
    @mock.patch("voice_nav_daemon.listen_and_transcribe", return_value="ленина двадцать пять")
    def test_arrival_within_15m_ends_navigation(self, _listen, _route, _loc, _confirm, mock_speak, _switch, _sleep, _status):
        db = {"ленина 25": (59.9, 30.3)}
        vnd.navigation_worker(mock.MagicMock(), db, list(db.keys()))
        self.assertFalse(vnd.NAV_ACTIVE)
        mock_speak.assert_any_call("Вы достигли пункта назначения. Навигация завершена.", sync=True)


class SharedConstantsTests(unittest.TestCase):
    """Значения, продублированные в двух языках, обязаны совпадать.

    Общего кода у python-демона и shell-скриптов нет, поэтому такие константы
    выписаны дважды и держатся только на комментариях "не расходиться". А
    расхождение здесь даёт не отказ, а тихую порчу: диктовка, записанная на
    одной частоте и сведённая монтажом как другая, зазвучит в ролике не в том
    темпе, в каком её произносили, и никакой ошибки при этом не будет.
    """

    VOICE_ENV = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "tools", "voice_env.sh")
    )

    def _shell_value(self, name):
        with open(self.VOICE_ENV, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.startswith(f"{name}="):
                    return line.split("=", 1)[1].strip().strip('"').strip("'")
        self.fail(f"{name} не найден в {self.VOICE_ENV}")

    def test_headset_rate_matches_voice_env(self):
        self.assertEqual(str(vnd.HEADSET_RATE), self._shell_value("HEADSET_RATE"))

    def test_audio_lock_path_matches_voice_env(self):
        self.assertEqual(vnd.AUDIO_LOCK_PATH, self._shell_value("AUDIO_LOCK"))


class CacheablePhrasesTests(unittest.TestCase):
    """Кэш озвучки должен совпадать с тем, что устройство действительно говорит.

    Демон объявляет только те манёвры, что требуют действия, — а сборщик кэша
    долго синтезировал все восемь направлений, включая три никогда не звучащих.
    Это ровно тот промах кэша (в другую сторону), ради которого список фраз и
    вынесен из скрипта в код.
    """

    def test_only_announced_directions_are_cached(self):
        cached = set(phrases.cacheable_phrases())
        for direction in phrases.SILENT_DIRECTIONS:
            text = phrases.turn_phrase(10, direction)
            self.assertNotIn(text, cached, f"кэшируется непроизносимое: {text}")

    def test_every_announced_direction_is_cached(self):
        cached = set(phrases.cacheable_phrases())
        for direction in phrases.ANNOUNCED_DIRECTIONS:
            self.assertIn(phrases.turn_phrase(10, direction), cached)

    def test_daemon_filter_uses_the_same_list(self):
        # Тот же источник, что и у фильтра в navigation_worker: если фильтр
        # снова обзаведётся собственным списком, тест этого не поймает — а вот
        # общий импорт поймает уже компилятор глазами ревьюера.
        self.assertEqual(
            set(phrases.ANNOUNCED_DIRECTIONS) | set(phrases.SILENT_DIRECTIONS),
            set(phrases.ALL_DIRECTIONS),
        )


if __name__ == "__main__":
    unittest.main()
