# SPDX-License-Identifier: GPL-2.0-or-later
"""The PROTOTYPE launcher's languages: English (the texts in prototype_launcher.py) and Russian.

tr(text) gives a text in the launcher's language: an exact entry of RU, else the first of
PATTERNS that matches the whole text (texts with numbers or names in them), else the text as it is.
The language is the launcher setting ui_language: 'en', 'ru', or 'auto' (Russian when Windows or
the system locale is Russian)."""
import functools
import locale
import os
import re
import sys

LANGUAGE = 'en'


@functools.lru_cache(maxsize=1)
def system_russian():
    if sys.platform == 'win32':
        try:
            import ctypes
            # LANGID: primary language 0x19 is Russian.
            if ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff == 0x19:
                return True
        except (AttributeError, OSError):
            pass
    for name in ('LC_ALL', 'LC_MESSAGES', 'LANG'):
        if os.environ.get(name, '').lower().startswith('ru'):
            return True
    try:
        return (locale.getlocale()[0] or '').lower().startswith(('ru', 'russian'))
    except ValueError:
        return False


def set_language(setting):
    """'en', 'ru' or 'auto'; returns the language now used."""
    global LANGUAGE
    LANGUAGE = 'ru' if setting == 'ru' or (setting not in ('en', 'ru') and system_russian()) else 'en'
    return LANGUAGE


def russian():
    return LANGUAGE == 'ru'


def tr(text):
    if LANGUAGE != 'ru' or not text:
        return text
    exact = RU.get(text)
    if exact is not None:
        return exact
    for pattern, replacement in PATTERNS:
        m = pattern.fullmatch(text)
        if m:
            return m.expand(replacement)
    return text


RU = {
    # ---- header, feed, the file
    '//  CONTAINMENT TERMINAL 7-A  ·  MANHATTAN ISLAND': '//  ТЕРМИНАЛ СДЕРЖИВАНИЯ 7-A  ·  ОСТРОВ МАНХЭТТЕН',
    'CAM 04  //  SUBJECT FEED': 'КАМ 04  //  НАБЛЮДЕНИЕ ЗА СУБЪЕКТОМ',
    'REC': 'ЗАП',
    'NO SIGNAL': 'НЕТ СИГНАЛА',
    '> SELECT THE GAME FOLDER': '> ВЫБЕРИТЕ ПАПКУ ИГРЫ',
    'ID MATCH 99.2%': 'СОВПАДЕНИЕ 99,2%',
    'CLASSIFIED': 'СЕКРЕТНО',
    'SUBJECT': 'СУБЪЕКТ', 'CODENAME': 'ПОЗЫВНОЙ', 'FILED': 'ДОСЬЕ ОТ', 'STATUS': 'СТАТУС',
    'ALEX MERCER': 'АЛЕКС МЕРСЕР', 'ZEUS': 'ЗЕВС', 'SGT. JAMES HELLER': 'СЕРЖ. ДЖЕЙМС ХЕЛЛЕР',
    'EXPERIMENTAL': 'ЭКСПЕРИМЕНТ',
    '[ CHANGE ]': '[ ИЗМЕНИТЬ ]',
    'RUNNING': 'ЗАПУЩЕНА', 'STOP': 'СТОП', 'PLAY': 'ИГРАТЬ', '[ ENTER ]': '[ ENTER ]', 'LOCKED': 'ЗАКРЫТО',
    'Preparing': 'Подготовка',
    '> INTEGRITY SCAN': '> ПРОВЕРКА ФАЙЛОВ',
    'GAME FOLDER': 'ПАПКА ИГРЫ', 'SAVES': 'СОХРАНЕНИЯ', 'BACKUPS': 'КОПИИ', 'LOG': 'ЛОГ', 'PORT FOLDER': 'ПАПКА ПОРТА',
    # ---- the console
    'GAME': 'ИГРА', 'GRAPHICS': 'ГРАФИКА', 'DISPLAY': 'ЭКРАН', 'CONTROLS': 'УПРАВЛЕНИЕ', 'CONTROLLER': 'ГЕЙМПАД',
    'FOLDER': 'ПАПКА', 'INFO': 'ИНФО',
    'Point at a setting to read what it does. Keys 1-6 switch the pages, Enter plays.':
        'Наведите на настройку, чтобы прочитать, что она делает. Клавиши 1–6 переключают страницы, Enter запускает игру.',
    'ON': 'ВКЛ', 'OFF': 'ВЫКЛ', 'Off': 'Выкл',
    # ---- the integrity scan (check_game)
    'Game folder': 'Папка игры', 'Game': 'Игра', 'not selected': 'не выбрана',
    'sce_sys/param.sfo missing': 'нет sce_sys/param.sfo',
    'PROTOTYPE executable': 'Файл PROTOTYPE', 'PROTOTYPE 2 executable': 'Файл PROTOTYPE 2',
    'missing': 'нет', 'original': 'оригинал', 'modified (Restore fixes it)': 'изменён (исправит Восстановить)',
    'System modules': 'Системные модули', 'sce_module missing': 'нет sce_module',
    # ---- GAME page
    'Render resolution': 'Разрешение рендера',
    'Experimental. The resolution PROTOTYPE 2 renders at; 1920 × 1080 is the PS4\'s. Above it the game is given '
    'more video memory. Lower is scaled up by FSR 1. If the game misbehaves, set it back to 1920 × 1080.':
        'Экспериментально. Разрешение, в котором рисует PROTOTYPE 2; 1920 × 1080 — как на PS4. Выше — игре '
        'даётся больше видеопамяти. Ниже — растягивается FSR 1. Если игра ведёт себя странно, верните 1920 × 1080.',
    'Frame rate': 'Частота кадров',
    'Experimental. 60 FPS lifts the game\'s 30 FPS cap. If the game runs too fast or a menu misbehaves, go back to '
    '30 FPS.':
        'Экспериментально. 60 FPS снимает ограничение игры в 30 FPS. Если игра идёт слишком быстро или меню '
        'ведёт себя странно, верните 30 FPS.',
    'Experimental. The resolution the game renders at; 1920 × 1080 is the PS4\'s. Higher supersamples (sharper, '
    'heavier); lower is scaled up by FSR 1. If the game misbehaves, set it back to 1920 × 1080. Also the window\'s '
    'size (Windowed) and the screen mode (Fullscreen).':
        'Экспериментально. Разрешение, в котором рисует игра; 1920 × 1080 — как на PS4. Выше — суперсэмплинг '
        '(чётче, тяжелее), ниже — растягивается FSR 1. Если игра ведёт себя странно, верните 1920 × 1080. Также '
        'это размер окна (В окне) и режим экрана (Полный экран).',
    '60 FPS: gameplay at 60; videos and menus keep their normal speed.':
        '60 FPS: игра идёт в 60 кадрах; видео и меню — с обычной скоростью.',
    'White tendrils fix': 'Исправление белых щупалец',
    'Consume tendrils keep their red veins (a GPU emulation difference). Keep it on.':
        'Щупальца поглощения сохраняют красные прожилки (различие эмуляции GPU). Оставьте включённым.',
    'Discord status': 'Статус в Discord',
    'Your Discord profile shows "Playing PROTOTYPE": in the menus or playing, the story mission and the time '
    'played, with a link to the port. Needs the Discord app running.':
        'Профиль Discord показывает «Играет в PROTOTYPE»: в меню или в игре, сюжетное задание и время в игре, '
        'со ссылкой на порт. Нужно запущенное приложение Discord.',
    'Graphics, display and controls are shared by both games.': 'Графика, экран и управление общие для обеих игр.',
    'PROTOTYPE 2 runs, but is newer in the port than PROTOTYPE: expect rough edges. Keyboard button icons are not '
    'in yet; the game shows PlayStation buttons. [ LOG ] at the bottom right shows what happened if it stops.':
        'PROTOTYPE 2 работает, но в порте он новее, чем PROTOTYPE: возможны шероховатости. Иконок клавиатуры пока '
        'нет; игра показывает кнопки PlayStation. Если игра остановится, [ ЛОГ ] справа внизу покажет почему.',
    'LAUNCH PROFILE  ·  SHARED': 'ПРОФИЛЬ ЗАПУСКА  ·  ОБЩИЙ',
    'UPSCALING': 'МАСШТАБИРОВАНИЕ', 'SHARPENING': 'РЕЗКОСТЬ', 'V-SYNC': 'V-SYNC', 'FILTERING': 'ФИЛЬТРАЦИЯ',
    'MOUSE LOOK': 'ОБЗОР МЫШЬЮ', 'BUTTON ICONS': 'ИКОНКИ КНОПОК', 'FRAME COUNTER': 'СЧЁТЧИК КАДРОВ',
    'WINDOWED': 'В ОКНЕ', 'BORDERLESS': 'БЕЗ РАМКИ', 'FULLSCREEN': 'ПОЛНЫЙ ЭКРАН',
    'AUTOMATIC': 'АВТО', 'KEYBOARD & MOUSE': 'КЛАВИАТУРА И МЫШЬ', 'PLAYSTATION': 'PLAYSTATION',
    # ---- GRAPHICS page
    'IMAGE  ·  BOTH GAMES': 'ИЗОБРАЖЕНИЕ  ·  ОБЕ ИГРЫ',
    'FSR 1 upscaling': 'Масштабирование FSR 1',
    'AMD FidelityFX Super Resolution 1: scales the game image to the window with edge-aware filtering. Only active '
    'when the window is larger than the image.':
        'AMD FidelityFX Super Resolution 1: растягивает изображение игры до размера окна с учётом краёв. Работает, '
        'только когда окно больше изображения.',
    'Sharpening': 'Резкость', 'Contrast-adaptive sharpening (RCAS) of the final image.':
        'Контрастно-адаптивная резкость (RCAS) итогового изображения.',
    'Sharpness': 'Сила резкости', 'Strength of the sharpening.': 'Насколько сильна резкость.',
    'Texture filtering': 'Фильтрация текстур',
    'Anisotropic filtering: sharper ground and walls at steep angles.':
        'Анизотропная фильтрация: чётче земля и стены под острым углом.',
    'Game default': 'Как в игре',
    'Render resolution and frame rate are on the GAME page: each game has its own.':
        'Разрешение рендера и частота кадров — на странице ИГРА: у каждой игры свои.',
    # ---- DISPLAY page
    'WINDOW': 'ОКНО', 'Display mode': 'Режим экрана',
    'Borderless fills the screen and switches instantly with Alt+Tab. PROTOTYPE: Windowed is a window of the render '
    'resolution, Fullscreen switches the screen to it.':
        'Без рамки заполняет экран и мгновенно переключается по Alt+Tab. PROTOTYPE: В окне — окно размера '
        'разрешения рендера, Полный экран переключает экран на него.',
    'Windowed': 'В окне', 'Borderless': 'Без рамки', 'Fullscreen': 'Полный экран',
    'V-Sync': 'Верт. синхронизация', 'Waits for the display refresh: no tearing.':
        'Ждёт обновления экрана: без разрывов кадра.',
    'Frame counter': 'Счётчик кадров', 'Shows the frame rate in a corner of the screen.':
        'Показывает частоту кадров в углу экрана.',
    'IN GAME': 'В ИГРЕ',
    'Insert (keyboard) or R3 + L2 (controller) opens the port menu while playing: FSR 3.1 / FSR 4 upscaling and '
    'the other graphics options.':
        'Insert (клавиатура) или R3 + L2 (геймпад) открывают меню порта во время игры: масштабирование FSR 3.1 / '
        'FSR 4 и другие настройки графики.',
    # ---- CONTROLS page
    'KEYBOARD AND MOUSE': 'КЛАВИАТУРА И МЫШЬ',
    'Button icons in game': 'Иконки кнопок в игре',
    'The buttons the game shows in its hints and tutorials: your keys and mouse buttons (below), or the '
    'PlayStation buttons. Automatic: the ones of what you played with last time. Takes effect at the next start.':
        'Кнопки, которые игра показывает в подсказках и обучении: ваши клавиши и кнопки мыши (ниже) или кнопки '
        'PlayStation. Авто: того устройства, на котором вы играли в прошлый раз. Применяется при следующем запуске.',
    'Automatic': 'Авто', 'Keyboard & mouse': 'Клавиатура и мышь', 'PlayStation': 'PlayStation',
    'Mouse look': 'Обзор мышью',
    'The mouse turns the camera as in PC games: by how far you move it, with no dead zone or speed limit (F7 '
    'switches it in game).':
        'Мышь поворачивает камеру как в играх для ПК: ровно настолько, насколько её сдвинули, без мёртвой зоны и '
        'ограничения скорости (F7 переключает в игре).',
    'Mouse sensitivity': 'Чувствительность мыши',
    'How fast the mouse turns the camera (also in the in-game menu).':
        'Как быстро мышь поворачивает камеру (есть и в меню игры).',
    'Keys for': 'Клавиши для',
    'Click a key box, then press the key or mouse button for it (click the box again for the left button). Delete '
    'clears the box; a click anywhere else cancels. A key is only on one action at a time.':
        'Нажмите на поле клавиши, затем нажмите клавишу или кнопку мыши (для левой кнопки — щёлкните поле ещё '
        'раз). Delete очищает поле; щелчок в другом месте отменяет. Одна клавиша — только на одном действии.',
    'Click a box, then press a key or a mouse button (click the box again for the left button). Delete clears it; '
    'a click anywhere else cancels.':
        'Нажмите на поле, затем клавишу или кнопку мыши (для левой кнопки — щёлкните поле ещё раз). Delete '
        'очищает; щелчок в другом месте отменяет.',
    'PRESS A KEY_': 'НАЖМИТЕ…',
    'RESET KEYS': 'СБРОСИТЬ КЛАВИШИ', 'Puts every key back to the port\'s layout.': 'Возвращает все клавиши порта.',
    'Orange: a key on two different buttons': 'Оранжевым: клавиша на двух разных кнопках',
    'Left click': 'ЛКМ', 'Right click': 'ПКМ', 'Middle click': 'СКМ', 'Mouse 4': 'Мышь 4', 'Mouse 5': 'Мышь 5',
    'Wheel Up': 'Колесо ↑', 'Wheel Down': 'Колесо ↓', 'Space': 'Пробел',
    # ---- CONTROLLER page
    'GAMEPAD  ·  DUALSENSE, DUALSHOCK 4 AND OTHERS': 'ГЕЙМПАД  ·  DUALSENSE, DUALSHOCK 4 И ДРУГИЕ',
    'Vibration': 'Вибрация', 'Strength of the game\'s vibration (rumble). Off: none.':
        'Сила вибрации игры. Выкл: без вибрации.',
    'Light bar': 'Световая панель',
    'DualSense and DualShock 4: the light bar shows the colours the game sets, as on the PS4. Off: the controller\'s '
    'own colour.':
        'DualSense и DualShock 4: световая панель показывает цвета игры, как на PS4. Выкл: собственный цвет '
        'контроллера.',
    'Menu vibration': 'Вибрация в меню',
    'A short tap when you press a button in the game\'s menus (select, back) and scroll the main menu, or also when '
    'you move between items in every menu.':
        'Короткий толчок при нажатии в меню игры (выбор, назад) и прокрутке главного меню, или также при переходе '
        'между пунктами в любом меню.',
    'Presses': 'Нажатия', 'All': 'Все',
    'Action vibration': 'Вибрация действий',
    'The port\'s own vibration in the game: consuming, the whipfist, wall running, powers, landings, explosions, '
    'blows, getting hit. Off: only the game\'s own.':
        'Вибрация порта в игре: поглощение, хлыст, бег по стенам, способности, приземления, взрывы, удары, '
        'попадания по вам. Выкл: только вибрация самой игры.',
    'HD haptics': 'HD-вибрация',
    'DualSense on a USB cable: all vibration plays on its haptic motors, with a feel of its own for each move (a '
    'whip\'s crack, steps on a wall, landings). Bluetooth: the plain rumble.':
        'DualSense по USB-кабелю: вся вибрация идёт через его тактильные моторы, у каждого движения своё ощущение '
        '(щелчок хлыста, шаги по стене, приземления). По Bluetooth — обычная вибрация.',
    'Sound haptics': 'Звук в вибрации',
    'HD haptics also play the game\'s sound: explosions, impacts, gunfire, tanks (not the music).':
        'HD-вибрация передаёт и звук игры: взрывы, удары, стрельбу, танки (не музыку).',
    'Low': 'Слабо', 'Medium': 'Средне', 'High': 'Сильно',
    'All work live: they are also in the game\'s own Options > Controller screen and on the port menu\'s CONTROLLER '
    'page (Insert or R3 + L2). Keyboard keys and mouse are on the CONTROLS page.':
        'Всё применяется сразу: эти настройки есть и в самой игре (Настройки > Контроллер), и на странице '
        'КОНТРОЛЛЕР меню порта (Insert или R3 + L2). Клавиатура и мышь — на странице УПРАВЛЕНИЕ.',
    # ---- FOLDER page
    'Not selected': 'Не выбрана', 'CHANGE': 'ИЗМЕНИТЬ',
    'Choose the folder of your game dump (the one with eboot.bin, sce_sys, sce_module, p1 and p2).':
        'Выберите папку вашего дампа игры (с eboot.bin, sce_sys, sce_module, p1 и p2).',
    'PREPARE': 'ПОДГОТОВИТЬ',
    'Converts the game into the native image now (also done at every start).':
        'Преобразует игру в нативный образ сейчас (это делается и при каждом запуске).',
    'RESTORE ORIGINAL': 'ВОССТАНОВИТЬ ОРИГИНАЛ',
    'Puts the original executables back from the .orig backups.': 'Возвращает оригинальные файлы из копий .orig.',
    'CLEAN UP': 'ОЧИСТИТЬ',
    'Deletes files earlier fixes and debugging left in the game folder. The game files themselves are kept.':
        'Удаляет файлы, оставленные в папке игры прежними исправлениями и отладкой. Сами файлы игры не трогает.',
    'OPEN SAVE FOLDER': 'ПАПКА СОХРАНЕНИЙ', 'Your saves: user\\savedata in the port folder.':
        'Ваши сохранения: user\\savedata в папке порта.',
    'OPEN BACKUPS': 'РЕЗЕРВНЫЕ КОПИИ',
    'A copy of your saves is made at every start (the latest ones are kept).':
        'Копия сохранений делается при каждом запуске (хранятся последние).',
    'LAUNCHER': 'ЛАУНЧЕР', 'Launcher language': 'Язык лаунчера',
    'The launcher\'s language. Automatic: Russian when Windows is in Russian. The game\'s own language comes from '
    'your game files.':
        'Язык лаунчера. Авто: русский, если Windows на русском. Язык самой игры зависит от ваших файлов игры.',
    # ---- statuses and dialogs
    'Select your game folder to begin': 'Выберите папку игры, чтобы начать',
    'Ready to play': 'Готово к игре',
    'No saves yet: they appear after the first save in game': 'Сохранений пока нет: они появятся после первого сохранения',
    'No save backups yet': 'Резервных копий сохранений пока нет',
    'No log yet: start the game once': 'Лога пока нет: запустите игру',
    'Fix the game folder first': 'Сначала исправьте папку игры',
    'Fix the game folder first (6 FOLDER)': 'Сначала исправьте папку игры (6 ПАПКА)',
    'Game prepared: native image ready': 'Игра подготовлена: нативный образ готов',
    'Original executables restored': 'Оригинальные файлы восстановлены',
    'Nothing to clean: the game folder holds only the game': 'Чистить нечего: в папке только файлы игры',
    'Starting… the game window opens in a moment': 'Запуск… окно игры сейчас откроется',
    'Game closed': 'Игра закрыта',
    'The game asked for an executable that is not in the game folder': 'Игра запросила файл, которого нет в папке игры',
    'PROTOTYPE BIOHAZARD BUNDLE folder (eboot.bin, sce_sys, p1, p2)':
        'Папка PROTOTYPE BIOHAZARD BUNDLE (eboot.bin, sce_sys, p1, p2)',
    'Restore original files': 'Восстановление оригинальных файлов',
    'Clean up the game folder': 'Очистка папки игры',
    'The game files themselves are kept.': 'Сами файлы игры сохраняются.',
}

PATTERNS = [(re.compile(p, re.S), r) for p, r in (
    (r'BIOHAZARD BUNDLE  ·  NATIVE PC PORT v(.+)', r'BIOHAZARD BUNDLE  ·  НАТИВНЫЙ PC-ПОРТ v\1'),
    (r'FILE (\d+)', r'ДЕЛО \1'),
    (r'(.+) PATCHES', r'ПАТЧИ \1'),
    (r'ON · (.+)', r'ВКЛ · \1'),
    (r'(.+) data', r'Данные \1'),
    (r'(.+) · version (.+)', r'\1 · версия \2'),
    (r'(.+) is not (.+)', r'\1 — не \2'),
    (r'(.+) found', r'папка \1 найдена'),
    (r'(.+) folder missing', r'нет папки \1'),
    (r'These apply to (.+?) only: (.+?) keeps its own \(switch with the file tabs on the left\)\. '
     r'Graphics, display and controls are shared by both games\. PROTOTYPE\'s resolution and frame rate are also '
     r'on its own Options > Display screen\.',
     r'Это только для \1: у \2 свои (переключение — вкладками дел слева). Графика, экран и управление общие для '
     r'обеих игр. Разрешение и частота кадров PROTOTYPE есть и в самой игре: Настройки > Дисплей.'),
    (r'These apply to (.+?) only: (.+?) keeps its own \(switch with the file tabs on the left\)\. '
     r'Graphics, display and controls are shared by both games\.',
     r'Это только для \1: у \2 свои (переключение — вкладками дел слева). Графика, экран и управление общие для '
     r'обеих игр.'),
    (r'(.+): not selected', r'\1: не выбрана'),
    (r'Cannot open (.+?): (.+)', r'Не удалось открыть \1: \2'),
    (r'Restore failed: (.+)', r'Восстановление не удалось: \1'),
    (r'Removed (\d+) files, (\d+) failed', r'Удалено файлов: \1, не удалось: \2'),
    (r'Removed (\d+) files', r'Удалено файлов: \1'),
    (r'Could not start: (.+)', r'Не удалось запустить: \1'),
    (r'The game stopped \(code (.+)\)\. \[ LOG \] shows why\.', r'Игра остановилась (код \1). [ ЛОГ ] покажет почему.'),
    (r'(.+) failed: (.+)', r'\1 — ошибка: \2'),
    (r'Replace these with their original backups\?\n\n(.*)', r'Заменить их оригинальными копиями?\n\n\1'),
    (r'Delete (\d+) leftover files \((\d+) MB\)\?\n\n(.*)\n\nThe game files themselves are kept\.',
     r'Удалить оставшиеся файлы (\1, \2 МБ)?\n\n\3\n\nСами файлы игры сохраняются.'),
)]
