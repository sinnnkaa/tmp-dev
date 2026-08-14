"""
Юнит-тесты для voice_nav_daemon.py.

Модули vosk и gps требуют железа/деплоя на плату и недоступны при разработке —
перед импортом демона они подменяются заглушками (см. _stub_hardware_modules),
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


def _stub_hardware_modules():
    """Подставляет заглушки vosk/gps в sys.modules, если они не установлены."""
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
    """Текст в speak() может приходить из распознанного голоса — важно, чтобы
    он не мог вырваться из shell-строки, которую строит flock -c '...'."""

    def _captured_inner_command(self, mock_popen, text):
        vnd.speak(text, sync=False)
        args, _kwargs = mock_popen.call_args
        argv = args[0]
        self.assertEqual(argv[0], "flock")
        self.assertEqual(argv[2], "-c")
        return argv[3]

    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_quotes_shell_metacharacters(self, mock_popen):
        malicious = "тест'; rm -rf / #"
        inner = self._captured_inner_command(mock_popen, malicious)
        # speak() обязан прогонять текст через shlex.quote перед подстановкой
        # в shell-команду — иначе одиночная кавычка в распознанном голосом
        # тексте могла бы оборвать echo и внедрить произвольную shell-команду.
        self.assertIn(vnd.shlex.quote(malicious), inner)

    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_passes_argv_list_not_shell_string(self, mock_popen):
        vnd.speak("обычный текст", sync=False)
        args, kwargs = mock_popen.call_args
        self.assertIsInstance(args[0], list)
        self.assertNotIn("shell", kwargs)

    @mock.patch("voice_nav_daemon.subprocess.run")
    @mock.patch("voice_nav_daemon.subprocess.Popen")
    def test_sync_uses_run_not_popen(self, mock_popen, mock_run):
        vnd.speak("текст", sync=True)
        mock_run.assert_called_once()
        mock_popen.assert_not_called()


class DirectionToRussianTests(unittest.TestCase):
    def test_known_directions(self):
        self.assertEqual(vnd.direction_to_russian("left"), "налево")
        self.assertEqual(vnd.direction_to_russian("sharp right"), "резко направо")

    def test_unknown_direction_defaults_to_straight(self):
        self.assertEqual(vnd.direction_to_russian("teleport"), "прямо")


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


class ConfirmAddressTests(unittest.TestCase):
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


class NavigationWorkerTests(unittest.TestCase):
    """Сценарии основного цикла макронавигации с полностью замоканными
    STT/TTS/GPS/OSRM — проверяем только переходы состояния NAV_ACTIVE."""

    def setUp(self):
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


if __name__ == "__main__":
    unittest.main()
