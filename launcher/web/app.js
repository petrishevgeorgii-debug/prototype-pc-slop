// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE launcher: the page. Everything it shows comes from the launcher process
// (launcher/prototype_web.py) through /api/<action>; every change goes back the same way.
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;

// ---------------------------------------------------------------- texts
const TEXT = {
  ru: {
    brandSub: 'BIOHAZARD BUNDLE · ПОРТ НА ПК', p1Sub: '2009 · АЛЕКС МЕРСЕР', p2Sub: '2012 · ДЖЕЙМС ХЕЛЛЕР',
    navHome: 'Главная', navGraphics: 'Графика', navDisplay: 'Экран', navKeys: 'Клавиши', navPad: 'Геймпад',
    navSaves: 'Сохранения', navMods: 'Моды', navNews: 'Новое', navNewsTitle: 'Что нового', navFolder: 'Папка',
    rec: 'ЗАПИСЬ', lastSave: 'Последнее сохранение', launchSettings: 'НАСТРОЙКИ ЗАПУСКА',
    imageBoth: 'Изображение — для обеих игр', onlyFor: 'Только для {game}',
    gfxLedeP1: 'Разрешение и частота кадров у каждой игры свои, остальное общее. Частоту кадров PROTOTYPE можно менять и в самой игре: Настройки › Экран.',
    gfxLedeP2: 'PROTOTYPE 2 появился в порте позже первой части, так что возможны недочёты. Настройки изображения общие для обеих игр.',
    displayLede: 'Как игра выводится на экран и что показывается поверх неё.', window: 'Окно', inGame: 'В игре',
    inGameNote: '<b>Insert</b> на клавиатуре или <b>R3 + L2</b> на геймпаде открывают меню порта прямо в игре: там FSR 3.1, FSR 4 и остальные настройки графики. <b>F1</b> — трейнер, <b>F3</b> прячет интерфейс, <b>Ё</b> открывает Lua-консоль.',
    keysTitle: 'Клавиатура и мышь', keysLede: 'Нажмите на поле рядом с действием, а затем новую клавишу или кнопку мыши. Одну клавишу нельзя назначить на два действия.',
    mouseIcons: 'Мышь и иконки', actions: 'Действия', resetKeys: 'Сбросить клавиши',
    keysHint: 'Для левой кнопки мыши щёлкните по полю ещё раз. Delete очищает поле, щелчок в другом месте отменяет.',
    pressKey: 'НАЖМИТЕ…', reservedKey: 'Клавиша {key} занята самим портом',
    padLede: 'DualSense, DualShock 4, Xbox и другие геймпады. Изменения действуют сразу; те же настройки есть в меню самой игры и в меню порта.',
    vibration: 'Вибрация',
    savesLede: 'Все ячейки с миссией, сложностью и временем сохранения. При каждом запуске делается резервная копия, и из неё можно вернуть любую ячейку.',
    backupNow: 'Создать копию', openFolder: 'Открыть папку', backups: 'Резервные копии', restore: 'Восстановить',
    noSavesYet: 'Сохранений этой игры пока нет: они появятся после первого сохранения в игре.',
    noBackups: 'Копий пока нет: первая появится при следующем запуске игры.',
    latest: 'последнее', slot: 'Ячейка {slot}', auto: 'автосохранение',
    difficulty: 'Уровень сложности: {d}',
    modsLede: 'Шейдер-моды меняют то, как игра рисует картинку. Файлы игры при этом не трогаются: моды подключаются при запуске.',
    shaderMods: 'Шейдер-моды', openMods: 'Открыть папку модов', luaScripts: 'Lua-скрипты', in12: 'появятся в 1.2',
    modsNote: 'Мод — это папка в <b>mods</b> с папкой <b>shaders</b> внутри. Как найти и изменить шейдер, написано в <b>docs/SHADER_MODS.md</b>.',
    luaNote: 'Скрипты будут выполняться в Lua самой игры после загрузки сохранения, так же как команды в консоли (<b>Ё</b> в игре). Например: бесконечное здоровье или замедление времени по клавише.',
    noMods: 'Модов пока нет.', looseShaders: 'Отдельные шейдеры в mods/shaders: {n}', shaderFiles: '{n} {files}',
    modOn: '«{name}» включится при следующем запуске', modOff: '«{name}» выключится при следующем запуске',
    gameFolder: 'Папка игры', folderLede: 'Ваш дамп PROTOTYPE BIOHAZARD BUNDLE. Порт только читает эти файлы и ничего в них не меняет.',
    change: 'Изменить', prepare: 'Подготовить игру', preparing: 'Подготовка…', restoreOriginals: 'Вернуть оригинальные файлы',
    clean: 'Очистить папку', openLog: 'Открыть лог', launcher: 'Лаунчер', notSelected: 'Папка не выбрана',
    log: 'ЛОГ', portFolder: 'ПАПКА ПОРТА', hide: 'Скрыть',
    play: 'ИГРАТЬ', stop: 'ОСТАНОВИТЬ', running: 'ИДЁТ ИГРА', starting: 'ЗАПУСК {game}',
    launchNote: 'Окно игры откроется через несколько секунд', launchRunning: 'Игра запущена',
    prepState: 'Подготовка', prepReady: 'готово', prepFirst: 'при запуске', cache: 'Кэш шейдеров',
    playTime: 'Время в игре', never: 'ещё не играли', discordOn: 'статус включён', discordOff: 'выключен',
    hours: '{h} ч {m} мин', minutes: '{m} мин',
    stFiles: 'Файлы игры', stPrep: 'Подготовка', stSupport: 'Поддержка', stIcons: 'Иконки кнопок',
    filesOk: 'проверены, {exe} без изменений', filesBad: '{what}: {detail}', prepDone: 'готово, взято из кэша',
    prepLater: 'будет сделана при запуске', supportFull: 'полная', supportExp: 'экспериментальная',
    iconsXbox: 'Xbox', iconsPS: 'PlayStation', iconsKeys: 'клавиатура', iconsAuto: 'по последнему устройству',
    padConnected: 'геймпад подключён', p2IconsNote: 'у PROTOTYPE 2 пока только PlayStation',
    pfRes: 'Разрешение', pfFps: 'Частота', pfFsr: 'FSR 1', pfSharp: 'Резкость', pfScreen: 'Экран', pfVsync: 'V-Sync',
    pfIcons: 'Иконки', pfMouse: 'Мышь', on: 'вкл.', off: 'выкл.', look: 'обзор, {n}',
    p1Cam: 'КАМЕРА 04 · ОБЪЕКТ ПОД НАБЛЮДЕНИЕМ', p2Cam: 'КАМЕРА 11 · НЬЮ-ЙОРК ЗЕРО',
    p1Wall: 'КАМЕРЫ · МАНХЭТТЕН', p2Wall: 'КАМЕРЫ · НЬЮ-ЙОРК ЗЕРО',
    p1Foot: 'Кадры миссий взяты из ваших файлов игры.', p2Foot: 'Сюжетные кадры взяты из файлов PROTOTYPE 2.',
    noArt: 'Кадры появятся, когда будет выбрана папка игры.', noSignal: 'НЕТ СИГНАЛА', camera: 'КАМЕРА',
    zones: ['ЖЁЛТАЯ ЗОНА', 'ЗЕЛЁНАЯ ЗОНА', 'КРАСНАЯ ЗОНА'],
    threats: ['ВЫСОКАЯ УГРОЗА', 'ЗАРАЖЕНИЕ 34%', 'ЦЕЛЬ НЕ ОПОЗНАНА', 'ЗАРАЖЕНИЕ 61%', 'КАРАНТИН'],
    heroes: {
      p1: ['АЛЕКС', 'МЕРСЕР', [['ПОЗЫВНОЙ', 'ЗЕВС'], ['ДОСЬЕ ЗАВЕДЕНО', '18.09.2008'], ['СТАТУС', 'НА СВОБОДЕ'], ['ОПОЗНАН НА', '99,2%']]],
      p2: ['ДЖЕЙМС', 'ХЕЛЛЕР', [['ЗВАНИЕ', 'СЕРЖАНТ'], ['ДОСЬЕ ЗАВЕДЕНО', '14.08.2009'], ['СТАТУС', 'ЗАРАЖЁН'], ['ОПОЗНАН НА', '97,6%']]]
    },
    cells: ['ячейка', 'ячейки', 'ячеек'], files: ['файл', 'файла', 'файлов'],
    updLatest: 'У вас последняя версия', updTest: 'Тестовая сборка {v}', updNewer: 'Вышла версия {v}',
    updLatestText: 'Последний выпуск на GitHub — {v}.', updNewerText: 'Её можно скачать на GitHub: настройки и сохранения останутся.',
    updFail: 'Не удалось проверить обновления', updFailText: 'Нет связи с GitHub. Лаунчер попробует снова при следующем запуске.',
    updChecking: 'Проверяем обновления…', checkUpdates: 'Проверить обновления', download: 'Открыть страницу',
    yes: 'Да', no: 'Отмена', ok: 'Понятно',
    restoreTitle: 'Восстановить сохранения?', restoreText: 'Сохранения вернутся к копии от {when}. Текущие перед этим тоже попадут в копию, так что это можно отменить.',
    cleanTitle: 'Очистить папку игры?', cleanText: 'Будут удалены {n} лишних {files} ({mb} МБ), оставшихся от прежних исправлений и отладки. Сами файлы игры не тронуты.',
    cleanNothing: 'Чистить нечего: в папке только файлы игры', restoreOrigTitle: 'Вернуть оригинальные файлы?',
    restoreOrigText: 'Изменённые исполняемые файлы будут заменены оригиналами из резервных копий .orig.',
    n_fixFolder: 'Сначала исправьте папку игры', n_prepared: 'Игра подготовлена к запуску',
    n_cleaned: 'Удалено файлов: {n}', n_restored: 'Оригинальные файлы возвращены', n_backedUp: 'Копия сохранений создана',
    n_noSaves: 'Сохранений пока нет: копировать нечего', n_closeGameFirst: 'Сначала закройте игру',
    n_backupRestored: 'Сохранения восстановлены', n_openFailed: 'Папка ещё не создана',
    exitOk: 'Игра закрыта', exitCode: 'Игра остановилась (код {code}). Подробности — в логе.',
    exit42: 'Игра запросила файл, которого нет в папке игры',
    langAuto: 'Как в Windows',
    rows: {
      res: ['Разрешение рендера', 'В каком разрешении игра рисует кадр; на PS4 это 1920 × 1080. Выше — чётче, но тяжелее для видеокарты, ниже — картинку растянет FSR 1.'],
      fpsP1: ['Частота кадров', 'С 60 FPS игра идёт плавнее, а ролики и меню — с обычной скоростью. Всё, что выше 60, пока экспериментально; меню игры при этом остаются на 60.'],
      fpsP2: ['Частота кадров', '60 FPS снимает ограничение игры в 30 кадров. Если игра начнёт ускоряться или меню будут вести себя странно, верните 30.'],
      tendrils: ['Красные щупальца', 'Щупальца при поглощении остаются красными, как на PS4, а не белыми. Лучше не выключать.'],
      discord: ['Статус в Discord', 'В профиле Discord видно, что вы играете в PROTOTYPE, какое сейчас задание и сколько вы уже играете.'],
      fsr1: ['Масштабирование FSR 1', 'Аккуратно растягивает картинку до размера окна. Работает, только если окно больше разрешения рендера.'],
      rcas: ['Резкость', 'Повышает чёткость готового кадра (AMD RCAS).'],
      sharp: ['Сила резкости', ''],
      aniso: ['Фильтрация текстур', 'Анизотропная фильтрация: земля и стены под острым углом остаются чёткими.'],
      display: ['Режим экрана', '«Без рамки» — на весь экран, а Alt+Tab переключает мгновенно. В окне и в полноэкранном режиме PROTOTYPE использует разрешение рендера.'],
      vsync: ['Вертикальная синхронизация', 'Убирает разрывы кадра, подстраиваясь под частоту монитора.'],
      fpsCounter: ['Счётчик кадров', 'Показывает FPS в углу экрана.'],
      prompts: ['Иконки кнопок в игре', 'Какие кнопки показывать в подсказках: ваши клавиши, PlayStation или Xbox. «Авто» — по тому, на чём вы играли в прошлый раз. Применяется при следующем запуске.'],
      mouseLook: ['Обзор мышью', 'Камера поворачивается мышью, как в обычных играх для ПК: без мёртвой зоны и ускорения. В игре переключается на F7.'],
      sens: ['Чувствительность мыши', ''],
      menuMouse: ['Мышь в меню', 'Курсор из ПК-версии во всех меню игры: можно щёлкать по пунктам, спискам и ползункам.'],
      vib: ['Вибрация', 'Сила вибрации в игре.'],
      menuVib: ['Вибрация в меню', 'Лёгкий отклик при нажатиях в меню игры или ещё и при перемещении по пунктам.'],
      actionVib: ['Вибрация от действий', 'Дополнительная вибрация порта: поглощение, хлыст, бег по стенам, приземления, взрывы и удары.'],
      lightbar: ['Световая панель', 'Подсвечивается цветами игры, как на PS4.'],
      haptics: ['HD-вибрация', 'DualSense по USB: у каждого движения своя отдача — щелчок хлыста, шаги по стене.'],
      soundHaptics: ['Звуки в вибрации', 'HD-вибрация повторяет звуки игры: взрывы, стрельбу, танки. Музыку — нет.'],
      language: ['Язык лаунчера', 'Язык самой игры зависит от ваших файлов игры.'],
      minimize: ['Сворачивать во время игры', 'Лаунчер сворачивается, пока идёт игра, и возвращается, когда вы из неё выйдете.']
    },
    opt: { asGame: 'Как в игре', windowed: 'В окне', borderless: 'Без рамки', fullscreen: 'Полный экран', auto: 'Авто',
           keyboard: 'Клавиатура', presses: 'При нажатии', all: 'Всегда', low: 'Слабо', medium: 'Средне', high: 'Сильно', offOpt: 'Выкл.' },
    tag: 'эксперимент'
  },
  en: {
    brandSub: 'BIOHAZARD BUNDLE · PC PORT', p1Sub: '2009 · ALEX MERCER', p2Sub: '2012 · JAMES HELLER',
    navHome: 'Home', navGraphics: 'Graphics', navDisplay: 'Display', navKeys: 'Keys', navPad: 'Gamepad',
    navSaves: 'Saves', navMods: 'Mods', navNews: 'News', navNewsTitle: 'What’s new', navFolder: 'Folder',
    rec: 'REC', lastSave: 'Last save', launchSettings: 'LAUNCH SETTINGS',
    imageBoth: 'Image — both games', onlyFor: '{game} only',
    gfxLedeP1: 'Each game keeps its own resolution and frame rate; the rest is shared. PROTOTYPE’s frame rate can also be changed in the game: Options › Display.',
    gfxLedeP2: 'PROTOTYPE 2 came to the port after the first game, so expect rough edges. Image settings are shared by both games.',
    displayLede: 'How the game reaches your screen and what shows on top of it.', window: 'Window', inGame: 'In game',
    inGameNote: '<b>Insert</b> on the keyboard or <b>R3 + L2</b> on a gamepad open the port menu in game: FSR 3.1, FSR 4 and the other graphics options. <b>F1</b> opens the trainer, <b>F3</b> hides the HUD, the key under <b>Esc</b> opens the Lua console.',
    keysTitle: 'Keyboard and mouse', keysLede: 'Click the box next to an action, then press the new key or mouse button. A key can be on one action only.',
    mouseIcons: 'Mouse and icons', actions: 'Actions', resetKeys: 'Reset keys',
    keysHint: 'For the left mouse button, click the box again. Delete clears it; a click anywhere else cancels.',
    pressKey: 'PRESS A KEY…', reservedKey: '{key} is one of the port’s own keys',
    padLede: 'DualSense, DualShock 4, Xbox and other gamepads. Changes apply at once; the same options are in the game’s own menu and the port menu.',
    vibration: 'Vibration',
    savesLede: 'Every slot with its mission, difficulty and time. A backup is made at every start, and any slot can be brought back from it.',
    backupNow: 'Back up now', openFolder: 'Open folder', backups: 'Backups', restore: 'Restore',
    noSavesYet: 'No saves for this game yet: they appear after your first save in game.',
    noBackups: 'No backups yet: the first is made at the next start.',
    latest: 'latest', slot: 'Slot {slot}', auto: 'autosave',
    difficulty: 'Difficulty: {d}',
    modsLede: 'Shader mods change how the game draws the picture. The game files stay untouched: mods are added at every start.',
    shaderMods: 'Shader mods', openMods: 'Open the mods folder', luaScripts: 'Lua scripts', in12: 'coming in 1.2',
    modsNote: 'A mod is a folder in <b>mods</b> with a <b>shaders</b> folder inside. <b>docs/SHADER_MODS.md</b> shows how to find and change a shader.',
    luaNote: 'Scripts will run in the game’s own Lua after a save loads, like commands in the console. For example: endless health, or slow motion on a key.',
    noMods: 'No mods yet.', looseShaders: 'Loose shaders in mods/shaders: {n}', shaderFiles: '{n} {files}',
    modOn: '“{name}” turns on at the next start', modOff: '“{name}” turns off at the next start',
    gameFolder: 'Game folder', folderLede: 'Your PROTOTYPE BIOHAZARD BUNDLE dump. The port only reads these files and never changes them.',
    change: 'Change', prepare: 'Prepare the game', preparing: 'Preparing…', restoreOriginals: 'Restore original files',
    clean: 'Clean up the folder', openLog: 'Open the log', launcher: 'Launcher', notSelected: 'No folder selected',
    log: 'LOG', portFolder: 'PORT FOLDER', hide: 'Hide',
    play: 'PLAY', stop: 'STOP', running: 'PLAYING', starting: 'STARTING {game}',
    launchNote: 'The game window opens in a few seconds', launchRunning: 'The game is running',
    prepState: 'Preparation', prepReady: 'done', prepFirst: 'at start', cache: 'Shader cache',
    playTime: 'Time played', never: 'not played yet', discordOn: 'status on', discordOff: 'off',
    hours: '{h} h {m} min', minutes: '{m} min',
    stFiles: 'Game files', stPrep: 'Preparation', stSupport: 'Support', stIcons: 'Button icons',
    filesOk: 'checked, {exe} original', filesBad: '{what}: {detail}', prepDone: 'done, from the cache',
    prepLater: 'done at the first start', supportFull: 'full', supportExp: 'experimental',
    iconsXbox: 'Xbox', iconsPS: 'PlayStation', iconsKeys: 'keyboard', iconsAuto: 'the last device used',
    padConnected: 'gamepad connected', p2IconsNote: 'PROTOTYPE 2 has PlayStation only for now',
    pfRes: 'Resolution', pfFps: 'Frame rate', pfFsr: 'FSR 1', pfSharp: 'Sharpening', pfScreen: 'Display', pfVsync: 'V-Sync',
    pfIcons: 'Icons', pfMouse: 'Mouse', on: 'on', off: 'off', look: 'look, {n}',
    p1Cam: 'CAMERA 04 · SUBJECT UNDER WATCH', p2Cam: 'CAMERA 11 · NEW YORK ZERO',
    p1Wall: 'CAMERAS · MANHATTAN', p2Wall: 'CAMERAS · NEW YORK ZERO',
    p1Foot: 'Mission pictures come from your game files.', p2Foot: 'Story pictures come from PROTOTYPE 2’s files.',
    noArt: 'The pictures appear once the game folder is set.', noSignal: 'NO SIGNAL', camera: 'CAMERA',
    zones: ['YELLOW ZONE', 'GREEN ZONE', 'RED ZONE'],
    threats: ['HIGH THREAT', 'INFECTION 34%', 'TARGET UNKNOWN', 'INFECTION 61%', 'QUARANTINE'],
    heroes: {
      p1: ['ALEX', 'MERCER', [['CODENAME', 'ZEUS'], ['FILE OPENED', '09/18/2008'], ['STATUS', 'AT LARGE'], ['ID MATCH', '99.2%']]],
      p2: ['JAMES', 'HELLER', [['RANK', 'SERGEANT'], ['FILE OPENED', '08/14/2009'], ['STATUS', 'INFECTED'], ['ID MATCH', '97.6%']]]
    },
    cells: ['slot', 'slots', 'slots'], files: ['file', 'files', 'files'],
    updLatest: 'You have the latest version', updTest: 'Test build {v}', updNewer: 'Version {v} is out',
    updLatestText: 'The latest release on GitHub is {v}.', updNewerText: 'Download it from GitHub: settings and saves stay.',
    updFail: 'Could not check for updates', updFailText: 'GitHub did not answer. The launcher tries again at its next start.',
    updChecking: 'Checking for updates…', checkUpdates: 'Check for updates', download: 'Open the page',
    yes: 'Yes', no: 'Cancel', ok: 'OK',
    restoreTitle: 'Restore your saves?', restoreText: 'Your saves go back to the backup of {when}. The current ones are backed up first, so this can be undone.',
    cleanTitle: 'Clean up the game folder?', cleanText: '{n} leftover {files} ({mb} MB) from earlier fixes and debugging will be deleted. The game files themselves stay.',
    cleanNothing: 'Nothing to clean: the folder holds only the game', restoreOrigTitle: 'Restore the original files?',
    restoreOrigText: 'The changed executables are replaced with the originals from their .orig backups.',
    n_fixFolder: 'Fix the game folder first', n_prepared: 'The game is prepared',
    n_cleaned: 'Files removed: {n}', n_restored: 'Original files restored', n_backedUp: 'Saves backed up',
    n_noSaves: 'No saves yet: nothing to back up', n_closeGameFirst: 'Close the game first',
    n_backupRestored: 'Saves restored', n_openFailed: 'That folder does not exist yet',
    exitOk: 'The game closed', exitCode: 'The game stopped (code {code}). The log shows why.',
    exit42: 'The game asked for a file that is not in the game folder',
    langAuto: 'As Windows',
    rows: {
      res: ['Render resolution', 'The resolution the game draws at; the PS4’s is 1920 × 1080. Higher is sharper but heavier; lower is scaled up by FSR 1.'],
      fpsP1: ['Frame rate', 'At 60 FPS gameplay is smoother while videos and menus keep their speed. Above 60 is experimental; the game’s menus stay at 60.'],
      fpsP2: ['Frame rate', '60 FPS lifts the game’s 30 FPS cap. If the game speeds up or a menu misbehaves, go back to 30.'],
      tendrils: ['Red tendrils', 'Consume tendrils stay red, as on the PS4, instead of white. Best left on.'],
      discord: ['Discord status', 'Your Discord profile shows that you play PROTOTYPE, the current mission and your play time.'],
      fsr1: ['FSR 1 upscaling', 'Scales the picture up to the window with clean edges. Works when the window is larger than the render resolution.'],
      rcas: ['Sharpening', 'Sharpens the final picture (AMD RCAS).'],
      sharp: ['Sharpness', ''],
      aniso: ['Texture filtering', 'Anisotropic filtering: ground and walls stay sharp at steep angles.'],
      display: ['Display mode', 'Borderless fills the screen and Alt+Tab switches instantly. Windowed and fullscreen PROTOTYPE use the render resolution.'],
      vsync: ['V-Sync', 'Waits for the display refresh: no tearing.'],
      fpsCounter: ['Frame counter', 'Shows the frame rate in a corner of the screen.'],
      prompts: ['Button icons in game', 'The buttons hints show: your keys, PlayStation or Xbox. Automatic follows what you played with last time. Applies at the next start.'],
      mouseLook: ['Mouse look', 'The mouse turns the camera as in PC games: no dead zone, no acceleration. F7 switches it in game.'],
      sens: ['Mouse sensitivity', ''],
      menuMouse: ['Mouse in menus', 'The PC version’s cursor in every game menu: click items, lists and sliders.'],
      vib: ['Vibration', 'Strength of the game’s vibration.'],
      menuVib: ['Menu vibration', 'A light tap on presses in the game’s menus, or also when moving between items.'],
      actionVib: ['Action vibration', 'The port’s own vibration: consuming, the whipfist, wall running, landings, explosions, blows.'],
      lightbar: ['Light bar', 'Shows the game’s colours, as on the PS4.'],
      haptics: ['HD haptics', 'DualSense on USB: each move has a feel of its own, a whip’s crack, steps on a wall.'],
      soundHaptics: ['Sound haptics', 'HD haptics also play the game’s sounds: explosions, gunfire, tanks. Not the music.'],
      language: ['Launcher language', 'The game’s own language comes from your game files.'],
      minimize: ['Minimise while playing', 'The launcher minimises while the game runs and comes back when you quit.']
    },
    opt: { asGame: 'Game default', windowed: 'Windowed', borderless: 'Borderless', fullscreen: 'Fullscreen', auto: 'Automatic',
           keyboard: 'Keyboard', presses: 'Presses', all: 'All', low: 'Low', medium: 'Medium', high: 'High', offOpt: 'Off' },
    tag: 'experimental'
  }
};
let L = TEXT.ru;
function t(key, vars = {}) {
  let s = key.split('.').reduce((o, k) => (o ? o[k] : undefined), L);
  if (s === undefined) s = key;
  return typeof s === 'string' ? s.replace(/\{(\w+)\}/g, (_, k) => (vars[k] ?? '')) : s;
}
function plural(n, forms) {
  if (L !== TEXT.ru) return n === 1 ? forms[0] : forms[1];
  const a = Math.abs(n) % 100, b = a % 10;
  return a > 10 && a < 20 ? forms[2] : b === 1 ? forms[0] : b >= 2 && b <= 4 ? forms[1] : forms[2];
}
const count = (n, forms) => `${n} ${plural(n, forms)}`;

// The releases, in both languages.
const RELEASES = {
  ru: [['1.1.0', '', [
    ['НОВОЕ', 'Новый лаунчер', 'Этот лаунчер: сохранения с картинками миссий, резервные копии, моды, проверка обновлений.'],
    ['НОВОЕ', 'Экран сохранений из ПК-версии', 'Ячейка выбирается на родном экране игры, как в ПК-версии: с миссией, датой, сложностью и картинкой. Есть и «Создать новую ячейку».'],
    ['НОВОЕ', 'Иконки Xbox', 'Кнопки Xbox 360 из шрифта ПК-версии прямо в текстах игры, а над людьми и предметами — цветные A, B, X и Y.'],
    ['НОВОЕ', 'Lua-консоль', 'Клавиша под Esc открывает консоль. Команды выполняются в Lua самой игры, а find ищет её функции.'],
    ['НОВОЕ', 'До 240 кадров в секунду', 'Можно выбрать 90, 120, 144, 165 или 240 FPS. Меню игры при этом работают на 60, а частоту можно менять прямо в игре.'],
    ['НОВОЕ', 'Скрытие интерфейса', 'F3 прячет HUD и возвращает его обратно.']]],
    ['1.0.5', '27 октября 2026', [
    ['НОВОЕ', 'Трейнер', 'Отдельный экран в меню паузы самой игры: F1 или удержание тачпада на DualSense.'],
    ['НОВОЕ', 'Карта рендера', 'Первый шаг к собственному рендеру: в логе видны все проходы, шейдеры и вызовы отрисовки.'],
    ['ИСПРАВЛЕНО', 'Дальность теней', 'Убрали настройку, которая почти ни на что не влияла.']]]],
  en: [['1.1.0', '', [
    ['NEW', 'A new launcher', 'This one: saves with their mission pictures, backups, mods, update checks.'],
    ['NEW', 'The PC version’s save screen', 'Slots are chosen on the game’s own screen, as on PC: mission, date, difficulty and picture, and “Create new slot”.'],
    ['NEW', 'Xbox button icons', 'The Xbox 360 buttons of the PC version’s font in the game’s texts, and A, B, X, Y in colour over people and objects.'],
    ['NEW', 'Lua console', 'The key under Esc opens a console that runs commands in the game’s own Lua; find looks up its functions.'],
    ['NEW', 'Up to 240 FPS', '90, 120, 144, 165 or 240 FPS. The game’s menus stay at 60, and the rate can be changed in game.'],
    ['NEW', 'Hide the HUD', 'F3 hides the HUD and brings it back.']]],
    ['1.0.5', 'October 27, 2026', [
    ['NEW', 'Trainer', 'A screen in the game’s own pause menu: F1, or hold the DualSense touchpad.'],
    ['NEW', 'Render map', 'The first step towards a native renderer: passes, shaders and draws in the log.'],
    ['FIXED', 'Shadow distance', 'The setting that barely did anything is gone.']]]]
};

// ---------------------------------------------------------------- api
let S = null;          // the launcher's state
let page = 'home';
let keyGroup = 'move';
async function api(name, args = {}) {
  const r = await fetch('/api/' + name, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(args) });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.status);
  return j;
}
async function act(name, args) {
  try {
    const s = await api(name, args);
    if (s && s.version) { S = s; render(); }
    if (s && s.notice) notice(s.notice);
    return s;
  } catch (e) { toast(String(e.message || e), 'bad'); return null; }
}

// ---------------------------------------------------------------- small parts
let toastT = null;
function toast(msg, kind = '') {
  const el = $('#toast'); el.textContent = msg; el.className = 'toast on ' + kind;
  clearTimeout(toastT); toastT = setTimeout(() => el.classList.remove('on'), 3600);
}
function notice(n) {
  if (!n) return;
  if (n.text) toast(n.text, n.kind);
  else toast(t('n_' + n.key, n), n.kind);
}
function ask(title, text, list, yes) {
  return new Promise(resolve => {
    $('#modal-title').textContent = title; $('#modal-text').textContent = text;
    const pre = $('#modal-list'); pre.hidden = !list; pre.textContent = list || '';
    $('#modal-yes').textContent = yes || t('yes'); $('#modal-no').textContent = t('no');
    const m = $('#modal'); m.hidden = false; $('#modal-no').focus();
    const done = v => { m.hidden = true; $('#modal-yes').onclick = $('#modal-no').onclick = null; resolve(v); };
    $('#modal-yes').onclick = () => done(true); $('#modal-no').onclick = () => done(false);
  });
}
function fmtDate(sec) {
  const d = new Date(sec * 1000), loc = L === TEXT.ru ? 'ru-RU' : 'en-GB';
  return d.toLocaleDateString(loc, { day: 'numeric', month: 'long' }) + ', ' + d.toLocaleTimeString(loc, { hour: '2-digit', minute: '2-digit' });
}
function fmtTime(sec) {
  const m = Math.floor(sec / 60), h = Math.floor(m / 60);
  return h ? t('hours', { h, m: m % 60 }) : t('minutes', { m });
}
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
function hashOf(s) { let h = 0; for (const c of s) h = (h * 31 + c.charCodeAt(0)) | 0; return Math.abs(h); }

// A settings row: options (stepper), range (slider) or a switch.
function row(container, spec) {
  const r = el('div', 'row');
  const lbl = el('div', 'lbl', spec.label);
  if (spec.tag) lbl.append(el('em', '', spec.tag));
  r.append(lbl);
  const value = spec.get();
  let control;
  if (spec.options) {
    control = el('div', 'stepper');
    const prev = el('button', '', '◀'), out = el('output'), next = el('button', '', '▶');
    prev.type = next.type = 'button';
    prev.setAttribute('aria-label', spec.label + ' −'); next.setAttribute('aria-label', spec.label + ' +');
    const i = Math.max(0, spec.options.findIndex(o => o[0] === value));
    out.textContent = spec.options[i][1];
    const step = d => spec.set(spec.options[(i + d + spec.options.length) % spec.options.length][0]);
    prev.onclick = () => step(-1); next.onclick = () => step(1);
    control.append(prev, out, next);
  } else if (spec.range) {
    control = el('div', 'slider');
    const input = el('input'); input.type = 'range'; [input.min, input.max] = spec.range; input.value = value;
    input.id = 'r-' + spec.id; input.setAttribute('aria-label', spec.label);
    const out = el('output', '', value);
    input.oninput = () => { out.textContent = input.value; };
    input.onchange = () => spec.set(+input.value);
    control.append(input, out);
  } else {
    control = el('button', 'toggle'); control.type = 'button'; control.id = 't-' + spec.id;
    control.setAttribute('role', 'switch'); control.setAttribute('aria-label', spec.label);
    control.setAttribute('aria-checked', String(!!value));
    control.onclick = () => spec.set(!value);
  }
  r.append(control);
  if (spec.desc) r.append(el('div', 'desc', spec.desc));
  if (spec.locked) r.classList.add('locked');
  container.append(r);
}
const V = key => S.values[key];
const setV = key => value => act('set', { key, value });
const G = name => S.perGame[S.game][name];
const setG = name => value => act('set', { key: 'game.' + name, value });
function setting(container, rowKey, extra) {
  const [label, desc] = t('rows.' + rowKey);
  row(container, Object.assign({ id: rowKey, label, desc }, extra));
}

// ---------------------------------------------------------------- rendering
function render() {
  if (!S) return;
  const lang = S.lang === 'ru' ? 'ru' : 'en';
  L = TEXT[lang]; document.documentElement.lang = lang;
  document.documentElement.dataset.game = S.game;
  $$('[data-t]').forEach(e => { e.textContent = t(e.dataset.t); });
  $$('[data-th]').forEach(e => { e.innerHTML = t(e.dataset.th); });
  $$('.switch button').forEach(b => { b.setAttribute('aria-pressed', String(b.dataset.game === S.game)); b.disabled = S.launch.running; });
  $('#ver').innerHTML = '';
  $('#ver').append('v', el('b', '', S.version), S.beta ? ` · b${S.beta}` : '');
  const pad = $('#pad');
  pad.hidden = !S.pad;
  if (S.pad) { pad.innerHTML = ''; pad.append((L === TEXT.ru ? 'ГЕЙМПАД ' : 'GAMEPAD '), el('b', '', S.pad.kind === 'xbox' ? 'XBOX' : (S.pad.type || 'PLAYSTATION').toUpperCase().replace('PS5', 'DUALSENSE').replace('PS4', 'DUALSHOCK 4'))); }
  const art = $('#art'), want = S.game === 'p2' ? 'heller' : 'alex';
  const url = S.art && S.art.art.includes(want) ? `url("/art/${want}.jpg")` : 'none';
  if (art.dataset.url !== url) {
    art.classList.add('swap');
    setTimeout(() => { art.style.backgroundImage = url; art.classList.remove('swap'); }, art.dataset.url ? 300 : 0);
    art.dataset.url = url;
  }
  renderHome(); renderSettings(); renderKeys(); renderSaves(); renderMods(); renderNews(); renderFolder(); renderDock();
  $('#news-badge').hidden = !(S.update && S.update.newer);
}

function heroText() {
  const [first, last, file] = t('heroes')[S.game];
  return { first, last, file };
}
let lastGame = null;
function renderHome() {
  const h = heroText(), name = $('#h-name');
  name.innerHTML = ''; name.append(h.first, el('br'), el('span', 'r', h.last));
  if (lastGame && lastGame !== S.game && !reduce) { name.classList.remove('glitch'); void name.offsetWidth; name.classList.add('glitch'); }
  $('#h-cam').textContent = t(S.game + 'Cam');
  const f = $('#h-file'); f.innerHTML = '';
  h.file.forEach(([k, v]) => { const s = el('span', '', k); s.append(el('b', '', v)); f.append(s); });
  const exe = S.game === 'p1' ? 'eboot.bin' : 'prototype2_ps4f.elf';
  const bad = S.checks.find(c => c[1] === 'bad');
  const prompts = V('prompts');
  const icons = S.game === 'p2' ? [t('iconsPS'), t('p2IconsNote')] :
    prompts === 'xbox' ? [t('iconsXbox'), ''] : prompts === 'gamepad' ? [t('iconsPS'), ''] : prompts === 'keyboard' ? [t('iconsKeys'), ''] :
    S.pad ? [S.pad.kind === 'xbox' ? t('iconsXbox') : t('iconsPS'), t('padConnected')] : [t('iconsAuto'), ''];
  const st = [
    [bad ? 'bad' : 'ok', t('stFiles'), !S.gameDir ? t('notSelected') : bad ? t('filesBad', { what: bad[0], detail: bad[2] }) : t('filesOk', { exe })],
    ['ok', t('stPrep'), S.prepared[S.game] ? t('prepDone') : t('prepLater')],
    [S.game === 'p1' ? 'ok' : 'warn', t('stSupport'), S.game === 'p1' ? t('supportFull') : t('supportExp')],
    ['ok', t('stIcons'), icons.filter(Boolean).join(' — ')]
  ];
  const s = $('#h-status'); s.innerHTML = '';
  st.forEach(([c, a, b]) => { const d = el('div'); d.append(el('i', c), el('b', '', a), el('span', '', b)); s.append(d); });
  const saves = S.saves[S.game];
  const cont = $('#h-continue');
  cont.hidden = !saves.length;
  if (saves.length) {
    const latest = saves.reduce((a, b) => (b.time > a.time ? b : a));
    setShot($('#h-shot'), latest);
    $('#h-save').textContent = latest.title + (latest.number ? ` (${latest.number})` : '');
    $('#h-savemeta').textContent = `${t('slot', { slot: latest.slot })} · ${fmtDate(latest.time)}`;
  }
  const pg = S.perGame[S.game];
  const fps = S.game === 'p1' ? pg.fps : (pg.fps60 ? 60 : 30);
  const prof = [
    [t('pfRes'), pg.render_res.replace('x', ' × '), 'graphics'], [t('pfFps'), fps + ' FPS', 'graphics'],
    [t('pfFsr'), V('fsr1') ? t('on') : t('off'), 'graphics'], [t('pfSharp'), V('rcas') ? V('rcas_strength') + '%' : t('off'), 'graphics'],
    [t('pfScreen'), t('opt.' + V('display_mode')).toLowerCase(), 'display'], [t('pfVsync'), V('vsync') ? t('on') : t('off'), 'display'],
    [t('pfIcons'), { auto: t('opt.auto').toLowerCase(), keyboard: t('opt.keyboard').toLowerCase(), gamepad: 'PlayStation', xbox: 'Xbox' }[prompts] || prompts, 'controls'],
    [t('pfMouse'), V('mouse_look') ? t('look', { n: V('mouse_sensitivity') }) : t('off'), 'controls']
  ];
  const p = $('#h-profile'); p.innerHTML = '';
  prof.forEach(([k, v, target], i) => {
    const b = el('button'); b.type = 'button'; b.id = 'pf-' + i;
    b.append(el('small', '', k), el('span', '', v)); b.onclick = () => go(target); p.append(b);
  });
  $('#wall-title').textContent = t(S.game + 'Wall');
  $('#wall-foot').textContent = S.art ? (wallIds().length ? t(S.game + 'Foot') : t('noArt')) : '';
  if (lastGame !== S.game || !$('#monitors').children.length || wallStale) buildWall();
  lastGame = S.game;
}

function setShot(node, save) {
  const list = save && S.art ? (S.game === 'p1' ? S.art.p1 : S.art.p2) : [];
  let id = null;
  if (S.game === 'p1') id = save.mission && list.includes(save.mission) ? save.mission : null;
  else if (list.length) id = list[hashOf(save.title + save.slot) % list.length];
  node.classList.toggle('blank', !id);
  node.style.backgroundImage = id ? `url("/art/${S.game}/${id}.jpg")` : '';
}

// The surveillance wall.
let feedIdx = 0, wallStale = false;
function wallIds() {
  if (!S.art) return [];
  return (S.game === 'p1' ? S.art.p1 : S.art.p2).slice();
}
function buildWall() {
  wallStale = !S.art;
  const w = $('#monitors'); w.innerHTML = ''; feedIdx = 0;
  const ids = wallIds();
  for (let i = 0; i < 5; i++) {
    const m = el('div', 'mon' + (i === 0 ? ' big' : ''));
    if (!ids.length) {
      m.classList.add('nosignal'); m.append(el('span', '', S.art ? t('noSignal') : '…'));
    } else {
      m.innerHTML = '<img alt=""><div class="hud"><div class="hr"><span class="cam"></span><span class="threat"></span></div><div class="hr"><span class="loc"></span><span class="tc"></span></div></div>';
      setFeed(m, i);
    }
    w.append(m);
  }
}
function setFeed(m, i) {
  const ids = wallIds(); if (!ids.length) return;
  const id = ids[feedIdx++ % ids.length];
  const img = m.querySelector('img'); img.src = `/art/${S.game}/${id}.jpg`;
  const cam = `${t('camera')} ${String(3 + ((feedIdx * 7) % 40)).padStart(2, '0')}`;
  if (S.game === 'p1') {
    const districts = (S.art.districts || []).map(d => d.toUpperCase());
    const mission = (S.art.missions || {})[id] || '';
    img.alt = mission;
    m.querySelector('.cam').textContent = districts.length ? `${cam} · ${districts[(feedIdx * 3 + i) % districts.length]}` : cam;
    m.querySelector('.loc').textContent = mission.toUpperCase();
  } else {
    img.alt = '';
    m.querySelector('.cam').textContent = cam;
    m.querySelector('.loc').textContent = t('zones')[(feedIdx + i) % 3];
  }
  const threats = t('threats');
  m.querySelector('.threat').textContent = threats[(feedIdx + i) % threats.length];
}
function cycleFeed() {
  const mons = $$('.mon').filter(m => !m.classList.contains('nosignal'));
  if (!mons.length || document.hidden) return;
  const m = mons[Math.floor(Math.random() * mons.length)];
  m.classList.remove('cut'); void m.offsetWidth; m.classList.add('cut');
  setFeed(m, mons.indexOf(m));
}

function renderSettings() {
  const g = $('#gfx-game'); g.innerHTML = '';
  $('#gfx-game-head').textContent = t('onlyFor', { game: S.game === 'p1' ? 'PROTOTYPE' : 'PROTOTYPE 2' });
  $('#gfx-lede').textContent = t(S.game === 'p1' ? 'gfxLedeP1' : 'gfxLedeP2');
  const res = ['1280x720', '1600x900', '1920x1080', '2560x1440', '3200x1800', '3840x2160'].map(r => [r, r.replace('x', ' × ')]);
  setting(g, 'res', { tag: t('tag'), options: res, get: () => G('render_res'), set: setG('render_res') });
  if (S.game === 'p1') {
    setting(g, 'fpsP1', { options: [30, 60, 90, 120, 144, 165, 240].map(f => [f, f + ' FPS']), get: () => G('fps'), set: setG('fps') });
    setting(g, 'tendrils', { get: () => G('tendril_fix'), set: setG('tendril_fix') });
    setting(g, 'discord', { get: () => V('discord'), set: setV('discord') });
  } else {
    setting(g, 'fpsP2', { tag: t('tag'), options: [[false, '30 FPS'], [true, '60 FPS']], get: () => !!G('fps60'), set: setG('fps60') });
  }
  const im = $('#gfx-image'); im.innerHTML = '';
  setting(im, 'fsr1', { get: () => V('fsr1'), set: setV('fsr1') });
  setting(im, 'rcas', { get: () => V('rcas'), set: setV('rcas') });
  setting(im, 'sharp', { range: [0, 100], get: () => V('rcas_strength'), set: setV('rcas_strength'), locked: !V('rcas') });
  setting(im, 'aniso', { options: [[0, t('opt.asGame')], [2, '2×'], [4, '4×'], [8, '8×'], [16, '16×']], get: () => V('anisotropy'), set: setV('anisotropy') });

  const w = $('#dsp-window'); w.innerHTML = '';
  setting(w, 'display', { options: ['windowed', 'borderless', 'fullscreen'].map(m => [m, t('opt.' + m)]), get: () => V('display_mode'), set: setV('display_mode') });
  setting(w, 'vsync', { get: () => V('vsync'), set: setV('vsync') });
  const dg = $('#dsp-game'); dg.innerHTML = '';
  setting(dg, 'fpsCounter', { get: () => V('show_fps'), set: setV('show_fps') });

  const m = $('#ctl-mouse'); m.innerHTML = '';
  setting(m, 'prompts', { options: [['auto', t('opt.auto')], ['keyboard', t('opt.keyboard')], ['gamepad', 'PlayStation'], ['xbox', 'Xbox']], get: () => V('prompts'), set: setV('prompts') });
  setting(m, 'mouseLook', { get: () => V('mouse_look'), set: setV('mouse_look') });
  setting(m, 'sens', { range: [10, 400], get: () => V('mouse_sensitivity'), set: setV('mouse_sensitivity'), locked: !V('mouse_look') });
  setting(m, 'menuMouse', { get: () => V('menu_mouse'), set: setV('menu_mouse') });

  const r = $('#pad-rumble'); r.innerHTML = '';
  setting(r, 'vib', { options: [0, 25, 50, 75, 100, 150, 200].map(v => [v, v ? v + '%' : t('opt.offOpt')]), get: () => V('vibration'), set: setV('vibration') });
  setting(r, 'menuVib', { options: [[0, t('opt.offOpt')], [1, t('opt.presses')], [2, t('opt.all')]], get: () => V('menu_vibration'), set: setV('menu_vibration') });
  setting(r, 'actionVib', { get: () => V('extra_vibration'), set: setV('extra_vibration') });
  const ds = $('#pad-ds'); ds.innerHTML = '';
  setting(ds, 'lightbar', { get: () => V('light_bar'), set: setV('light_bar') });
  setting(ds, 'haptics', { get: () => V('haptics'), set: setV('haptics') });
  setting(ds, 'soundHaptics', { options: [[0, t('opt.offOpt')], [1, t('opt.low')], [2, t('opt.medium')], [3, t('opt.high')]], get: () => V('haptics_sound'), set: setV('haptics_sound'), locked: !V('haptics') });

  const l = $('#fd-launcher'); l.innerHTML = '';
  setting(l, 'language', { options: [['auto', t('langAuto')], ['en', 'English'], ['ru', 'Русский']], get: () => V('ui_language') || 'auto', set: setV('ui_language') });
  setting(l, 'minimize', { get: () => V('minimize_on_play') !== false, set: setV('minimize_on_play') });
}

// ---------------------------------------------------------------- keys
const PAD = {
  ps: { cross: '✕', circle: '○', square: '□', triangle: '△', l1: 'L1', r1: 'R1', l2: 'L2', r2: 'R2', l3: 'L3', r3: 'R3', up: '↑', down: '↓', left: '←', right: '→', touchpad: 'TP', options: 'OPT' },
  xbox: { cross: 'A', circle: 'B', square: 'X', triangle: 'Y', l1: 'LB', r1: 'RB', l2: 'LT', r2: 'RT', l3: 'LS', r3: 'RS', up: '↑', down: '↓', left: '←', right: '→', touchpad: '⧉', options: '☰' }
};
const KEY_RU = { 'Space': 'Пробел', 'Mouse Left': 'ЛКМ', 'Mouse Right': 'ПКМ', 'Mouse Middle': 'СКМ', 'Mouse X1': 'Мышь 4', 'Mouse X2': 'Мышь 5', 'Wheel Up': 'Колесо ↑', 'Wheel Down': 'Колесо ↓' };
const KEY_EN = { 'Mouse Left': 'Left click', 'Mouse Right': 'Right click', 'Mouse Middle': 'Middle click', 'Mouse X1': 'Mouse 4', 'Mouse X2': 'Mouse 5', 'Wheel Up': 'Wheel ↑', 'Wheel Down': 'Wheel ↓' };
const KEY_SHORT = { 'Return': 'Enter', 'Escape': 'Esc', 'Left Shift': 'L-Shift', 'Right Shift': 'R-Shift', 'Left Ctrl': 'L-Ctrl', 'Right Ctrl': 'R-Ctrl', 'Left Alt': 'L-Alt', 'Right Alt': 'R-Alt' };
const keyText = k => (L === TEXT.ru ? KEY_RU : KEY_EN)[k] || KEY_SHORT[k] || k;
const RESERVED = new Set(['Insert', 'F7', 'F9', 'F10', ',', '=', '+']);
const CODES = {
  Space: 'Space', Tab: 'Tab', Enter: 'Return', NumpadEnter: 'Return', Escape: 'Escape', Backspace: 'Backspace', CapsLock: 'CapsLock',
  ShiftLeft: 'Left Shift', ShiftRight: 'Right Shift', ControlLeft: 'Left Ctrl', ControlRight: 'Right Ctrl', AltLeft: 'Left Alt', AltRight: 'Right Alt',
  ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right', PageUp: 'PageUp', PageDown: 'PageDown', Home: 'Home', End: 'End',
  Insert: 'Insert', Semicolon: ';', Minus: '-', Period: '.', Slash: '/', Backquote: '`', BracketLeft: '[', Backslash: '\\', BracketRight: ']',
  Quote: "'", Comma: ',', Equal: '='
};
function sdlKey(code) {
  if (CODES[code]) return CODES[code];
  let m;
  if ((m = code.match(/^Key([A-Z])$/))) return m[1];
  if ((m = code.match(/^Digit(\d)$/))) return m[1];
  if ((m = code.match(/^Numpad(\d)$/))) return 'Keypad ' + m[1];
  if ((m = code.match(/^F(\d{1,2})$/))) return code;
  return null;
}
let capture = null;  // {action, slot, node}
function renderKeys() {
  const c = S.controls;
  if (!c.groups.some(g => g[0] === keyGroup)) keyGroup = c.groups[0][0];
  const tabs = $('#key-groups'); tabs.innerHTML = '';
  c.groups.forEach(([id, en, ru]) => {
    const b = el('button', '', L === TEXT.ru ? ru : en); b.type = 'button';
    b.setAttribute('aria-pressed', String(id === keyGroup));
    b.onclick = () => { keyGroup = id; stopCapture(); renderKeys(); };
    tabs.append(b);
  });
  const style = (V('prompts') === 'xbox' || (V('prompts') === 'auto' && S.pad && S.pad.kind === 'xbox')) ? PAD.xbox : PAD.ps;
  const list = $('#ctl-keys'); list.innerHTML = '';
  const clashes = new Set(c.clashes.map(k => k.toLowerCase()));
  c.actions.filter(a => a.group === keyGroup).forEach(a => {
    const r = el('div', 'keyrow');
    const padName = a.pad.startsWith('lstick') ? 'L' + { up: '↑', down: '↓', left: '←', right: '→' }[a.pad.split('_')[1]] : (style[a.pad] || a.pad);
    r.append(el('span', 'pad', padName), el('span', '', L === TEXT.ru ? a.ru : a.en));
    for (let slot = 0; slot < c.slots; slot++) {
      const key = a.keys[slot] || '';
      const k = el('kbd', key ? '' : 'empty', key ? keyText(key) : '—');
      if (key && clashes.has(key.toLowerCase())) k.classList.add('clash');
      k.tabIndex = 0; k.setAttribute('role', 'button'); k.id = `kb-${a.id}-${slot}`;
      k.onmousedown = e => onKeyMouse(e, a.id, slot, k);
      k.onkeydown = e => { if (!capture && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); startCapture(a.id, slot, k); } };
      r.append(k);
    }
    list.append(r);
  });
}
function startCapture(action, slot, node) {
  stopCapture();
  capture = { action, slot, node, armed: Date.now() };
  node.classList.add('wait'); node.textContent = t('pressKey');
}
function stopCapture() {
  if (!capture) return;
  capture = null; renderKeys();
}
function setKey(key) {
  const c = capture; capture = null;
  if (key && RESERVED.has(key)) { toast(t('reservedKey', { key }), 'bad'); renderKeys(); return; }
  act('key', { action: c.action, slot: c.slot, key });
}
function onKeyMouse(e, action, slot, node) {
  e.preventDefault();
  if (capture && capture.node === node) {
    const names = ['Mouse Left', 'Mouse Middle', 'Mouse Right', 'Mouse X1', 'Mouse X2'];
    if (e.button === 0 && Date.now() - capture.armed < 250) return;
    setKey(names[e.button] || null);
  } else startCapture(action, slot, node);
}
addEventListener('keydown', e => {
  if (!capture) return;
  e.preventDefault(); e.stopPropagation();
  if (e.code === 'Delete') { setKey(''); return; }
  const key = sdlKey(e.code);
  if (key) setKey(key);
}, true);
addEventListener('wheel', e => { if (capture) { e.preventDefault(); setKey(e.deltaY < 0 ? 'Wheel Up' : 'Wheel Down'); } }, { passive: false, capture: true });
addEventListener('mousedown', e => { if (capture && !e.target.closest('.keyrow kbd')) stopCapture(); }, true);
addEventListener('contextmenu', e => { if (capture || e.target.closest('.keyrow kbd')) e.preventDefault(); });
addEventListener('auxclick', e => { if (capture) e.preventDefault(); });

// ---------------------------------------------------------------- saves, mods, news, folder
function renderSaves() {
  const list = S.saves[S.game];
  $('#sv-count').textContent = `${count(list.length, t('cells'))} · ${S.game === 'p1' ? 'PROTOTYPE' : 'PROTOTYPE 2'}`;
  const grid = $('#sv-grid'); grid.innerHTML = '';
  if (!list.length) grid.append(el('div', 'empty-state', t('noSavesYet')));
  const latest = list.length ? list.reduce((a, b) => (b.time > a.time ? b : a)) : null;
  [...list].sort((a, b) => b.time - a.time).forEach(s => {
    const b = el('div', 'save');
    const shot = el('div', 'shot'); setShot(shot, s);
    b.append(shot, el('span', 'slot', s.number ? String(s.number).padStart(2, '0') : s.slot));
    const info = el('div', 'info');
    info.append(el('strong', '', s.title));
    const parts = s.detail.split(/\s*\n\s*/).filter(Boolean).map(x => x.toLowerCase());
    if (S.game === 'p1' && parts.length === 1 && !/[:%]/.test(parts[0])) parts[0] = t('difficulty', { d: parts[0] }).toLowerCase();
    if (s.auto) parts.unshift(t('auto'));
    let detail = parts.join(' · ');
    detail = detail.charAt(0).toUpperCase() + detail.slice(1);
    info.append(el('span', 'd', detail));
    const meta = el('div', 'meta');
    meta.append(el('span', '', `${t('slot', { slot: s.slot })} · ${fmtDate(s.time)}`), el('span', 'latest', s === latest ? t('latest') : ''));
    info.append(meta); b.append(info);
    grid.append(b);
  });
  const bk = $('#sv-backups'); bk.innerHTML = '';
  if (!S.backups.length) bk.append(el('div', 'empty-state', t('noBackups')));
  S.backups.forEach(x => {
    const r = el('div', 'backup');
    r.append(el('time', '', fmtDate(x.time)), el('span', '', count(x.slots, t('cells'))));
    const b = el('button', 'btn', t('restore')); b.type = 'button';
    b.onclick = async () => { if (await ask(t('restoreTitle'), t('restoreText', { when: fmtDate(x.time) }), '', t('restore'))) act('restore_backup', { name: x.name }); };
    r.append(b); bk.append(r);
  });
}

function renderMods() {
  const m = $('#mod-list'); m.innerHTML = '';
  const mods = S.mods;
  if (!mods.named.length && !mods.loose) m.append(el('div', 'empty-state', t('noMods')));
  mods.named.forEach(x => {
    const r = el('div', 'mod');
    const t1 = el('div', 't');
    t1.append(el('strong', '', x.name), el('small', '', `mods/${x.name} · ${count(x.files, t('files'))}`));
    const tog = el('button', 'toggle'); tog.type = 'button'; tog.setAttribute('role', 'switch'); tog.setAttribute('aria-label', x.name);
    tog.setAttribute('aria-checked', String(x.enabled));
    tog.onclick = async () => { await act('mod', { name: x.name, enabled: !x.enabled }); toast(t(x.enabled ? 'modOff' : 'modOn', { name: x.name })); };
    r.append(el('div', 'ic', 'GLSL'), t1, tog); m.append(r);
  });
  if (mods.loose) m.append(el('div', 'hint', t('looseShaders', { n: mods.loose })));
}

function renderNews() {
  const u = $('#update'); u.innerHTML = '';
  const up = S.update;
  const dot = el('div', 'dot'); const txt = el('div', 'txt'); let btn;
  if (!up) { txt.append(el('b', '', t('updChecking'))); }
  else if (!up.ok) { txt.append(el('b', '', t('updFail')), el('span', '', t('updFailText'))); btn = [t('checkUpdates'), () => checkUpdate()]; }
  else if (up.newer) { dot.style.background = 'var(--accent-hi)'; txt.append(el('b', '', t('updNewer', { v: up.latest })), el('span', '', t('updNewerText'))); btn = [t('download'), () => act('url', { url: up.url })]; }
  else {
    dot.style.background = S.beta ? 'var(--warn)' : 'var(--ok)';
    txt.append(el('b', '', S.beta ? t('updTest', { v: `${S.version} · b${S.beta}` }) : t('updLatest')), el('span', '', t('updLatestText', { v: up.latest || '—' })));
    btn = [t('checkUpdates'), () => checkUpdate()];
  }
  u.append(dot, txt);
  if (btn) { const b = el('button', 'btn', btn[0]); b.type = 'button'; b.onclick = btn[1]; u.append(b); }
  const r = $('#releases'); r.innerHTML = '';
  RELEASES[L === TEXT.ru ? 'ru' : 'en'].forEach(([v, when, items]) => {
    const s = el('div', 'release');
    const head = el('div'); head.append(el('h4', '', v), el('div', 'when', when || (S.beta ? `b${S.beta}` : '')));
    const ul = el('ul');
    items.forEach(([k, title, p]) => { const li = el('li'); const d = el('div'); d.append(el('b', '', title), el('p', '', p)); li.append(el('span', 'k', k), d); ul.append(li); });
    s.append(head, ul); r.append(s);
  });
}
async function checkUpdate() { S.update = null; renderNews(); const r = await api('check_update').catch(() => null); if (r) { S.update = r.update; renderNews(); $('#news-badge').hidden = !(S.update && S.update.newer); } }

function renderFolder() {
  $('#fd-path').textContent = S.gameDir || t('notSelected');
  const c = $('#fd-checks'); c.innerHTML = '';
  S.checks.forEach(([label, state, detail]) => {
    const r = el('div', 'check');
    r.append(el('i', state), el('b', '', label), el('span', '', detail));
    c.append(r);
  });
  const prep = $('#fd-prepare');
  prep.textContent = S.launch.busy === 'prepare' ? t('preparing') : t('prepare');
  prep.disabled = !S.ready || !!S.launch.busy;
  $('#fd-restore').hidden = !S.restorable;
}

// ---------------------------------------------------------------- dock and the start
function renderDock() {
  const running = S.launch.running;
  const play = $('#play');
  play.classList.toggle('running', running);
  play.disabled = !running && (!S.ready || !!S.launch.busy);
  $('#play-label').textContent = running ? t('stop') : t('play');
  $('#play-sub').textContent = running ? `${t('running')} · ${fmtTime(Date.now() / 1000 - S.launch.since)}` : `${S.game === 'p1' ? 'PROTOTYPE' : 'PROTOTYPE 2'} · ENTER`;
  const d = $('#dock-info'); d.innerHTML = '';
  const played = (S.playTime || {})[S.game] || 0;
  [[t('prepState'), S.prepared[S.game] ? t('prepReady') : t('prepFirst'), S.prepared[S.game]],
   [t('cache'), `${S.cache.mb} ${L === TEXT.ru ? 'МБ' : 'MB'} · ${S.cache.files}`],
   [t('playTime'), played ? fmtTime(played) : t('never')],
   ['Discord', V('discord') ? t('discordOn') : t('discordOff')]].forEach(([k, v, ok]) => {
    const x = el('div'); x.append(el('small', '', k), el('span', ok ? 'ok' : '', v)); d.append(x);
  });
}
let bootShown = false, lastExit = null;
function renderBoot() {
  const l = S.launch, boot = $('#boot');
  if (l.running && bootShown) {
    $('#boot-title').textContent = t('starting', { game: l.game === 'p2' ? 'PROTOTYPE 2' : 'PROTOTYPE' });
    const secs = Math.max(0, Math.floor(Date.now() / 1000 - l.since));
    $('#boot-time').textContent = `${secs} s`;
    $('#boot-bar').style.width = Math.min(100, secs / 12 * 100) + '%';
    const log = $('#boot-log'); log.innerHTML = '';
    l.log.forEach(line => { const s = el('span', /error|failed|fatal/i.test(line) ? '' : /ready|ok\b|up to date|backed up/i.test(line) ? 'ok' : '', line + '\n'); log.append(s); });
    $('#boot-note').textContent = secs < 12 ? t('launchNote') : t('launchRunning');
    boot.classList.add('on');
  } else boot.classList.remove('on');
  $('.app').classList.toggle('launching', l.running && bootShown);
  $('#dock').classList.toggle('armed', l.running);
  if (l.exit && JSON.stringify(l.exit) !== lastExit) {
    if (lastExit !== null || bootShown) {
      const code = l.exit.code;
      toast(code === 0 ? t('exitOk') : code === 42 ? t('exit42') : t('exitCode', { code }), code === 0 ? '' : 'bad');
    }
    lastExit = JSON.stringify(l.exit);
    bootShown = false;
  }
}
async function play() {
  if (S.launch.running) { act('stop'); return; }
  bootShown = true;
  const s = await act('play');
  if (s) renderBoot();
}

// ---------------------------------------------------------------- pages
const PAGES = ['home', 'graphics', 'display', 'controls', 'gamepad', 'saves', 'mods', 'news', 'folder'];
function go(p) {
  if (!PAGES.includes(p)) p = 'home';
  page = p; stopCapture();
  $$('.page').forEach(x => x.classList.toggle('on', x.id === 'page-' + p));
  $$('.nav').forEach(n => (n.dataset.page === p ? n.setAttribute('aria-current', 'page') : n.removeAttribute('aria-current')));
  $('#main').scrollTop = 0;
  try { localStorage.setItem('page', p); } catch (e) { /* the page still works */ }
}

// ---------------------------------------------------------------- ambient
function noise() {
  const c = $('#noise'), x = c.getContext('2d'); const W = 220, H = 140; c.width = W; c.height = H;
  const img = x.createImageData(W, H);
  const draw = () => { for (let i = 0; i < img.data.length; i += 4) { const v = Math.random() * 255; img.data[i] = img.data[i + 1] = img.data[i + 2] = v; img.data[i + 3] = 255; } x.putImageData(img, 0, 0); };
  draw(); if (!reduce) setInterval(draw, 120);
}
function clock() {
  const now = new Date();
  $('#clock').textContent = now.toLocaleTimeString(L === TEXT.ru ? 'ru-RU' : 'en-GB');
  $$('.mon .tc').forEach((e, i) => { e.textContent = `${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}:${String((now.getMilliseconds() / 40 | 0) + i).padStart(2, '0')}`; });
  if (S && S.launch.running) renderDock();
}

// ---------------------------------------------------------------- start
async function poll() {
  try {
    const p = await api('poll');
    const wasRunning = S.launch.running, hadArt = !!S.art;
    S.launch = p.launch; S.pad = p.pad; if (p.update) S.update = p.update;
    if (p.notice) notice(p.notice);
    if ((p.artReady && !hadArt) || (wasRunning && !p.launch.running) || (S.launch.busy === null && $('#fd-prepare').textContent === t('preparing'))) {
      S = await api('state'); wallStale = true; render();
    } else { renderDock(); renderFolder(); }
    renderBoot();
  } catch (e) { /* the launcher process is gone: the window closes with it */ }
  setTimeout(poll, S && S.launch.running && bootShown ? 600 : 1500);
}
async function init() {
  S = await api('state');
  let saved = 'home';
  try { saved = localStorage.getItem('page') || 'home'; } catch (e) { /* default page */ }
  render(); go(saved);
  if (S.launch.exit) lastExit = JSON.stringify(S.launch.exit);
  $$('.nav').forEach(n => { n.onclick = () => go(n.dataset.page); });
  $$('.switch button').forEach(b => { b.onclick = () => act('game', { game: b.dataset.game }); });
  $('#play').onclick = play;
  $('#play').onmouseenter = () => $('#dock').classList.add('armed');
  $('#play').onmouseleave = () => { if (!S.launch.running) $('#dock').classList.remove('armed'); };
  $('#h-continue').onclick = () => { if (!S.launch.running) play(); };
  $('#boot-hide').onclick = () => { bootShown = false; renderBoot(); };
  $('#keys-reset').onclick = () => act('reset_keys');
  $('#sv-backup').onclick = () => act('backup');
  $('#sv-folder').onclick = () => api('open', { what: 'saves' }).then(r => { if (!r.ok) toast(t('n_openFailed')); });
  $('#mods-open').onclick = () => api('open', { what: 'mods' });
  $('#l-log').onclick = $('#fd-log').onclick = () => api('open', { what: 'log' }).then(r => { if (!r.ok) toast(t('n_openFailed')); });
  $('#l-port').onclick = () => api('open', { what: 'port' });
  $('#fd-change').onclick = () => act('choose_folder');
  $('#fd-prepare').onclick = () => act('prepare');
  $('#fd-restore').onclick = async () => { if (await ask(t('restoreOrigTitle'), t('restoreOrigText'))) act('restore_originals'); };
  $('#fd-clean').onclick = async () => {
    const p = await api('clean_preview');
    if (!p.files.length) { toast(t('cleanNothing')); return; }
    if (await ask(t('cleanTitle'), t('cleanText', { n: p.files.length, files: plural(p.files.length, t('files')), mb: p.mb }), p.files.join('\n'))) act('clean');
  };
  addEventListener('keydown', e => {
    if (capture || !$('#modal').hidden || e.target.closest('input')) return;
    if (/^[1-9]$/.test(e.key) && !e.ctrlKey && !e.altKey) go(PAGES[+e.key - 1]);
    else if (e.key === 'Enter' && (document.activeElement === document.body || !document.activeElement)) play();
    else if (e.key === 'Escape' && bootShown) { bootShown = false; renderBoot(); }
  });
  if (!reduce) setInterval(cycleFeed, 2600);
  noise(); clock(); setInterval(clock, 1000);
  if (!S.gameDir) go('folder');
  poll();
}
init();
