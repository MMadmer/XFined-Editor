# NQ — Node Quest Graph. Архитектура нодового графа квестов

Документ описывает, как в XFined Editor и в Dead Air Refined внедряется система
квестов на нодовом графе: формат ассета `.nqasset`, каталог действий, модель
исполнения, движковые правки, редакторский UI, MCP, экспорт, тесты и план
внедрения по фазам. Это рабочий гайд для реализации: все решения приняты, открытых
вопросов не остаётся (реестр решений и их причины — §18). Все якоря в код даны по
текущим деревьям `D:\Games\XFined-Editor` (редактор) и
`D:\Games\Dead Air\DeadAir-x64` (движок); распакованные скрипты игры —
`D:\Games\Dead Air\_analysis\original\scripts` (только чтение, это эталон).

Статус: **реализовано, покрыто runtime regression-тестами и editor MCP build
gate.** До первого релиза редактора формат можно менять свободно (правило из
`CLAUDE.md`), после релиза §2 замораживается.

---

## 0. Резюме решений (TL;DR)

1. **1 ассет = 1 квест = 1 файл `.nqasset`.** Файл — валидный Lua, но строго
   декларативный: `return { ... }` (одна таблица, никакого кода вне строк).
   Читается и человеком, и ИИ, и редактором, и игрой одним и тем же способом.
   **AI-first**: нейронка создаёт и правит квест текстом, без скриншотов — схема
   видов отдаётся `quest_catalog`, координаты нод необязательны (редактор
   раскладывает сам), проблемы возвращаются текстом с кодами, `quest_get` отдаёт
   канонический текст плюс текстовый outline графа (§4.5).
2. **Редактор НЕ компилирует граф в игровой Lua.** При билде он только
   валидирует и кладёт файл в модуль как есть. Всю логику исполняет
   **NQ-рантайм внутри игры** (Lua-скрипты Dead Air Refined + ~200 строк C++).
   Обновление игры чинит все моды разом — ровно то, что требовалось.
3. **Три пула «что можно выбрать»** — триггеры/основные действия/доп. действия/
   условия — описаны **каталогом** (`configs\nq\catalog.ltx` + `configs\nq\kinds\*.ltx`),
   который поставляет игра, а редактор сканирует из подключённой игры (как режимы)
   с бандл-фоллбэком. Основные действия — строго из каталога. Доп. действия и
   условия — из каталога **плюс** `lua` (кастомный код).
4. **Модель исполнения — токены** (набор активных нод), детерминированный
   порядок, отложенная очередь переходов, `pcall` на каждой границе: сломанный
   квест не роняет ни игру, ни соседние квесты.
5. **Диалоги** — фразы это ноды; рантайм собирает из них `CPhraseDialog` через
   `init_func`-путь движка. Нужны маленькие движковые правки: регистрация
   «виртуальных» диалогов и хук выдачи диалогов NPC (§7, §12.3).
6. **Состояние квестов** — один блоб `xms.save_data("xms.nq", …)` в `.scov`
   (заморож. контракт сейвов не трогаем), с обязательным перестейджем после загрузки.
7. **Тексты** — UTF-8 в ассете, конвертация в cp1251 на стороне игры при загрузке
   (`translate()` движка отдаёт сырой ключ, string table не нужна).
8. **Редактор**: новый тип ассета в контент-браузере (ПКМ → Create → Quest Graph),
   отдельная вкладка-документ с бесконечным канвасом (пан ПКМ, 10 уровней зума,
   поток сверху вниз, у ноды полоски on_enter/on_exit), инспектор справа,
   свой undo, полный MCP (`quest_*`), валидация = гейт билда.
9. **Ссылки на объекты**: строковые story id (CoC-стиль `[story_object]`),
   `ref`-имена для того, что квест сам заспавнил, профили/сообщества для «любого NPC».
10. **Задания PDA** — рантайм ведёт `CGameTask` напрямую, минуя `task_manager`
    (тот читает свой ltx один раз при загрузке скрипта и не расширяется).
11. **Никаких XML/ltx-заглушек от редактора**: инфопоршни не нужны (движок не
    падает на неизвестных id, а флаги квеста живут в его переменных), диалоги и
    задания регистрируются рантаймом.
12. **Внедрение — 6 фаз**, игра и редактор пилятся параллельно субагентами после
    фиксации контракта (§17).

---

## 1. Термины

| Термин | Значение |
|---|---|
| **Квест** | один `.nqasset`; в игре имеет uid `<module id>.<quest id>` |
| **Нода** | элемент графа: `id`, `kind` (вид), `params`, `cond`, `on_enter[]`, `on_exit[]`, `out{}` |
| **Вид (kind)** | запись каталога: что нода/действие/условие делает, какие у него параметры и пины |
| **Триггер** | корневая нода без входа (овал «T» на канвасе); взводится, пока квест активен |
| **Основное действие (main)** | вид ноды; строго из каталога; бывает мгновенным и ожидающим |
| **Доп. действие (extra)** | мгновенная операция в `on_enter`/`on_exit`; из каталога или `lua` |
| **Условие (cond)** | предикат из каталога или `lua`; используется триггерами, ветвлениями, фразами |
| **Пин** | именованный выход ноды (`next`, `done`, `yes`, …); у пина 0..N целей |
| **Токен** | «квест сейчас стоит на этой ноде»; на одной ноде максимум один токен |
| **Ref** | имя, под которым квест запомнил созданный им объект (отряд, предмет, якорь) |
| **NQ-рантайм** | набор скриптов `xms_nq*.script` в loose-слое Dead Air Refined |
| **Каталог** | `configs\nq\catalog.ltx` (+ `configs\nq\kinds\*.ltx`) — реестр видов |

---

## 2. Контракт NQ: что заморозится на релизе

После первого публичного релиза редактора замораживаются (по правилу
`PROJECT_RULES.md` §1 игры и `CLAUDE.md` редактора):

1. **Формат `.nqasset`** (§4) — поле `nq = <версия>`; игра поддерживает все
   выпущенные версии; миграции — на стороне игры (`xms_nq_load.migrate`).
2. **Семантика каталога** (§6): вид, однажды выпущенный, никогда не меняет смысла;
   новые виды и новые необязательные параметры добавляются свободно; удаление вида —
   запрещено (реализация остаётся, редактор помечает «устарел»).
3. **Модель исполнения** (§5) — жизненный цикл, порядок, `once`, поведение
   `flow.end`, правило «пин активирует все цели».
4. **Схема сохранённого состояния** (§11) — только аддитивные изменения с
   миграцией по полю `v`.

До релиза всё это правится свободно, старые ревизии не поддерживаются.
Совместимость сейвов игры (`.scop`/`.scoc` byte-for-byte, `.scov` только чанками)
не затрагивается ни в каком случае: NQ пишет исключительно через `xms.save_data`.

---

## 3. Файлы и папки

### 3.1 Проект редактора и модуль в игре

```
<project>\
  quests\wolf_debt.nqasset          ; ассет (может лежать в любой папке проекта,
                                    ;  кроме source-only rawdata\ и Content\)
<game>\modules\<id>\
  mod.ltx                           ; без изменений; квесты находятся сканом
  quests\wolf_debt.nqasset          ; экспорт копирует файл как есть
```

Рантайм сканирует **весь корень модуля рекурсивно** по маске `*.nqasset`
(нативка `xms_native_list_files`, §12.3), поэтому расположение файла внутри
модуля не имеет значения. Экспорт (`EditorMod::Export`,
`Source/Editors/XrECore/Editor/EditorModManifest.cpp:843-1063`) и так копирует
все не-editor-only папки целиком (`:939-960`) — для `.nqasset` не нужен отдельный
экспорт-хук, нужен только **гейт валидации** (§13.10).

### 3.2 Игра (репо движка)

```
packaging\dead-air-x64\compatibility\gamedata\
  scripts\xms_nq.script            ; ядро: API, on_game_start, тик, диспетчер, состояние
  scripts\xms_nq_load.script       ; поиск .nqasset, парс, валидация, канонизация, миграции
  scripts\xms_nq_kinds.script      ; реализации видов каталога (main / extra / cond)
  scripts\xms_nq_dialog.script     ; сборка CPhraseDialog, коллбеки движка
  scripts\xms_nq_task.script       ; CGameTask-обёртки
  scripts\xms_nq_util.script       ; cp1251, резолв ссылок, места, время, сериализация
  scripts\xms_nq_console.script    ; консоль `nq …`
  configs\nq\catalog.ltx           ; каталог ядра
  configs\nq\kinds\                ; расширения каталога (модули кладут сюда свои *.ltx)
src\xrGame\xms_game.cpp            ; новые нативки (§12.3)
src\xrGame\PhraseDialog.{h,cpp}    ; виртуальные диалоги (§12.3)
src\xrGame\actor_communication.cpp ; хук выдачи диалогов (§12.3)
src\xrGame\console_commands.cpp    ; команда `nq`
docs\dead-air\NQ_RUNTIME.md        ; спецификация рантайма (выделяется из этого дока в фазе 0)
```

`script.ltx` менять не нужно: `axr_main.on_game_start()` перебирает все корневые
`*.script` и вызывает их `on_game_start()` (`axr_main.script:198-232`), а сам
доступ `_G[name]` автозагружает файл.

### 3.3 Редактор

```
Source\Editors\XrECore\Editor\Nq\        ; модель, IO, каталог, валидатор, документ, MCP, экспорт-гейт
  NqAsset.h/.cpp        NqLua.h/.cpp        NqCatalog.h/.cpp
  NqValidate.h/.cpp     NqPickers.h/.cpp    NqDoc.h/.cpp
  NqExport.cpp          NqMcp.cpp
Source\Editors\LevelEditor\UI\QuestGraph\ ; окно-вкладка, канвас, инспектор
  UIQuestGraph.h/.cpp   ; UIQuestGraph (реестр документов, Open/Update/Close) + UIQuestGraphWindow (окно на документ)
  NqCanvas.h/.cpp       NqInspector.h/.cpp
gamedata\configs\nq\catalog.ltx           ; бандл-фоллбэк каталога (копия игрового)
docs\NQ_ARCHITECTURE.md                   ; этот документ
docs\nq\examples\*.nqasset                ; эталонные квесты (фаза 0)
tools\mcp\xfined_mcp.py, tools\mcp\AI_SETUP.md ; новые тулы quest_*
```

Слоение: всё, что не UI, — в `XrECore` (чтобы `EditorMod::Export` мог звать
валидатор); UI — в `LevelEditor`.

---

## 4. Формат `.nqasset`

### 4.1 Правила

- Кодировка UTF-8 без BOM, `\n`. Расширение `.nqasset`.
- Содержимое — **один `return { … }`**. Разрешены: таблицы, строки (`"…"`, `[[…]]`),
  числа, `true/false`, комментарии. Запрещены: вызовы функций, операторы, `local`,
  ссылки на глобалы. Игра грузит чанк через `loadstring` + `setfenv(chunk, {})`
  (пустое окружение) — любой «код» упадёт на загрузке и квест будет отклонён с
  диагностикой; редактор грузит тем же способом через C API LuaJIT (§13.3).
- Кастомный Lua допустим **только** внутри строкового поля `params.code` у
  действия/условия вида `lua`.
- Ключи таблиц — идентификаторы (`id = …`), не `["id"] = …` (читается обоими
  вариантами, пишется каноническим). Исключение — ключи-зарезервированные слова Lua
  (пин `else` у `flow.branch`, поле `["not"]` у условий): они пишутся как `["else"] = …`.
- Редактор при сохранении **переписывает файл канонически** (порядок ключей и
  форматирование фиксированы, §13.3): комментарии, написанные рукой/ИИ, теряются
  (это осознанно — диффы стабильны), а в начало файла записывается генерируемый
  блок-комментарий с outline квеста (§4.5, п.7).
- `pos` у ноды необязателен: ноды без координат раскладываются автоматически при
  открытии (§4.5, п.2).

### 4.2 Схема

```lua
return {
  nq        = 1,                 -- версия формата (обязательно)
  id        = "wolf_debt",       -- [a-z0-9_], уникален в модуле; uid в игре = "<module>.<id>"
  title     = "Долг вежливости", -- текст (см. типы), для журнала/отладки/PDA-фоллбэка
  activation= "auto",            -- "auto" (при старте/установке) | "manual" (действием quest.activate)
  vars      = { <name> = <default>, … },        -- переменные квеста (bool/number/string)
  tasks     = { <task id> = { title=<text>, descr=<text>, type="additional"|"storyline",
                              target=<target_ref>|nil, icon=<string>|nil,
                              -- шаги задачи: у каждого свой текст и СВОЯ метка на карте
                              objectives = { { id=<ident>, title=<text>, descr=<text>,
                                               target=<target_ref>|nil,
                                               visible=<bool>|nil }, … } }, … },
  nodes     = { <node>, <node>, … },            -- порядок = порядок в файле (важен для детерминизма)
}

<node> = {
  id      = "meet_wolf",          -- [a-z0-9_], уникален в квесте
  kind    = "dialog.topic",       -- вид из каталога, use ⊇ {main} или {trigger}
  once    = true,                 -- необязательно; дефолт зависит от вида
  params  = { … },                -- параметры вида
  cond    = { <cond>, … },        -- необязательно; AND-список (для триггеров, фраз, ожиданий)
  on_enter= { <action>, … },      -- необязательно; выполняются по порядку до основного действия
  on_exit = { <action>, … },      -- необязательно; после завершения основного действия
  out     = { <pin> = "<node id>" | { "<id>", "<id>" }, … }, -- пины из каталога вида
  pos     = { x, y },             -- редакторское; координаты канваса при зуме 1.0
  comment = "…",                  -- редакторское
}

<action> = { kind = "item.give", params = { section = "wpn_pm", count = 1 } }
<action> = { kind = "lua", params = { code = [[ nq.news("Привет") ]] } }
<cond>   = { kind = "var", params = { name = "greeted", op = "eq", value = false }, ["not"] = false }
<cond>   = { kind = "lua", params = { code = [[ return nq.actor:money() > 500 ]] } }
```

Универсальное правило: **любая сущность каталога — `{ kind, params }`**; для
условий дополнительно `["not"] = true` инвертирует результат.

### 4.3 Типы значений параметров

| Тип | Форма в файле | Заметки |
|---|---|---|
| `text` | `"строка"` или `{ rus = "…", eng = "…" }` | UTF-8; плейсхолдеры `{var:name}`, `{actor}` |
| `string`, `int`, `float`, `bool`, `enum(a\|b)` | как есть | |
| `duration` | `{ seconds = N }` (реальные) или `{ game_hours = N }` / `{ game_minutes = N }` | |
| `npc_ref` | `{ story = "esc_m_trader" }` \| `{ ref = "guard" }` \| `{ profile = "sim_default_stalker_1" }` \| `{ community = "stalker", level = "l01_escape" }` | порядок резолва §8 |
| `target_ref` | `npc_ref` \| `{ ref = "boars" }` (отряд/объект) \| `{ smart = "esc_smart_terrain_2_12" }` | для целей заданий/меток |
| `place` | `{ level = "l01_escape", pos = {x,y,z}, radius = 5 }` \| `{ restrictor = "zone_name" }` \| `{ smart = "…" }` | |
| `objective_id` | `"bread"` | шаг задачи, названной параметром `task` рядом. Редактор даёт выпадающий список шагов этой задачи |
| `object_ref`, `squad_ref` | `{ story = "esc_m_trader" }` \| `{ ref = "stash" }` | ОДИН конкретный объект мира. Уже, чем `npc_ref`: `profile`/`community` называют вид существа, а не вещь, которую можно залутать, запомнить под ref или посчитать её смерть |
| `spawn_spec` | `{ section = "simulation_boar", smart = "…", ref = "boars", hold = true }` | для `spawn.squad` и целей `objective.kill` |
| `item_section`, `squad_section`, `level`, `smart`, `story_id`, `profile`, `community`, `info`, `var_name`, `task_id`, `quest_id` (`"<module>.<quest>"` или `"<quest>"` = свой модуль), `ref_name`, `signal_name`, `spot_type`, `relation` (`enemy\|neutral\|friend`) | строки | редактор даёт пикеры (§13.6) |
| `lua` | `[[ код ]]` | см. §14 |
| `cases` | `{ { name = "yes", cond = { … } }, … }` / `{ { name = "a", weight = 3 }, … }` | имена = пины |

**Подзадачи.** Движок носит их с самого начала: `CGameTask` держит вектор
`SGameTaskObjective`, у каждого свои заголовок, описание, иконка и метка
(`set_map_location` / `set_map_object_id`). Индекс 0 — сама задача, шаги нумеруются
с единицы в порядке объявления. Поэтому «собери X, собери Y, доложи Z» — это ОДНА
задача PDA с тремя строками и, если надо, тремя метками, а не три квеста.
Состояние шагов живёт в `qs.objectives[<task>][<objective>]`, переживает сохранение
и читается условием `objective_status`, так что граф может ветвиться на «хлеб найден,
аптечка ещё нет». Завершение шага (`task.objective_complete`) саму задачу активной
оставляет — закрывать её по-прежнему `task.complete`.

**Видимость шага.** `visible = false` в объявлении прячет шаг: он работает как
обычно (его можно завершить, `objective_status` его видит), но в PDA не занимает
строку — как Collapsed в UI UE. Показать/спрятать по ходу квеста —
`task.set_objective_visible`. Что квест сделал руками, запоминается в
`qs.ovis[<task>][<objective>]` и переживает сохранение; движок сериализует задачу
БЕЗ её шагов (`CGameTask::save` пишет только себя), поэтому после загрузки
рантайм пересобирает шаги из объявления и возвращает им статусы и видимость
(`xms_nq_task.script`, `restore_objectives`).

### 4.4 Эталонный пример

Ids сталкеров/смартов иллюстративны.

```lua
-- Долг вежливости: Сидорович просит перебить кабанов у деревни новичков.
return {
  nq = 1,
  id = "wolf_debt",
  title = "Долг вежливости",
  activation = "auto",
  vars = { agreed = false },
  tasks = {
    kill_boars = { title = "Отстрел кабанов", descr = "Сидорович просил перебить кабанов у деревни новичков.",
                   type = "additional", target = { ref = "boars" } },
  },
  nodes = {
    { id = "start", kind = "trigger.start", out = { next = "meet" }, pos = {0, 0} },

    { id = "meet", kind = "dialog.topic",
      params = { npc = { story = "esc_m_trader" }, text = "Слышал, у тебя проблемы с кабанами." },
      once = true,
      out = { next = "sid_1" }, pos = {0, 160} },
    { id = "sid_1", kind = "dialog.npc_phrase", params = { text = "Лезут из леса. Перебьёшь — не обижу." },
      out = { next = { "accept", "decline" } }, pos = {0, 300} },
    { id = "accept", kind = "dialog.actor_phrase", params = { text = "Помогу." },
      on_exit = { { kind = "var.set", params = { name = "agreed", value = true } },
                  { kind = "task.give", params = { task = "kill_boars" } } },
      out = { next = "kill" }, pos = {-160, 440} },
    { id = "decline", kind = "dialog.actor_phrase", params = { text = "Не моё дело." }, pos = {160, 440} },

    { id = "kill", kind = "objective.kill",
      params = { target = { spawn = { section = "simulation_boar", smart = "esc_smart_terrain_2_12", ref = "boars", hold = true } } },
      out = { done = "report" }, pos = {-160, 580} },

    { id = "report", kind = "dialog.topic",
      params = { npc = { story = "esc_m_trader" }, text = "Кабаны мертвы." },
      out = { next = "sid_thanks" }, pos = {-160, 720} },
    { id = "sid_thanks", kind = "dialog.npc_phrase", params = { text = "Молодец. Держи." },
      on_exit = { { kind = "item.give",  params = { section = "wpn_pm", count = 1 } },
                  { kind = "money.give", params = { amount = 1500 } },
                  { kind = "task.complete", params = { task = "kill_boars" } } },
      out = { next = "finish" }, pos = {-160, 860} },
    { id = "finish", kind = "flow.end", params = { status = "completed" }, pos = {-160, 1000} },
  },
}
```

### 4.5 AI-first: как нейронка работает с ассетом без скриншотов

Требование проекта: ИИ должен читать, писать и править квесты **текстом**, не глядя
на канвас. Что это гарантирует:

1. **Файл самодостаточен.** Всё, что знает редактор о квесте, лежит в `.nqasset`;
   ничего не хранится в сайдкарах или в состоянии редактора. Порядок нод в файле —
   логический (сверху вниз по потоку), редактор при сохранении сортирует ноды
   топологически от триггеров (устойчиво: связи не меняются — порядок не меняется).
2. **Координаты необязательны.** Нода без `pos` получает позицию авто-раскладкой при
   открытии; ИИ никогда не обязан писать `pos`. Редакторские поля (`pos`, `comment`)
   всегда идут последними в ноде, чтобы не мешать чтению.
3. **Схема — по запросу.** `xfined_quest_catalog` возвращает все виды с группами,
   назначением (`use`), параметрами (тип/обязательность/дефолт/enum), пинами и
   однострочным описанием — этого достаточно, чтобы написать квест с нуля. Тот же
   каталог лежит в `docs\nq\NQ_FORMAT.md` (фаза 0) и в промпт-блоке `AI_SETUP.md`.
4. **Проблемы — текстом.** `xfined_quest_write` (записать целиком) /
   `xfined_quest_validate` возвращают список `{code, severity, node, slot, message}`
   (§15) — тот же формат, что печатает игра. Позиция синтаксической ошибки Lua —
   строка:колонка.
5. **Чтение — текстом.** `xfined_quest_get` отдаёт канонический текст файла и
   **outline** — линейное текстовое описание графа:
   ```
   quest madmer.cordon_tales.wolf_debt "Долг вежливости" (active: auto; vars: agreed; tasks: kill_boars)
   [T] start (trigger.start) -> meet
   meet (dialog.topic; npc=story:esc_m_trader; once) "Слышал, у тебя проблемы с кабанами."
       next -> sid_1
   sid_1 (dialog.npc_phrase) "Лезут из леса. Перебьёшь — не обижу."  next -> accept, decline
   accept (dialog.actor_phrase) "Помогу."  on_exit: var.set(agreed=true), task.give(kill_boars)  next -> kill
   decline (dialog.actor_phrase) "Не моё дело."  (leaf)
   kill (objective.kill; target=spawn simulation_boar@esc_smart_terrain_2_12 ref=boars)  done -> report
   …
   finish (flow.end; completed)
   problems: none            -- иначе: "W011 sid_1: безусловны не все ответы игрока — …"
   ```
   Outline можно запросить и без открытия вкладки (`xfined_quest_list` даёт сводку по
   всем квестам).
6. **Правка — целиком или операциями.** ИИ обычно переписывает файл целиком
   (`quest_write` или прямая запись файла + `quest_reload`); точечные правки — `quest_apply`
   с операциями, тоже текстом (Lua-таблица).
7. **Генерируемый заголовок.** Канонический писатель добавляет в начало файла блок
   комментариев с тем же outline (пересобирается при каждом сохранении, при чтении
   игнорируется) — открыв файл в любом редакторе, человек или ИИ видит структуру
   квеста без графа.
8. **Имена говорящие.** Виды и параметры — короткие английские идентификаторы с
   точечными пространствами (`dialog.topic`, `objective.kill`, `item.give`), тексты —
   на языке игрока.
9. **Скриншоты не нужны, но возможны.** `xfined_quest_view` + `xfined_screenshot_editor`
   остаются для контроля вида, но ни один шаг рабочего цикла ИИ от них не зависит.

---

## 5. Модель исполнения (одинакова для редактора-симулятора и игры)

### 5.1 Жизненный цикл квеста

`inactive → active → completed | failed`. Активация:

- `activation = "auto"`: при первом безопасном моменте (`actor_on_first_update`)
  каждой игры/загрузки, если модуль применим (`xms.module_applies(id)`, §12.3) и в
  состоянии нет записи о квесте. Работает и на **существующем сейве** после
  установки мода — квест просто стартует при следующей загрузке (тот же принцип,
  что у add-операций спавна XMS).
- `activation = "manual"`: действием `quest.activate` из другого квеста или
  консолью `nq activate <uid>`.
- При активации: создаётся запись состояния, все `trigger.start` получают токен и
  срабатывают, все `trigger.when` взводятся.
- Терминал: `flow.end` (статус явно) **или** неявно — когда не осталось ни токенов,
  ни взведённых триггеров (статус `completed`). Квест с триггером `repeat=true`
  остаётся активным до явного `flow.end` (это и есть «фоновый» квест).
  `flow.end{restart=true}` сбрасывает состояние (токены, `done`, `fired`, `joins`,
  таймеры; `vars` — к дефолтам; `refs` и задания — остаются) и активирует заново
  (повторяемые квесты).
- `quest_status` для квеста, которого нет (модуль удалён/не применим) — `inactive`.

### 5.2 Жизненный цикл ноды

```
enter(node):
  если токен уже есть → игнор (макс. один токен на ноду)
  если node.once и node ∈ done → игнор
  tokens[node] = { t0 = now }
  run_actions(node.on_enter)            -- синхронно, по порядку, каждое в pcall
  res = kind.begin(ctx)                 -- мгновенный вид → {pin=…}; ожидающий → "wait"
  если res ≠ "wait" → complete(node, res.pin)

complete(node, pin):
  run_actions(node.on_exit)
  tokens[node] = nil; если node.once → done[node] = true
  для каждой цели в node.out[pin] (в порядке файла) → enqueue(enter(target))
```

- Ожидающие виды получают `kind.poll(ctx)` раз в тик и/или `kind.on_event(ctx, evt)`
  по событию; возвращают имя пина → `complete`. `begin` ожидающего вида сразу делает
  первую проверку: если условие уже выполнено (цель уже мертва, предмет уже в
  инвентаре, актор уже в зоне) — нода завершается при входе.
- Пин без целей — ветка кончается. Пин активирует **все** свои цели (fork).
- `flow.join` считает приходы по числу входящих рёбер, срабатывает при полном наборе,
  сбрасывает счётчик.
- Циклы (ребро назад) разрешены; `once` защищает от повторного входа.

### 5.3 Порядок, тик, отложенная очередь

- Единственная нить, всё в `actor_on_update` (регистрация в `on_game_start`) плюс
  событийные коллбеки (`actor_on_item_take`, `npc_on_death_callback`,
  `squad_on_unregister`, `actor_on_info_callback`, `actor_on_leave_dialog`,
  `on_level_changing` …, полный список — `axr_main.script:24-161`).
- Тик: очередь переходов дренится **каждый кадр** (дёшево, обычно пуста); опрос
  `poll`-нод и триггеров — **раз в 250 мс** (`time_global()`).
- Переходы (`enter`) **всегда** идут через очередь `queue`; действия `on_enter/on_exit`
  самой ноды выполняются синхронно. Внутри дренирования новые переходы дописываются в
  хвост — реентерабельности нет.
- Переходы, порождённые из коллбеков диалога (фраза сказана), тоже кладутся в
  очередь и дренятся на следующем кадре — иначе изменение состояния внутри
  `SayPhrase` может отфильтровать все продолжения фразы и словить
  `R_ASSERT2("No available phrase to say")` (`PhraseDialog.cpp:129-130`).
- Порядок обхода детерминирован: квесты по uid, ноды по порядку в файле, триггеры
  по порядку в файле.
- Бюджет: пока не загружен ни один квест — коллбеки не регистрируются вовсе (0
  стоимости в ванили). С квестами: O(ожидающих poll-нод) раз в 250 мс, события —
  через индекс `event → {quest,node}`.

### 5.4 Ошибки

- Каждая граница (загрузка ассета, `begin/poll/on_event`, каждое действие, каждое
  условие, коллбеки диалогов) — в `pcall`.
- Ошибка действия: лог `! [nq] <uid>/<node>: <action#i kind>: <err>`, действие
  пропускается, нода продолжает (остальные действия и переход выполняются).
- Ошибка `begin/poll`: токен остаётся, `errors[node] = err`, повтор на следующем
  тике не чаще раза в 5 с; `nq state <uid>` показывает ошибку.
- Невалидный ассет: квест не загружается, лог `! [nq] <file>: E0xx …`, остальные
  квесты не затрагиваются; `nq list` показывает `INVALID`.
- Режим `nq debug 1`: каждый переход дублируется в PDA-новости и лог.
- Крэш игры из-за содержимого `.nqasset` — считается багом рантайма.

---

## 6. Каталог видов v1

### 6.1 Формат каталога (`configs\nq\catalog.ltx` + `configs\nq\kinds\*.ltx`)

Один вид = одна секция. LTX выбран потому, что его нативно читают обе стороны
(`ini_file` в игре, `CInifile` в редакторе) и его дополняют модули (файл в
`gamedata\configs\nq\kinds\` виден через VFS-монтирование модуля; редактор сканирует
ту же папку через `EditorGameContent`).

```ini
[nq.catalog]                       ; заголовок каталога (только в catalog.ltx ядра)
version = 2                        ; версия каталога; редактор и quest_catalog показывают её
api     = 1                        ; версия контракта реализации видов (поля begin/poll/…)

[nq.item.give]                     ; секция = "nq." .. kind
group   = items                    ; группа в дропдауне
title   = Выдать предмет           ; cp1251, что видит автор
use     = extra                    ; список из: trigger, main, extra, cond
params  = section, count           ; порядок показа
section = item_section, required   ; <тип>[, required][, default=<v>][, min=<v>][, max=<v>][, enum=a|b|c]
count   = int, default=1, min=1
pins    =                          ; для main/trigger: список пинов; "cases" = пины из params.cases
wait    = false                    ; main: ожидающий вид?
once    = false                    ; main: дефолт node.once
impl    = core                     ; core | <module id> (кто даёт реализацию через xms.registry)
since   = 1                        ; версия каталога, в которой вид появился
```

**Точка с запятой — начало комментария**, поэтому её нельзя писать в значении:
`;` внутри `desc` обрезает описание на середине, и вид приезжает в редактор с
усечённым текстом без единой жалобы. В прозе вместо неё — тире.

`since` — это версия каталога, в которой вид ПОЯВИЛСЯ, а не в которой его последний
раз правили: поднять `since` у существующего вида значит спрятать его от редактора,
подключённого к игре постарше, и сломать уже написанные квесты. Новые ПАРАМЕТРЫ
старого вида `since` не трогают (см. `objective.fetch` в 6.3).

Расширение: модуль кладёт `gamedata\configs\nq\kinds\<mod>.ltx` и в своём
`scripts\register.script` регистрирует реализации:
`xms.registry.get("nq.kinds"):add("<kind>", { begin=…, poll=…, on_event=…, run=…, test=… })`.
Редактор показывает такие виды с пометкой модуля. Ядро регистрирует свои виды в
тот же реестр из `xms_nq_kinds.script`.

Реализация вида (таблица):

| Поле | Для чего | Сигнатура |
|---|---|---|
| `begin(ctx)` | main | → `{ pin = "…" }` или `"wait"` |
| `poll(ctx)` | main (wait=true) | → pin или `nil` |
| `on_event(ctx, evt)` | main (wait=true) | → pin или `nil`; `evt = { name, … }` |
| `cancel(ctx)` | main | снятие токена извне (`flow.end`, reset) — почистить рефы/подписки |
| `run(ctx, params)` | extra | мгновенно |
| `test(ctx, params)` | cond | → bool |
| `save(ctx)`/`load(ctx, w)` | main | подсостояние токена (`tokens[node].w`) |

`ctx` = `{ quest, node, params, vars, refs, npc (партнёр диалога или nil), actor, now, log, emit(evt) }`.

### 6.2 Триггеры (корни, `use = trigger`)

| kind | params | пины | семантика |
|---|---|---|---|
| `trigger.start` | — | `next` | срабатывает один раз при активации квеста |
| `trigger.when` | `repeat: bool=false`, `cooldown: duration?`; условия — `node.cond` | `next` | срабатывает на **восходящем фронте** «все условия истинны»; момент взвода считается «ложью» — если условия уже истинны при активации квеста, триггер срабатывает сразу; при `repeat` перевзводится, когда условия снова ложны (и прошёл `cooldown`); `fired[node]` считает срабатывания |

Событийные условия (`event.*`) внутри `trigger.when`/`wait.when` подписываются на
коллбеки; уровневые (`has_item`, …) — опрашиваются. Условие-событие считается
истинным ровно в момент события; остальные условия списка проверяются тут же.

### 6.3 Основные действия (`use = main`)

| kind | params | пины | wait | реализация в игре |
|---|---|---|---|---|
| `dialog.topic` | `npc: npc_ref`, `text`, `initiator: enum(actor\|npc)=actor` (+`node.cond`, `node.once` default **true**) | `next` (фразы), `done` (лист сказан) | да | §7 |
| `dialog.npc_phrase` | `text` (+`node.cond` = precondition) | `next` | нет | фраза NPC |
| `dialog.actor_phrase` | `text` (+`node.cond`) | `next` | нет | фраза игрока |
| `objective.kill` | `target: {story}\|{ref}\|{spawn=spawn_spec}`, `by_actor: bool=false` | `done` | да | `npc_on_death_callback`/`monster_on_death_callback`/`squad_on_npc_death`/`squad_on_unregister` + опрос `alife():object(id)` для оффлайн-смертей; `spawn` создаёт цель при входе (`SIMBOARD:create_squad`, `sim_board.script:176`) и запоминает `ref` |
| `objective.fetch` | ТРИ формы, ровно одна за раз (иначе E022): `section: item_section` (+`count: int=1`) — любые предметы секции в инвентаре; то же плюс `from: object_ref` — засчитываются только вынесенные из ЭТОГО контейнера; `item: object_ref` — один конкретный объект. `count` в штуках предметов: для патронов — коробки, а не патроны | `done` | да | `section` — опрос инвентаря актора (+`actor_on_item_take`); `from` — состав контейнера через `alife():get_children()` (работает оффлайн, что для тайника норма), id засчитывается, только если его видели ВНУТРИ контейнера и потом нашли у игрока; `item` — `parent_id` серверного объекта равен актору |
| `task.objective_complete` / `task.objective_fail` | `task`, `objective` | — | нет | отмечает ОДИН шаг задачи; сама задача остаётся активной (`actor:set_task_state(state, tid, index)`) |
| `task.set_objective_target` | `task`, `objective`, `target?` | — | нет | метка ОТДЕЛЬНОГО шага; без `target` снимается только она. У каждого шага своя (`SGameTaskObjective::change_map_location`) |
| `task.set_objective_text` | `task`, `objective`, `new_title?`, `new_descr?` | — | нет | текст отдельного шага |
| `task.set_objective_visible` | `task`, `objective`, `visible: bool=true` | — | нет | показать/спрятать шаг в PDA; спрятанный работает, но строку не занимает |
| `objective.kill_count` | `count: int=1`, `by_actor: bool=true`, и не более одного фильтра: `community` \| `squad: object_ref` \| `section: string` (иначе E022). Без фильтра — любая смерть NPC | `done` | да | счёт с входа в ноду, дедуп по id, счётчик в токене (`mark_dirty`), переживает сохранение. Оффлайн-смерти опрашиваются только для фильтра `squad` (единственный, у кого известен список кандидатов) и только при `by_actor = false`: опрос не знает убийцу, а убийство игроком всегда онлайн и всегда приносит колбэк |
| `objective.reach` | `place`, `map_spot: bool=true`, `spot_text: text?` | `done` | да | `level.name()` + дистанция / `db.zone_by_name[...]:inside()`; для позиции создаётся якорь `space_restrictor` (`alife():create`) — под метку карты и цель задания, удаляется при выходе |
| `wait.timer` | `duration` | `done` | да | игровое время через `game.get_game_time()`; реальное — остаток в секундах, декремент по тику |
| `wait.when` | `timeout: duration?`; условия — `node.cond` | `done`, `timeout` | да | как `trigger.when`, но с входом |
| `wait.any` | `cases: [{name, cond[]}]` | `<cases>` | да | первый истинный кейс (по порядку) |
| `flow.branch` | `cases: [{name, cond[]}]` | `<cases>`, `else` | нет | первый истинный, иначе `else` |
| `flow.random` | `cases: [{name, weight}]` | `<cases>` | нет | взвешенный `math.random` |
| `flow.join` | — | `next` | да | ждёт приходов по всем входящим рёбрам |
| `flow.step` | — | `next` | нет | «Шаг»: пустое основное действие, только `on_enter/on_exit` |
| `flow.end` | `status: enum(completed\|failed)`, `restart: bool=false` | — | нет | терминал: снимает все токены (`cancel`), гасит триггеры, статус; `restart` — сброс и повторная активация |

Мгновенные операции мира (спавн, выдача, награды) — **не** основные действия, а
доп. (§6.4): «нода = ключевой этап», всё остальное — обвязка вокруг него. Если
нужен «этап без ожидания» — `flow.step`.

### 6.4 Доп. действия (`use = extra`; все мгновенные)

| kind | params | реализация (якорь) |
|---|---|---|
| `item.give` | `section`, `count=1` (штуки предметов: для патронов — коробки, а не патроны) | в контексте диалога — `dialogs.relocate_item_section_to_actor(npc, actor, section, count)` (`dialogs.script:1976`, с UI «получено»); иначе `alife():create(section, actor pos, lvid, gvid, actor:id())`×count (патроны — `create_ammo`, `_g.script:1438`) + `news_manager.relocate_item(actor,"in",…)` (`news_manager.script:247`) |
| `item.take` | `section`, `count=1` \| `"all"` (те же единицы, что у `item.give`: штуки, для патронов — коробки) | в диалоге — `dialogs.relocate_item_section_from_actor` (`:2058`); иначе итерация `db.actor:iterate_inventory` + `alife():release` |
| `item.spawn` | `section`, `place` \| `into: target_ref` (контейнер/NPC), `ref?` | `alife():create(section, pos, lvid, gvid[, parent_id])`; lvid/gvid для чужого уровня — `xms.graph_vertex` + `game_graph():vertex(gvid):level_vertex_id()` |
| `money.give` / `money.take` | `amount` | `db.actor:give_money(±n)`; в диалоге `dialogs.relocate_money_to_actor/from_actor` (`:2025/:2031`) |
| `info.give` / `info.disable` | `info` | `db.actor:give_info_portion/disable_info_portion` — неизвестный id **не** ассертит (`InfoPortion.cpp:28-36`) |
| `var.set` / `var.add` | `name`, `value` / `delta` | переменные квеста (персистентны) |
| `signal.send` | `name` | событие `event.signal` для триггеров/ожиданий (в т.ч. чужих квестов, имя глобальное `<module>.<name>` или своё) |
| `quest.activate` | `quest: quest_id` | активация другого квеста (`activation=manual`) |
| `task.give` / `task.complete` / `task.fail` / `task.remove` | `task` | §9 |
| `task.set_target` | `task`, `target: target_ref` | §9 |
| `task.set_text` | `task`, `new_title?`, `new_descr?` | §9 (не `title`: служебный ключ каталога) |
| `spawn.squad` | `spawn_spec` (`section`, `smart`, `ref`, `hold=true`) | `SIMBOARD:create_squad(SIMBOARD.smarts_by_names[smart], section)` (`sim_board.script:176,142`) + `setup_squad_and_group` для членов (как `xr_effects.create_squad`, `xr_effects.script:3542`); `hold` → `squad.scripted_target = smart` (`sim_squad_scripted.script:102-106`), **не персистится движком** → рантайм переставляет из `refs` при `squad_on_register`/первом апдейте |
| `spawn.object` | `section`, `place`, `ref?` | `alife():create` |
| `squad.move` | `target: {ref}\|{story}`, `smart` \| `follow_actor: bool` | `squad.scripted_target = smart` или `"actor"` (`sim_squad_scripted.script:128`) |
| `squad.remove` | `target` | `SIMBOARD:remove_squad` через `safe_release_manager.release` (`xr_effects.script:3642,3316`) |
| `npc.remove` | `npc_ref` | `safe_release_manager.release(alife():object(id))` |
| `npc.kill` | `npc_ref` | `obj:kill(obj)` онлайн; оффлайн — релиз |
| `relation.set` | `who: npc_ref` (в т.ч. `{community=…}`), `value: relation` (отношение к игроку) | `npc:force_set_goodwill(±1000, actor)` / `squad:set_squad_relation` / `game_relations.change_factions_community_num` (`xr_effects.script:3821-3951`) |
| `relation.goodwill` | `who`, `delta` | `npc:change_goodwill`/`inc_faction_goodwill_to_actor` |
| `news.tip` | `text`, `sender: npc_ref?`, `icon?`, `duration=5` | `news_manager.send_tip(actor, text, nil, sender_go, showtime)` (`news_manager.script:67`), текст — cp1251 сырой |
| `map.spot` / `map.unspot` | `target: target_ref`, `text?`, `spot: spot_type=secondary_task_location` | `level.map_add_object_spot(id, spot, hint)` / `map_remove_object_spot` (в `xr_effects` эти обёртки закомментированы — зовём движок напрямую) |
| `dialog.force` | `npc: npc_ref`, `allow_break: bool=true` | `db.actor:run_talk_dialog(npc, not allow_break)` (`xr_effects.force_talk`, `xr_effects.script:1741`) |
| `dialog.break` | — | `actor:stop_talk(); npc:stop_talk()` (`dialogs.break_dialog`, `dialogs.script:1624`) |
| `actor.teleport` | `place` (тот же уровень) | `db.actor:set_actor_position(pos)` (`xr_effects.teleport_actor`, `:2200`); чужой уровень — ошибка валидации E031 (в DA нет скриптового межуровневого телепорта; переходы — через level changer) |
| `sound.play` | `theme: string` | `xr_sound.set_sound_play(actor:id(), theme)` (как `xr_effects.play_sound_on_actor`, `:3368`) |
| `lua` | `code` | §14 |

### 6.5 Условия (`use = cond`)

Уровневые (можно везде, включая фразы и `flow.branch`):

| kind | params | реализация |
|---|---|---|
| `has_info` | `info` | `has_alife_info(info)` (`_g.script:540`, работает оффлайн) |
| `has_item` | `section`, `count=1` (штуки предметов: для патронов — коробки, а не патроны) | подсчёт по инвентарю актора |
| `has_money` | `amount` | `db.actor:money()` |
| `var` | `name`, `op: enum(eq\|ne\|lt\|le\|gt\|ge)=eq`, `value` | переменные квеста |
| `node_done` | `node: string` | нода с `once` уже отработала (`done`) |
| `quest_status` | `quest: quest_id`, `is: enum(inactive\|active\|completed\|failed)` | состояние другого квеста |
| `task_status` | `task`, `is: enum(none\|active\|completed\|failed)` | §9 |
| `objective_status` | `task`, `objective`, `is: enum(none\|active\|completed\|failed)` | шаг задачи в статусе; состояние из `qs.objectives` |
| `actor_on_level` | `level` | `level.name()` |
| `actor_in_place` | `place` | как `objective.reach` |
| `npc_alive` / `npc_dead` | `npc_ref` | `se:alive()` серверного объекта / отсутствие |
| `squad_alive` | `target` | `alife():object(id) ~= nil` |
| `actor_community` | `community` | `db.actor:character_community()` |
| `relation` | `who: npc_ref\|community`, `is: relation` | `game_relations`/`npc:relation(actor)` |
| `time_of_day` | `from: int`, `to: int` (часы) | `level.get_time_hours()` |
| `elapsed` | `from: enum(quest\|node)=quest`, `node?`, `duration` | по `t0` квеста/токена, игровое или реальное время (имя `from`, а не `since`: `since` — служебный ключ каталога, ключи параметров не должны совпадать со служебными) |
| `chance` | `percent` | `math.random(100) <= percent` (для фраз — на каждый показ) |
| `lua` | `code` | §14, `return <bool>` |

Событийные (`event.*`; допустимы только в `trigger.when` / `wait.when` / `wait.any`;
в `cond` фраз и `flow.branch` — ошибка валидации E020):

| kind | params | источник |
|---|---|---|
| `event.item_taken` / `event.item_dropped` / `event.item_used` | `section` | `actor_on_item_take/drop/use` |
| `event.npc_killed` | `who: npc_ref\|community\|any`, `by_actor=false` | `npc_on_death_callback`, `monster_on_death_callback` |
| `event.squad_dead` | `target` | `squad_on_unregister` + оффлайн-опрос |
| `event.info_given` | `info` | `actor_on_info_callback` |
| `event.level_entered` | `level` | `actor_on_first_update` после `on_level_changing` |
| `event.talk_started` | `npc: npc_ref` | хук выдачи диалогов (§12.3) — момент открытия окна разговора |
| `event.talk_ended` | `npc` | `actor_on_leave_dialog(victim_id)` (`pda.script:81`) |
| `event.signal` | `name` | `signal.send` |
| `event.trade_done` | `npc?` | `actor_on_trade` |

Логические комбинаторы: `["not"]` на любом условии; `any` = `{ kind = "any", params = { of = { <cond>, … } } }`.

### 6.6 Правила расширения каталога

- Новый вид/параметр — новая секция/ключ, `since = <версия>`; редактор скрывает виды новее
  версии каталога подключённой игры только если такой игры нет; иначе показывает то,
  что игра реально отдала.
- Переименование вида запрещено; можно добавить `alias = old.kind` (рантайм резолвит).
- Обязательный параметр не может стать обязательным задним числом.

---

## 7. Диалоги

### 7.1 Как граф ложится на движок

Движковые факты (по `src\xrGame`): `CPhraseDialog::load_shared`
(`PhraseDialog.cpp:198-248`) при отсутствии `<phrase_list>` зовёт `init_func(dialog)`
**один раз на id за жизнь процесса** (данные — `CSharedClass`, `shared_data.h:118-127`);
`AddPhrase(text, id, prev_id, goodwill)` при повторном id **добавляет ребро** и
возвращает `nullptr` (`:252-282`) — циклы легальны, но precondition/action вешать
надо при первом добавлении; говорящий строго чередуется на каждом ребре
(`SayPhrase`, `:78`), корень — фраза `"0"` (`:38-40`); NPC выбирает ответ по
goodwill (порог по отношению, затем **случайно** внутри одинакового goodwill —
`AI_PhraseDialogManager.cpp:27-70`); если у не-листовой фразы все продолжения
отфильтрованы — `R_ASSERT2` (`:129-130`); precondition фразы —
`f(first, second, dialog_id, phrase_id, next_phrase_id)` (`PhraseScript.cpp:139-170`,
не обёрнут в try), action — `f(first, second, dialog_id, phrase_id)` (`:172-191`,
обёрнут); `script_text` — `f(first, second, dialog_id, phrase_id) → string` (`:79-92`);
текст фразы, не найденный в string table, отдаётся как есть (`StringTable.cpp:455-463`).
Lua-поверхность: `CPhraseDialog:AddPhrase`, `CPhrase:GetPhraseScript`,
`CPhraseScript:AddPrecondition/AddAction/SetScriptText/AddHasInfo/…`
(`PhraseDialog_script.cpp`).

Маппинг:

- Каждая нода `dialog.topic` = один диалог движка с id
  `nq.<module>.<quest>.<node>`; корневая фраза `"0"` = текст темы. При
  `initiator=actor` диалог попадает в список тем актора; при `initiator=npc` — назначается
  через `npc:set_start_dialog(id)` (`script_game_object_script3.cpp:291-293`) пока
  токен на теме есть и NPC онлайн; снимается `restore_default_start_dialog()`.
  `xr_meet` может перезаписать start_dialog по своей логике — рантайм переставляет
  значение раз в тик, если `npc:get_start_dialog() ~= id`.
- Каждая нода `dialog.*_phrase`, достижимая из темы по рёбрам через фразовые ноды,
  = фраза с id ноды; рёбра `next` = `AddPhrase(text, id, prev)`.
- **Чередование**: чётная глубина от корня — инициатор, нечётная — собеседник.
  Валидатор (E010) требует, чтобы `dialog.actor_phrase`/`dialog.npc_phrase` стояли на
  глубине правильной чётности по **всем** путям от темы; две подряд реплики одной
  стороны — оформляются двумя нодами через фразу «…» другой стороны (редактор
  предлагает вставить автоматически).
- **Детерминизм выбора NPC**: goodwill фраз = `-10000 - <порядковый номер в out.next>`;
  движок сортирует по убыванию goodwill и берёт первый проходящий порог → выбирается
  **первая по порядку** фраза, чей `cond` истинен. Для актора тот же порядок = порядок
  показа в списке.
- **Защита от ассерта**: рантайм добавляет скрытое безусловное продолжение-лист
  («Пока.» для стороны актора / «…» для стороны NPC), если продолжения фразы
  **не все** безусловны — одного безусловного соседа недостаточно
  (`build_dialog`: `#kids > 0 and (uncond == 0 or uncond < #kids)`). Безусловная
  фраза — без `cond` **и** без `once` (`is_uncond` = `#node.cond == 0 and not
  node.once`): `once` работает как precondition. Лист висит на минимальном
  goodwill, поэтому берётся только когда не прошёл ни один настоящий кандидат;
  но если до него дошли — разговор закрывается, а тема **не** засчитывается:
  ни `on_exit` темы, ни пина `done`, токен остаётся на теме. Среди реплик NPC до
  листа не добраться (любой безусловный сосед его перекрывает), в списке ответов
  игрока он — реальная выбираемая строка. W011 редактора повторяет этот расклад:
  для ответов игрока — ровно условие рантайма, для реплик NPC — только когда
  безусловных нет вовсе.
- **Выход из диалога в граф**: ребро из фразы в не-диалоговую ноду = «когда фраза
  сказана, активировать цель» (через очередь). Фраза без диалоговых детей = лист →
  диалог закрывается; в этот момент тема считается пройденной: `on_exit` темы,
  снятие токена (при `once`), пин `done`.
- Закрытие окна разговора на середине ветки ничего не завершает: тема остаётся
  доступной с начала (поведение X-Ray; состояние — через `cond`/`var`).
- `on_enter` фразы выполняется в `action` фразы **до** её отметки, `on_exit` — сразу
  после (оба синхронно, в одном коллбеке); поэтому `var.set` в `on_exit` реплики
  игрока уже виден в `cond` следующей реплики NPC (движок фильтрует продолжения
  после action, `PhraseDialog.cpp:95,113`).
- `once` темы: после листа тема исчезает; `once=false` — тема повторяема.
- NPC мёртв/не существует/оффлайн — тема просто не предлагается, токен ждёт; для
  развилки «а если он умер» — параллельная ветка `wait.any` / триггер с `npc_dead`.
- Один NPC — сколько угодно тем от разных квестов и модулей: список тем =
  объединение по всем активным токенам, порядок — по uid квеста и порядку нод.
- Тексты: без плейсхолдеров — статический cp1251 в `AddPhrase`; с `{…}` —
  `SetScriptText("xms_nq_dialog.text")`, функция подставляет значения на показ.
- Один набор функций на все квесты: `xms_nq_dialog.init(dialog)`, `.pre(...)`,
  `.act(...)`, `.text(...)`, `.query(npc, actor)`; демультиплексация по `dialog_id`.
  Внутри — `pcall`, precondition при ошибке возвращает `false`.

### 7.2 Регистрация и выдача NPC (движковые правки)

1. `xms_native_dialog_register(dialog_id, "xms_nq_dialog.init")` /
   `xms_native_dialog_unregister(id)` / `xms_native_dialog_invalidate(id)` —
   реестр «виртуальных» диалогов в `CPhraseDialog` (статическая
   `xr_flat_hash_map<shared_str, shared_str>`); `load_shared` проверяет реестр
   **до** `GetById` (`PhraseDialog.cpp:200`) и для виртуального id идёт по ветке
   `init_func` без XML. `invalidate` сбрасывает shared-данные диалога (для `nq reload`).
2. Хук в `CActor::UpdateAvailableDialogs` (`actor_communication.cpp:193-221`): после
   добавления `ActorDialogs()` партнёра — вызов Lua `xms.dialogs_for(partner, actor)`
   (если функция существует), полученные id → `AddAvailableDialog(id, partner)`.
   Рантайм возвращает только темы с активным токеном, подходящим NPC и истинными
   `cond` — это и есть availability-фильтр (dialog-level precondition не нужен).
   Этот же вызов = событие `event.talk_started`.
3. Регистрация идемпотентна и делается при каждой инициализации рантайма (Lua-стейт
   пересобирается на новую игру/загрузку/смену уровня).

---

## 8. Ссылки на объекты и цели

- **`story`** — строковый story id CoC-стиля: `[story_object] story_id = …` в
  `custom_data` спавна, реестр `story_objects.script` (`get_story_object_id(sid)`,
  `_g.script:1548`; не персистится, восстанавливается из спавна). Мод-плейснутый NPC
  получает story id через свой `custom_data` в `.xspawn` (`EditorModScene.cpp:774-792`
  уже пишет `custom_data` файлом) — редактор дописывает `[story_object]` по кнопке
  «назначить story id» в пикере (§13.6). Ванильные NPC — по их story id.
- **`ref`** — имя объекта, созданного этим квестом (`spawn.*`, `objective.kill{spawn}`,
  якорь `objective.reach`). Хранится в `refs[name] = { id, kind, section, smart, hold }`
  и переживает сейв. Дополнительно рантайм регистрирует story id
  `nq.<module>.<quest>.<ref>` в `story_objects` (не персистно, переставляется при
  загрузке) — чтобы кастомный Lua и ванильные кондлисты могли адресовать объект.
- **`profile`** — любой NPC с данным `character_profile` (`npc:profile_name()`).
- **`community`** (+`level`) — любой NPC сообщества (`npc:character_community()`).
- Резолв `npc_ref` в конкретный объект: `ref` → `story` → первый онлайн NPC по
  `profile`/`community`; для диалогов проверка идёт **от NPC** («подходит ли этот
  партнёр под ref»).
- **`place`**: `{level,pos,radius}` — дистанция от актора на этом уровне;
  `{restrictor}` — `db.zone_by_name[name]:inside()` (`xr_conditions.actor_in_zone`,
  `xr_conditions.script:760`); `{smart}` — радиус смарта.
- **Позиция → вершины**: `level.vertex_id(pos)` на текущем уровне,
  `xms.graph_vertex(level, x, y, z)` (`xms_game.cpp:563-587`) для любого уровня.

---

## 9. Задания PDA

`task_manager.script` читает `misc\task_manager.ltx` **один раз при загрузке скрипта**
и не имеет API регистрации (`task_manager.script:1,50-53`); `game_tasks.xml`
движка одиночный. Поэтому NQ ведёт задания сам через движковый `CGameTask`
(`GameTask_script.cpp:16-104`, `script_game_object_script3.cpp:226-238`):

- `task.give`: `t = db.actor:get_task(tid, true) or CGameTask(); t:set_id(tid);
  t:set_type(task.additional|task.storyline); t:set_title(text); t:set_description(text);
  t:set_priority(0); t:set_icon_name(icon or "ui_pda2_mtask_overlay");
  [t:set_map_location(spot); t:set_map_object_id(id); level.map_add_object_spot(id, blink, "")];
  db.actor:give_task(t, 0, true, 0)`.
  `tid = "nq.<module>.<quest>.<task>"`. Никаких complete/fail-функторов: состояние
  меняет только рантайм. Движковый actor callback уже отправляет новость `new`.
- `task.complete`: `db.actor:set_task_state(task.completed, tid)`; движковый actor
  callback уже отправляет `complete`, поэтому ручной вызов создал бы дубль.
- `task.fail`: `db.actor:set_task_state(task.fail, tid)` +
  `news_manager.send_task(db.actor, "fail", t)`. Binder намеренно не отправляет
  failure, а `task_manager.task_callback` для динамического NQ id молча выходит
  (`task_manager.script:184-187`), поэтому только failure остаётся ручным.
- `task.set_target`: `t:remove_map_locations(false); t:change_map_location(spot, id)`;
  цель `target_ref` резолвится в объект (`ref`/`story`/`smart` — у смарта есть id;
  для `place{pos}` — якорь-рестриктор). Цель на другом уровне остаётся меткой на карте
  того уровня (гайдеров `task_objects` не используем).
- `task.set_text`: `set_title/set_description`.
- Персистентность: сами задания хранит движок в реестре актора (это ванильное
  поведение); NQ хранит `tasks[tid] = "active"|"completed"|"failed"` в своём блобе и при
  загрузке сверяется с `db.actor:get_task(tid, false)`.
- Спот-типы: `secondary_task_location` / `storyline_task_location`, мигание
  `ui_secondary_task_blink` / `ui_storyline_task_blink` (как `task_objects.script:122-161`).

---

## 10. Тексты и локализация

- В ассете — UTF-8. `text` = строка (язык по умолчанию — `rus`) или таблица по языкам.
- Рантайм при загрузке выбирает язык из `localization.ltx [string_table] language`,
  фоллбэк — `rus`, затем любой; конвертирует **UTF-8 → cp1251** чистым Lua
  (`xms_nq_util.cp1251`: таблица кириллицы, `Ё/ё`, `— – « » “ ” … №`; прочее → `?`).
- Строки идут в движок сырыми: `AddPhrase`, `set_title`, `send_tip` — `translate()`
  возвращает ключ, если его нет в таблице. String table модуля не генерируется.
- Плейсхолдеры: `{var:name}` (переменная квеста), `{actor}` (имя актора),
  `{n}` внутри `count`-зависимых фраз не делаем — просто `{var:…}`.
- Редактор: язык редактирования — селектор в тулбаре; предупреждение W040 на символ
  вне cp1251.

---

## 11. Персистентность

- Канал: `xms.save_data("xms.nq", marshal.encode(state))` / `xms.load_data("xms.nq")`
  (`xms_game.cpp:487-519`; чанк `0x584D0000|ns`, `xms_game.h:21-28`). Для ядра вводится
  зарезервированный псевдо-ns `xms.nq → 0xFF01` (§12.3), чтобы не занимать блоб
  модуля (у автора модуля свой `xms.save_data(<id>)`).
- **Перестейдж обязателен**: загруженный блоб не попадает в следующий сейв сам
  (`xms_game.cpp:338-342`, `GetLoadedBlob` не стейджит). Рантайм: после
  `load_data` на `actor_on_first_update` сразу `save_data`; далее — при любом изменении
  состояния (`dirty`) в конце тика.
- `marshal` есть (`Externals\xrLuaFix\lua-marshal`, глобал `marshal`, `USE_MARSHAL`).

Схема:

```lua
state = {
  v = 1,
  quests = {
    ["madmer.cordon_tales.wolf_debt"] = {
      status = "active",                -- inactive|active|completed|failed
      hash   = 0x1234abcd,              -- FNV-1a канонического текста ассета на момент последнего запуска
      tokens = { kill = { t0 = <game secs>, w = { … } } },
      vars   = { agreed = true },
      refs   = { boars = { id = 5123, kind = "squad", section = "simulation_boar", smart = "…", hold = true } },
      done   = { meet = true },         -- ноды с once, которые отработали
      fired  = { t_item = 2 },          -- счётчики срабатываний триггеров
      joins  = { j1 = { a = true } },
      tasks  = { kill_boars = "active" },
      errors = { kill = "…" },
      timers = { wait_1 = { real_left = 12.5 } },
    },
  },
}
```

- **Смена графа между сессиями** (мод обновлён): `hash` не совпал → сверка: токен на
  несуществующей ноде или ноде с другим `kind` — снимается с логом; новые `vars` —
  дефолты; `refs`, `done`, `tasks` — сохраняются; триггеры перевзводятся по новому графу.
- **Мод удалён**: запись остаётся в блобе (крошечная), игнорируется; вернулся — продолжает.
- **Ванильный сейв / оригинальный DA**: `.scov` игнорируется, `.scop/.scoc` не тронуты.
- Игровое время хранится через `utils.CTime_to_table` (`utils.script:546`); реальное —
  остатком.

---

## 12. Игровая сторона: реализация

### 12.1 Инициализация и порядок

1. Скрипты `xms_nq*.script` — loose-слой Refined (`packaging\...\compatibility\gamedata\scripts`),
   ставятся с игрой как остальные `dead_air_x64_*`.
2. `xms_nq.on_game_start()`: `RegisterScriptCallback("actor_on_first_update", init)`.
   Ничего больше — `db.actor` ещё нет (`bind_stalker.script:38`), режимы не восстановлены.
3. `init` (на первом апдейте актора, `bind_stalker_ext.script:80`):
   `xms_nq_load.scan()` → для каждого модуля `xms.modules()` с `enabled` и
   `xms.module_applies(id)`: `xms.list_files(id, "", "*.nqasset", true)` →
   `xms.read_file` → парс в пустом окружении → `validate` → `canonicalize`
   (тексты в cp1251, дефолты, индексы) → реестр `quests[uid]`.
   Затем `state = decode(xms.load_data("xms.nq"))` → реконсиляция → регистрация
   диалогов (§7.2) → активация `auto`-квестов → восстановление `refs`
   (story id, `scripted_target`, start_dialog) → регистрация остальных коллбеков
   (только если квестов > 0) → перестейдж блоба.
4. Тик — `actor_on_update`; смена уровня — `on_level_changing` (флаш блоба уже сделан
   dirty-логикой; при новом стейте `init` повторится и подхватит pending-блоб).

### 12.2 Диспетчер событий

Индекс `subs[event_name] = { {quest, node}, … }` строится при входе токена в
ожидающую ноду и при взводе триггера; коллбеки движка превращаются в
`evt = { name = "npc_killed", npc = go, se = se, killer = who, story = sid, section = … }`
и раздаются подписчикам через очередь. Каждое событие обрабатывается детерминированно
(порядок подписки). Опрос (`poll`) — по списку `polled` раз в 250 мс.

### 12.3 Движковые правки (C++, все маленькие)

| # | Что | Где | Зачем |
|---|---|---|---|
| 1 | `xms_native_list_files(id, subdir, mask, recursive) → {relpaths}` | `xms_game.cpp` рядом с `xms_native_read_script (:460-485)`; поверх `XMS::ListFiles` (`xms_core.h:188-191`) + рекурсия | найти `*.nqasset` в модуле |
| 2 | `xms_native_read_file(id, relpath) → string\|nil` | там же; guard как `valid_script_name (:446-458)`: `[A-Za-z0-9_./-]`, без `..`, внутри `root` | прочитать ассет |
| 3 | `xms_native_module_applies(id) → bool` | обёртка `XMS::ModuleApplies` (`xms_core.cpp:1215-1238`) | гейт по `mode=` одной семантикой с движком |
| 4 | Псевдо-ns ядра для `save_data/load_data`: таблица `kCoreBlobs = { {"xms.nq", 0xFF01} }`, резолв до поиска модуля (`:487-519`) | `xms_game.cpp` | один блоб NQ, не задевая блобы модулей |
| 5 | Виртуальные диалоги: `CPhraseDialog::RegisterVirtual/UnregisterVirtual/InvalidateVirtual`; ветка в `load_shared` до `GetById` (`PhraseDialog.cpp:200`); нативки `xms_native_dialog_register/unregister/invalidate` | `PhraseDialog.{h,cpp}`, `xms_game.cpp` | диалоги без XML |
| 6 | Хук `CActor::UpdateAvailableDialogs` → Lua `xms.dialogs_for(partner, actor)` (если функция есть; `try/catch`, ошибка → лог и пусто) → `AddAvailableDialog(id, partner)` | `actor_communication.cpp:193-221` | темы квестов у любого NPC; на стороне Lua — индекс «NPC → темы с токенами», вызов дешёвый |
| 7 | `CMD1(CCC_XmsNq, "nq")` → `xms_nq_console.exec("<args>")` (по образцу `run_string`) | `console_commands.cpp:2442-2447` | отладка |
| 8 | Бутстрап `xms.*`: алиасы `xms.list_files`, `xms.read_file`, `xms.module_applies`, `xms.dialog_register/unregister/invalidate` | `xms_game.cpp:592-765` | единый API |

Всё аддитивно, ни один существующий контракт (сейвы, XMS, Lua API) не меняется.
`PROJECT_RULES.md` §3 (сверка с оригиналом) не затрагивается: правки — новые точки, не
изменение поведения ванили; ноль модулей = ноль вызовов.

### 12.4 Консоль `nq …`

`nq list` (uid, статус, ошибки) · `nq state <uid>` (токены, vars, refs, tasks) ·
`nq activate <uid>` · `nq reset <uid>` · `nq jump <uid> <node>` (снять всё, поставить
токен) · `nq fire <uid> <node>` (принудительно завершить ожидающую ноду пином по
умолчанию) · `nq reload` (перечитать ассеты + `dialog_invalidate` всех своих id;
состояние сохраняется, реконсиляция как при смене хэша) · `nq debug 0|1` ·
`nq validate` (перевалидировать все, вывести проблемы) · `nq dump` (json в
`$app_data_root$\nq_report.json`).

Лог-префикс: `* [nq]` / `~ [nq]` / `! [nq]`.

### 12.5 QA игры

По схеме `PROJECT_RULES.md` §8: скрытый запуск `xrEngine.exe -fsltx <qa.ltx> … -start server(<save>/single/alife/load)`,
QA-скрипт через `[common]`, ждёт `actor_on_first_update`, дальше `nq jump/fire` и
проверка `nq state` + лога. Сценарии — §16.

---

## 13. Редакторская сторона: реализация

### 13.1 Модель (`NqAsset.h`)

```cpp
struct SNqAction { xr_string kind; SNqParams params; };            // params: упорядоченная карта ключ→значение-варианта
struct SNqCond   { xr_string kind; SNqParams params; bool negate; };
struct SNqNode   { xr_string id, kind, comment; bool once, once_set; SNqParams params;
                   xr_vector<SNqCond> cond; xr_vector<SNqAction> on_enter, on_exit;
                   xr_vector<std::pair<xr_string, xr_vector<xr_string>>> out; Fvector2 pos; };
struct SNqTask   { xr_string id, title, descr, type, icon; SNqValue target; };
struct SNqQuest  { int nq; xr_string id, title, activation; xr_vector<SNqVar> vars;
                   xr_vector<SNqTask> tasks; xr_vector<SNqNode> nodes; };
```

`SNqValue` — вариант (nil/bool/number/string/table с упорядоченными ключами) — то,
что вернул парсер; типизация — по каталогу при валидации/отрисовке.

### 13.2 Каталог (`NqCatalog`)

Загрузка: сначала `EditorGameContent::Find("configs\\nq\\catalog.ltx")` и все
`configs\nq\kinds\*.ltx` подключённой игры (`EditorGameContent` читает архивы
`database\*.xdb*` и loose `gamedata\**`), плюс расширения установленных модулей —
`<game>\modules\*\gamedata\configs\nq\kinds\*.ltx` и `<game>\mods\*\…` (оба корня, по
образцу `EditorGameModes::ScanInstalledModules`); иначе — бандл
`<sdk>\gamedata\configs\nq\catalog.ltx`. Кэш до `Invalidate()` (смена линка).
`title` — cp1251 → UTF-8 через `Cp1251ToUtf8` (`EditorGameModes.cpp:17-31`).
`quest_catalog` и тулбар показывают источник (`game`/`bundled`) и `version`.

### 13.3 Чтение/запись (`NqLua`)

- Чтение через C API LuaJIT (уже слинкован: `cmake/EditorTargets.cmake:118-126`,
  `lua51.dll` стейджится): `luaL_newstate` без открытия библиотек → `luaL_loadbuffer`
  → пустое окружение (`lua_newtable; lua_setfenv`) → `lua_pcall` → обход результата
  (`lua_next`) в `SNqValue`. Ошибка синтаксиса/не-таблица → сообщение с
  строкой:колонкой из Lua. Один `lua_State` на операцию, закрывается сразу.
- Проверка синтаксиса кастомного `code`: `luaL_loadbuffer` chunk без выполнения (тем
  же путём) — предупреждение W050 «синтаксическая ошибка Lua».
- Запись — канонический сериализатор: порядок ключей квеста
  `nq, id, title, activation, vars, tasks, nodes`; ноды — в устойчивом топологическом
  порядке (обход от триггеров в порядке файла, по пинам в порядке каталога, по целям в
  порядке `out`; недостижимые — в конце в прежнем порядке); ключи ноды
  `id, kind, once, params, cond, on_enter, on_exit, out, pos, comment`; строки в `"…"` с
  экранированием, многострочные (`code`, длинные тексты) — `[[…]]`/`[==[…]==]`;
  отступ 2 пробела; `out` — строка при одной цели, список при нескольких. Файл всегда
  начинается с генерируемого блока комментариев: `-- nq: <id> "<title>"` и outline
  графа (§4.5, п.5) — единственные комментарии, которые пишет редактор; при чтении
  они игнорируются и пересобираются при каждом сохранении.
- Outline (§4.5) генерирует та же функция для писателя, `quest_get` и `quest_list`.

### 13.4 Документ и undo (`NqDoc`)

- `NqDoc { path, quest, dirty, undo/redo (кольцо снимков SNqQuest, 200), selection,
  view {cx, cy, zoom_idx} }`. Каждая мутация — через `NqDoc::Apply(op)` (те же операции,
  что MCP `quest_apply`, §13.11) → снимок в undo → перевалидация (инкрементальная не
  нужна: граф маленький).
- Открытые документы — реестр `UIQuestGraph::Docs()`; путь = ключ. Повторное
  открытие — фокус на вкладку.
- `Ctrl+Z/Y`, `Ctrl+S`, `Del`, `Ctrl+C/V/A` обрабатывает окно документа, когда оно в
  фокусе (`ImGui::IsWindowFocused(RootAndChildWindows)`); сценовые команды
  `COMMAND_UNDO/REDO` при этом не срабатывают, сценовый undo-стек не трогается.
- Внешнее изменение файла (ИИ переписал): кнопка/команда `Reload`; при сохранении
  редактор сравнивает mtime и, если файл менялся снаружи после открытия, спрашивает
  (UI) / перезаписывает (MCP с `force`).

### 13.5 Валидатор (`NqValidate`)

Один список правил для редактора и рантайма (коды §15). Возвращает
`SNqProblem { severity, code, node_id, slot ("enter:2"/"exit:0"/"cond:1"/"param:npc"), message }`.
Ошибки блокируют билд; предупреждения — нет.

### 13.6 Пикеры игровых данных (`NqPickers`)

Индексы строятся лениво из подключённой игры, кэш в `_appdata_\nq_index_<hash>.ltx`
(проектные источники — `story_id` из `custom_data` и весь `restrictor` — идут мимо кэша):

| Тип | Источник |
|---|---|
| `item_section` | `pSettings` (уже игровые, `EditorGameConfigs`), секции с `inv_name` |
| `squad_section` | `pSettings`, секции с `class = ON_OFF_S` |
| `level` | `configs\game_levels.ltx` |
| `smart` | секции `configs\misc\simulation.ltx` (имена) + позиции/уровни из скана `all.spawn` (ниже) |
| `story_id` | два источника: (1) `pSettings` — секции с ключом `story_id` (в DA так объявлены уникальные NPC: `configs\creatures\spawn_sections_*.ltx`, напр. `esc_2_12_stalker_wolf`, `esc_2_12_stalker_trader`; `story_objects.check_spawn_ini_for_story_id` читает `system_ini():r_string_ex(section,"story_id")`), это основной и дешёвый; (2) скан `spawns\all.spawn` подключённой игры (`[story_object] story_id` в `custom_data` — рестрикторы, аномальные споты) через `XrSE_Factory`, кэш в `_appdata_`; плюс `[story_object]` из `.xspawn` текущего проекта. Без `all.spawn` — только (1) и проектные, W060 не выдаётся |
| `profile` | `system.ltx [profiles] specific_characters_files` → `character_desc_*.xml` (id + name через string table) |
| `community` | `configs\creatures\game_relations.ltx [communities]` |
| `info` | `[info_portions] files` → `gameplay\info_*.xml` |
| `spot_type` | фиксированный список ядра |
| `restrictor` | **только проект**: `rawdata\levels\<уровень>\spawn.part` (обычный ltx-текст), объекты со спавн-секцией `space_restrictor` — берётся имя объекта (ключ `000003`), потому что рантайм ищет зону по ИМЕНИ (`db.zone_by_name`), а не по story id. Строится мимо кэша, так что только что поставленный рестриктор виден сразу; `extra` = уровень |
| `quest_id`/`task_id`/`var_name`/`ref_name`/`node id` | из открытого документа и других `.nqasset` проекта |

Пикер = комбо с поиском; всегда допускает ручной ввод (значение вне индекса — W060).
`place` имеет кнопку «взять из вьюпорта»: текущий уровень сцены + точка под курсором /
позиция выделенного объекта. `npc_ref` имеет вкладку «объекты сцены» (спавн-поинты,
помеченные как контент мода): выбор объекта без story id предлагает записать
`[story_object] story_id = nq_<mod>_<имя>` в его `custom_data` (сцена становится dirty).

### 13.7 Контент-браузер и окно

- `UIContentBrowser.cpp`: предикат `IsQuestExt(".nqasset")`; иконка/подпись
  «Quest Graph» в плитке; `OpenAsset` (`:846-959`) → `UIQuestGraph::Open(path, err)`;
  `DrawGridContextMenu` (`:2113-2151`) получает подменю **Create → Quest Graph** (только
  `m_Source == 0`), создание по образцу `CreateFolder` (`:2242-2284`): свободное имя
  `NewQuest.nqasset`, `NewQuest2.nqasset`, шаблон = минимальный квест
  (`trigger.start → flow.end`), `BeginRename` — **обобщается на файлы**
  (сейчас folder-only, `:2286-2359`): переименование файла = `MoveFileA` +
  синхронизация `id` внутри ассета не делается (id и имя файла независимы; id правится
  в инспекторе, W061 если не совпадают — только подсказка).
- Окно: класс `UIQuestGraphWindow : XrUI` на документ, `ImGui::Begin("Quest: <имя>##<path>")`
  с `ImGuiWindowFlags_MenuBar`, докается вкладкой в центр (`DockCenter`), пампится из
  `CLevelMain::OnDrawUI()` (`UI_LevelMain.cpp:3322-3341`) через `UIQuestGraph::Update()`.
  Внутри — тулбар, сплиттер канвас|инспектор (как `UIContentBrowser::DrawSplitter`),
  строка проблем.
- Из `Draw()` ничего, что перерисовывает кадр (правило `CLAUDE.md`, `XrUIManager::InUIPass()`).

### 13.8 Канвас (`NqCanvas`) — по скетчу

Собственная реализация на `ImDrawList` (в дереве нет нодовых библиотек, ImGui 1.88
docking в `Source/Editors/XrEUI/`; сторонние (imnodes/imgui-node-editor) не дают
вертикальный поток и версионно капризны).

```
┌ Quest: wolf_debt ─────────────────────────────────────┬─ Нода ─────────────────┐
│ [Сохранить][Проверить][Разложить][Кадр][100%▾][rus▾]  │ id    [meet          ] │
│                                                       │ вид   [Диалог: тема ▾] │
│        ( T · старт )                                  │ npc   [Сидорович  …  ] │
│             │                                         │ text  [Слышал, у тебя…]│
│    ┌────────▼───────────┐                             │ once  [x]              │
│    │ 1 │ 2 │ 3 │ +      │  ← on_enter                 │ условия  [+]           │
│    ├────────────────────┤                             │ комментарий [        ] │
│    │ Диалог: тема       │  ← основное действие        ├─ Действие (enter #2) ──┤
│    │ Сидорович: «Слышал…│                             │ вид  [Метка на карте▾] │
│    ├────────────────────┤                             │ target [Сидорович  … ] │
│    │ 1 │ 2 │ +          │  ← on_exit                  │ text   [Сидорович    ] │
│    └──┬────────────┬────┘                             │                        │
│    next│           │done                              │                        │
│  ┌─────▼────┐  ┌───▼─────┐                            │                        │
│  │ …        │  │ …       │                            │                        │
│ ▸ Проблемы: 0 ошибок, 1 предупреждение                │                        │
└───────────────────────────────────────────────────────┴────────────────────────┘
```

- **Бесконечный канвас**: мировые координаты float; трансформ `screen = (world - center) * zoom + origin`.
  Панорамирование — зажатая **ПКМ** (как в UE) и СКМ; клик ПКМ без движения — контекстное меню.
- **10 уровней зума**: `{0.2, 0.3, 0.4, 0.5, 0.65, 0.8, 1.0, 1.25, 1.5, 2.0}`, колесо
  меняет уровень на ±1 с якорем под курсором; `Home` — кадрировать всё, `F` — выделение.
  LOD: при зуме ≤ 0.4 чипы не рисуются (только полоски-счётчики), тексты сокращаются.
- **Поток сверху вниз**: вход — верхняя грань ноды (одна точка, принимает N рёбер),
  выходы — нижняя грань, по одному пину на имя из каталога, подписи под нодой; рёбра —
  вертикальные кубические безье со стрелкой.
- **Нода**: верхняя полоска чипов `on_enter` («1 | 2 | 3 | +»), тело (заголовок вида,
  строка-сводка ключевых параметров: NPC/текст/секция…), нижняя полоска `on_exit`
  («1 | 2 | +»). Цвета по семейству вида (dialog/objective/wait/flow/end), триггер — овал
  «T · <вид>». Бейдж ошибки/предупреждения; недостижимая нода — приглушена; ноды с
  `once` — метка ↻̸.
- **Взаимодействие**: ЛКМ — выбор/перетаскивание (всё выделенное), рамка на пустом
  месте, `Ctrl`+клик — переключение; перетаскивание из пина в тело/вход другой ноды —
  ребро; бросок на пустое место — меню «создать ноду и соединить»; `Alt`+клик по ребру
  или выделение ребра + `Del` — удалить; `Del` — удалить ноды (undo, без модалки);
  `Ctrl+Z/Y`, `Ctrl+C/V` (буфер = Lua-фрагмент `return { nodes = {…} }`, вставка с
  дедупом id и смещением), `Ctrl+A`, `Ctrl+S`, `F2` — переименовать id; двойной клик по
  чипу — фокус в нижний инспектор; «+» — меню доп. действий (каталог + Lua);
  перетаскивание чипов меняет порядок; ПКМ по чипу — удалить/дублировать.
  Привязка к сетке 16 px по отпусканию.
- **Навигация**: `Ctrl+F` — поиск по всему содержимому нод (id, вид, параметры,
  условия, действия, связи, комментарии), `F3`/`Shift+F3` — следующий/предыдущий
  результат; `Ctrl+B` — transient-закладка выделенных нод; `Alt+Left/Right` — назад/
  вперёд по transient-истории вида (центр, зум, выделение, слот инспектора). Миникарта
  показывает весь граф и прямоугольник viewport; клик/drag по ней панорамирует канвас.
  Поиск, закладки, история и видимость миникарты не сериализуются и не меняют dirty/undo.
- **Разложить** — детерминированная pin-aware раскладка сверху вниз: ветви
  следуют визуальному порядку пинов; полные границы вложенных поддеревьев
  разделяются малым зазором, а целых деревьев триггеров — увеличенным. Общий
  потомок нескольких ветвей становится единым merge-блоком между ними и не
  смещает центр эксклюзивной части ни одной ветви или триггера.
- Состояние вида (центр, зум) — в `NqDoc`, не в файле.

### 13.9 Инспектор (`NqInspector`)

Верхняя секция — **Нода** (или **Квест**, если ничего не выделено): `id`, вид
(комбо основных видов по группам; смена вида — сброс параметров, undo), генерируемые
по каталогу виджеты параметров (тип → виджет/пикер), `once`, `cond` (список строк:
`[not] [вид ▾] параметры… [×]`, `+ условие`, для `wait.any/flow.branch` — редактор
`cases`), комментарий. Для квеста: `id`, `title`, `activation`, таблица `vars`,
таблица `tasks`. Нижняя секция — **Действие**: выбранный чип — вид (доп. действия по
группам + «Lua»), параметры или многострочный редактор кода с кнопкой «Проверить»
(§13.3). Строка проблем под канвасом раскрывается в список; клик — фокус на ноду/слот.

### 13.10 Экспорт и билд

- В `EditorMod::Export` перед копированием (после гейта режима, `EditorModManifest.cpp:855-862`)
  — `NqExport::Validate(project_root, err)`: находит все `*.nqasset` проекта (кроме
  source-only), парсит, валидирует, проверяет уникальность `id` в модуле; при ошибках —
  `err = "quest graph errors: N (see log)"` + построчный лог, билд **отказывает** (как
  «no target game mode»); MCP получает тот же `err`.
- Ассеты копируются как есть (уже общий путь копирования папок); никаких
  сгенерированных файлов.

### 13.11 MCP

C++ — новые ветки в `XFinedInspector` (`UI_LevelMain.cpp:1356-2861`) через `NqMcp.cpp`;
Python — записи в `TOOLS` + `CMD_MAP` (`tools/mcp/xfined_mcp.py`), таблица в
`tools/mcp/AI_SETUP.md`. Аргументы-структуры передаются **строкой с Lua-таблицей**
(парсер уже есть — `NqLua`), ответы — JSON строками-эмиттерами (без JSON-библиотеки).

| Тул | Аргументы | Ответ |
|---|---|---|
| `xfined_quest_catalog` | — | виды с группами, `use`, параметрами (тип/обяз./дефолт/enum), пинами; версия каталога; источник (game/bundled) |
| `xfined_quest_list` | — | все `.nqasset` проекта: путь, id, title, ошибок/предупреждений |
| `xfined_quest_new` | `path` (относит. проекта) | создаёт из шаблона, открывает вкладку |
| `xfined_quest_open` / `xfined_quest_close` | `path`[, `discard`] | |
| `xfined_quest_get` | `path` | `{ lua: "<канонический текст>", outline: "<текстовый outline, §4.5>", problems: [...], stats }`; работает и для неоткрытого файла (читает с диска) |
| `xfined_quest_write` | `path`, `lua` | заменить документ целиком (парс → валидация → undo-снимок); проблемы и outline в ответе; файл не обязан быть открыт (тогда пишется на диск после валидации, ошибки не блокируют запись — блокирует только билд) |
| `xfined_quest_undo` / `xfined_quest_redo` | `path` | undo/redo документа (сценовый `undo` не трогает квесты) |
| `xfined_quest_apply` | `path`, `ops` = Lua-таблица операций: `add_node{node}`, `set_node{id, patch}`, `rename_node{id,new_id}`, `remove_node{id}`, `connect{from,pin,to}`, `disconnect{from,pin,to}`, `add_action{id,slot,index,action}`, `set_action{id,slot,index,patch}`, `move_action{...}`, `remove_action{...}`, `set_quest{patch}`, `set_pos{id,x,y}` | проблемы после применения |
| `xfined_quest_validate` | `path` | список проблем |
| `xfined_quest_save` | `path`[, `force`] | |
| `xfined_quest_reload` | `path` | перечитать с диска |
| `xfined_quest_layout` | `path` | авто-раскладка |
| `xfined_quest_view` | `path`, `frame:"all"\|<node>`, `zoom_level`, `cx`/`cy` (перебивают `frame`), `select:"<node>"\|"a,b"\|"none"` (выделение — инспектор идёт за ним), `slot:"enter:0"\|"exit:1"\|"none"` (открыть действие в инспекторе) | итоговые `zoom_level`/`center`/`selected`/`slot`; raw-ответ помечает отложенный первый `frame:all` как `pending:true`, Python bridge дочитывает вид после следующего Draw; затем `xfined_screenshot_editor` |
| `xfined_quest_edit` | `path`, `op`, `nodes` (кому вместо текущего выделения), `from`/`pin`/`to` | все правки графа, которые умеет канвас, без мыши: `connect`, `disconnect` (без `to` — весь пин, без `pin` — все пины), `bypass` (нода остаётся на месте, цепочка смыкается через неё: A→B→C становится A→C), `delete`, `duplicate`, `connect_nearest`, `select_all`. Ответ содержит outline — результат читается без скриншота |
| `xfined_quest_find` | `path`, `action`, `query`, `select`, `limit` | полнотекстовые результаты в порядке файла, активный результат и состояние вида; `get` с `query` атомарно меняет запрос без навигации, `next`/`previous` фокусируют ноду |
| `xfined_quest_project_find` | `query`, `limit` | read-only поиск по прямому отсортированному скану всех `*.nqasset`, не открывающий документы; грязные документы — memory-overlay. Ответ: `source` на результате, `complete`/`diagnostics`, cache `generation` и детерминированный content `fingerprint` |
| `xfined_quest_references` | `type`, `value`, `path?`, `limit?` | точные ссылки только по типам текущего каталога, включая вложенные `cond_list`/`cases_cond`; неизвестное, malformed, Lua и нечитаемые области явно делают результат неполным |
| `xfined_quest_rename_task` | `path`, `from`, `to`, `ack_runtime_identity:true` | безопасное переименование task внутри документа: preflight всех каталог-типизированных областей, затем один `Snapshot` + одна мутация объявления/ссылок. При неизвестном/Lua/malformed/partial отказывает до снимка; ack обязателен из-за runtime/save identity |
| `xfined_quest_bookmarks` | `path`, `action`, `node` | transient-закладки (`get/add/remove/toggle/next/previous/clear`) и состояние вида |
| `xfined_quest_history` | `path`, `action` | transient-история (`get/back/forward/clear`) со всеми состояниями вида |
| `xfined_quest_minimap` | `path`, `action` | видимость, границы графа/viewport и состояние вида (`get/show/hide/toggle`) |
| `xfined_quest_lookup` | `type`, `query`, `limit` | данные пикеров (§13.6) |
| `xfined_quest_check_all` | — | то же, что гейт билда |

Правило деструктивности: MCP-команды не показывают диалогов; `quest_close` без
`discard` при dirty возвращает ошибку `unsaved`, `quest_write` перезаписывает
документ (undo есть).

`NqProjectIndex` не вызывает `NqDocs::Open`: он читает отсортированный список
файлов напрямую и повторно использует разбор только при совпадении точного хеша
содержимого. Открытый **dirty** документ накладывается на соответствующий файл
read-only; чистый документ не скрывает внешнюю правку на диске. Fingerprint
строится из нормализованных путей, полного content hash, источника и статуса
разбираемости, поэтому одинаковый снимок проекта даёт одинаковое значение.

`NqReferences` не выводит смысл из имён `task`, `id` и т. п.: ссылка существует
только когда активный каталог объявил конкретный параметр как `task_id` (или
другой запрошенный тип). Для `cond_list` и `cases_cond` вид каждого вложенного
условия снова берётся из каталога. Неизвестный kind/параметр, неподходящая форма,
кастомный Lua либо частично прочитанный файл означают неполное доказательство.
Поэтому rename task сначала прогоняет весь документ на копии, и лишь при нуле
диагностик создаёт один undo-снимок и меняет declaration + точные ссылки. UI
ставит Cancel безопасной кнопкой по умолчанию и явно предупреждает, что task id
уже записан в runtime/save state; существующее состояние не мигрирует.

---

## 14. Кастомный Lua

- Где: доп. действие `{ kind = "lua", params = { code = [[…]] } }` и условие
  `{ kind = "lua", params = { code = [[ return … ]] } }`. Основные действия — никогда.
- Компиляция: `loadstring(code, "<uid>/<node>/<slot>")` один раз при загрузке квеста;
  ошибка компиляции — ошибка валидации (квест не загружается, E050 с позицией).
- Окружение: `setfenv(fn, env)`, `env = setmetatable({ nq = ctx_api }, { __index = _G })` —
  полный доступ к API игры на чтение и вызовы, запись глобалов уходит в `env` (не в `_G`).
- `nq` в окружении: `nq.quest`, `nq.node`, `nq.actor` (= `db.actor`), `nq.npc`
  (партнёр диалога или nil), `nq.vars` (прокси на переменные, запись = `var.set`),
  `nq.ref(name)` → серверный объект, `nq.ref_go(name)` → игровой объект или nil,
  `nq.signal(name)`, `nq.news(utf8_text)`, `nq.tr(utf8)` → cp1251, `nq.log(...)`,
  `nq.give_item(section, n)`, `nq.take_item(section, n)`, `nq.has_item(section, n)`,
  `nq.money(delta)`, `nq.time()` (игровые секунды), `nq.done(node_id)`.
- Ошибка выполнения — как у любого действия (§5.4); условие с ошибкой = `false`.
- Тексты внутри кода: автор пишет UTF-8, использует `nq.tr`/`nq.news`.

---

## 15. Валидация (общий список правил)

`E` — ошибка (билд/загрузка отклоняются), `W` — предупреждение.

| Код | Правило |
|---|---|
| E001 | `nq` отсутствует/не поддерживается рантаймом |
| E002 | `id` квеста не `[a-z0-9_]` или дублируется в модуле/проекте |
| E003 | файл не является одной таблицей / содержит код вне строк |
| E004 | `id` ноды не `[a-z0-9_]` или дублируется |
| E005 | неизвестный `kind` (для данной позиции: main/trigger/extra/cond) |
| E006 | обязательный параметр отсутствует / неверный тип / значение вне enum |
| E007 | пин не объявлен у вида; цель ребра не существует; ребро ведёт в триггер |
| E008 | нет ни одного триггера |
| E009 | `flow.join` без входящих рёбер |
| E010 | нарушено чередование говорящих в диалоге (нода `dialog.*_phrase` на глубине неверной чётности хотя бы по одному пути; или недостижима из темы) |
| E011 | фразовая нода имеет вход не из фразы/темы (вход из мира) |
| E020 | событийное условие (`event.*`) в `cond` фразы/темы или в `cases` `flow.branch` (допустимо только в `trigger.when` / `wait.when` / `wait.any`) |
| E021 | `cases` пусты / имена кейсов дублируются / `weight <= 0` |
| E030 (подзадачи) | `objective` называет шаг, которого нет у указанной `task`, либо рядом нет самой `task` |
| E022 | у вида выбрано несколько взаимоисключающих форм параметров сразу, либо не выбрано ни одной (`objective.fetch`: `item` против `section`/`count`/`from`; `objective.kill_count`: больше одного фильтра). Формы объявлены в `NqCatalog::ExclusiveForms` — одна таблица на валидатор и на инспектор, который гасит проигравшие поля |
| E030 | `task`/`quest`/`ref`/`var`/`node` ссылка на несуществующее объявление |
| E031 | `actor.teleport` на другой уровень |
| E050 | синтаксическая ошибка кастомного Lua |
| W011 | продолжения не-листовой фразы: среди ответов игрока безусловны не все (появится скрытый выход мимо `done`), либо среди реплик NPC нет ни одной безусловной (появится авто-ответ). Безусловная = без `cond` и без `once` |
| W012 | у `dialog.topic` нет ни одной фразы-продолжения |
| W013 | тема с `once=false` без условий — будет предлагаться бесконечно |
| W020 | недостижимая нода |
| W021 | ожидающая нода без исходящих рёбер (квест зависнет здесь) |
| W022 | одна цель дважды указана на одном пине; повторное ребро игнорируется |
| W030 | `ref` используется до ноды, которая его создаёт (не по всем путям) |
| W031 | `spawn.squad` без `hold` — отряд может уйти в симуляцию |
| W040 | символ вне cp1251 в тексте |
| W050 | синтаксис Lua не проверен (нет LuaJIT) — только редактор |
| W060 | значение не найдено в индексе игры (story id/секция/уровень/смарт/профиль/инфопоршень) — только редактор |
| W061 | имя файла не совпадает с `id` квеста — только редактор |
| W070 | квест без `flow.end` (завершится неявно) |

Рантайм печатает те же коды в лог; редактор — в строку проблем и MCP.

---

## 16. Тест-план

**Редактор (headless, `LevelEditor.exe -project D:\XFinedProjects\Test -nodlg` + MCP):**
1. `quest_new` → файл создан, каноничен, `quest_get` = шаблон, 0 ошибок.
2. `quest_write` эталона (§4.4) → 0 ошибок; повторный `quest_get.lua` байт-в-байт равен
   (идемпотентность канонизации).
3. `quest_apply` набора операций → ожидаемые проблемы (E007 при битом ребре, E010 при
   нарушении чередования, E020, W011 …).
4. `quest_catalog` содержит все виды из бандла; при линке — из игры.
5. `mod_export` с ошибочным квестом → `ok:false`, текст ошибки; с валидным → файл в
   `<game>\modules\<id>\…\*.nqasset` идентичен проектному.
6. UI-дым: `quest_open` → `screenshot_editor` (вкладка, ноды, чипы, рёбра); `quest_view`
   с зумом 0.2 и 2.0; `quest_layout` не меняет семантику (`quest_get` без `pos` равен).
7. Undo: `quest_apply` ×3 → `quest_undo` ×3 → исходный текст; `quest_redo` ×3 → конечный.
8. AI-цикл без UI: `quest_write` файла, которого нет на диске → создан; `quest_get`
   неоткрытого файла → outline и проблемы совпадают с открытым; `quest_list` даёт сводку.

**Игра (QA-скрипт по §12.5, три эталонных квеста из `docs\nq\examples`):**
1. `linear_fetch`: старт → задание → взять предмет → награда → `flow.end`; проверка
   `nq state` на каждом шаге через `nq fire`; сейв/лоад посередине → токен на месте.
2. `dialog_branching`: темы у Сидоровича, две реплики игрока, ветка NPC по `var`;
   проверка `xms.dialogs_for` (список тем), авто-ответ при всех-условных
   продолжениях, `once`.
3. `parallel_triggers`: `trigger.start` + `trigger.when{event.item_taken}` +
   `objective.kill{spawn}`; отряд создан, `hold` работает после лоада; смерть отряда
   оффлайн (`nq`-команда релиза) → `done`.
4. Смена уровня туда-обратно: состояние и рефы сохранены; `scripted_target`, story id
   рефов и start_dialog переставлены.
5. Мод удалён → сейв грузится, лог без `!`; мод возвращён → квест продолжается.
6. Изменённый граф (нода удалена) → токен снят с логом, остальное живо.
7. Ноль модулей с квестами → в логе нет `[nq]`-регистрации коллбеков (нулевая стоимость).
8. Невалидный ассет (код вне таблицы, битый пин) → квест `INVALID`, игра живёт.
9. `.scov`: после лоада и немедленного сейва блоб NQ присутствует (перестейдж).

---

## 17. План внедрения

Правило работы: каждая фаза — отдельные субагенты (по правилам `CLAUDE.md`), после
фиксации контракта игровые и редакторские фазы идут **параллельно**. Порядок
коммитов — по фазам, каждая проверяется своим DoD.

| Фаза | Репо | Что делаем | Кому | DoD |
|---|---|---|---|---|
| **0. Контракт** | оба | из этого дока выделить `docs\dead-air\NQ_RUNTIME.md` (игра) и `docs\nq\NQ_FORMAT.md` (редактор, формат+каталог+валидация); написать `configs\nq\catalog.ltx` v1 (игра) и его копию в `<sdk>\gamedata\configs\nq\`; 3 эталонных `.nqasset` в `docs\nq\examples\` | 1 агент | оба репо содержат одинаковый каталог; примеры проходят ручную проверку по §15 |
| **1. Движок** | игра | правки §12.3 (#1–#8), сборка `tools\build\build_x64.ps1` incremental, дым: `nq` команда отвечает, `xms.list_files/read_file` видят тестовый модуль | 1 агент | инкрементальная сборка чистая; ваниль без модулей — ноль новых логов |
| **2a. Рантайм ядро** | игра | `xms_nq.script`, `xms_nq_load`, `xms_nq_util`, `xms_nq_kinds` (flow.*, wait.*, trigger.*, extra: item/money/var/info/signal/news/quest, cond уровневые+событийные), персистентность, консоль | 1 агент (параллельно с 2b) | тест-план игры п.1, 6, 7, 8, 9 |
| **2b. Рантайм диалоги/цели/задания** | игра | `xms_nq_dialog`, `xms_nq_task`, виды `dialog.*`, `objective.*`, `spawn.*`, `squad.*`, `map.*`, `relation.*` | 1 агент | п.2, 3, 4, 5 |
| **3. Редактор ядро** | редактор | `Nq*` в `XrECore`: модель, `NqLua` (C API), каталог, валидатор, пикеры (индексы), `NqDoc`+undo, `NqExport` гейт, `NqMcp` + python + `AI_SETUP.md`; шаблон создания | 1–2 агента (IO/валидатор/каталог ↔ пикеры/MCP/экспорт), параллельно с 1–2 | тест-план редактора п.1–5, 7 (без UI) |
| **4. Редактор UI** | редактор | `UIQuestGraph`, `NqCanvas`, `NqInspector`, контент-браузер (Create/Open/rename файлов), тулбар, проблемы, горячие клавиши, `quest_view/layout` | 1 агент после 3 (модель/ops стабильны) | п.6; ручная проверка по скетчу |
| **5. Интеграция и QA** | оба | e2e: MCP строит 3 эталона → `mod_export` → игра по §12.5; фиксы | 1 агент | все пункты §16 зелёные |
| **6. Документация** | оба | `MODDING.md` (раздел NQ, ссылка на `NQ_RUNTIME.md`), `MOD_QUICKSTART.md` (раздел «Квесты»), `AI_SETUP.md`, README-строки; память проекта | 1 агент | доки описывают то, что собрано |

Риски и как закрыты:

| Риск | Закрытие |
|---|---|
| Хук диалогов ведёт себя не так в каком-то UI-пути (торговля, лечение) | хук только добавляет id; темы всегда с precondition корня = наличие токена; тест п.2 на трейдере |
| Ассерт «No available phrase» | авто-продолжение (§7.1) + W011 |
| Состояние NQ выпадает из сейва | перестейдж на первом апдейте + dirty (§11), тест п.9 |
| `xr_meet` перебивает start_dialog | переустановка раз в тик пока токен есть |
| Каталог игры и редактора разъехались | источник — игра; бандл только фоллбэк; `quest_catalog` показывает источник и версию |
| ИИ пишет невалидные файлы | одинаковые коды ошибок с обеих сторон, `quest_write` возвращает проблемы, игра не падает |
| Коллизии id между модами | uid = `<module>.<id>`; story id рефов и id диалогов/заданий с префиксом `nq.<module>.<quest>` |
| Смена уровня пересоздаёт Lua-стейт | `init` идемпотентен, всё восстанавливается из блоба + `refs` |
| ИИ не видит канвас и вынужден делать скриншоты | текстовый outline в `quest_get`/`quest_list` и в заголовке файла, `pos` необязателен, каталог по запросу, проблемы текстом (§4.5) |

---

## 18. Реестр решений (почему именно так)

| Решение | Альтернатива | Почему так |
|---|---|---|
| Интерпретация в игре, а не компиляция в редакторе | генерировать Lua/XML при билде | требование пользователя + фикс модов обновлением игры; редактор без движка не может проверить сгенерированный код |
| `.nqasset` = декларативный Lua (`return {…}`) | JSON/LTX/полноценный Lua-DSL | Lua читается ИИ и людьми, грузится одинаково в игре и редакторе (LuaJIT есть в обоих), пустое окружение делает его данными; JSON в игре парсить нечем, LTX плох для вложенности |
| Каталог в LTX + Lua-реализации через `xms.registry` | каталог в Lua | редактор без Lua-рантайма игры не выполнит Lua-каталог, а LTX читают обе стороны; модули расширяют его файлом в VFS |
| Мгновенные операции — только доп. действия; main = ключевые этапы | дублировать спавн/выдачу и в main | одна форма записи для одного дела; соответствует описанию пользователя («нода — ключевое действие») |
| Условия — каталог + `lua` | только каталог | триггеры без кастомных условий слишком узки; правило «кастом только в доп.» распространено на условия сознательно, main остаётся строгим |
| Фразы — отдельные ноды | диалог-редактор внутри ноды | пользователь: «нода = фраза»; графовая связь фраз с остальным квестом (ребро из фразы в мир) даёт нелинейность бесплатно |
| Токены + отложенная очередь | немедленные рекурсивные переходы | реентерабельность внутри `SayPhrase` и коллбеков движка; детерминизм; ассерт движка при фильтрации всех продолжений |
| Хук выдачи диалогов + виртуальные диалоги в движке | XML-заглушки от редактора / `character_dialogs.xml` | без правки движка нельзя добавить тему конкретному NPC динамически, а заглушки от редактора нарушают принцип интерпретации; правка ~100 строк и аддитивна |
| Собственный канвас на ImDrawList | imnodes / imgui-node-editor | их нет в дереве, обе горизонтальные, imnodes без зума, imgui-node-editor завязан на версию ImGui (у нас 1.88 с патчами); нам нужен вертикальный поток, чипы, 10 фиксированных зумов и MCP-управление видом |
| Один блоб `xms.nq` в `.scov` | блоб на модуль / `.scoc` через `save_state` | не воевать за блоб модуля с автором; `.scov` — единственный санкционированный канал (SAVE_COMPATIBILITY); удаление мода не ломает ничего |
| Задания через `CGameTask` напрямую | `task_manager` + ltx | `task_ini` читается один раз при загрузке скрипта, регистрации нет |
| Тексты сырым cp1251 | генерировать string table | `translate()` возвращает ключ; string table = лишний генерируемый файл, а мы их не генерируем |
| Флаги квеста — `vars`, а не инфопоршни | инфопоршни | инфопоршни не персистятся вне `.scop` актора и не нужны движку; `info.give` оставлен для интеропа с ванилью |
| Строковые story id | числовые `xms.story_id` | скрипты игры адресуют объекты строковыми id (`story_objects`), редактор пишет их в `custom_data` без правок движка |
| Скан модуля рекурсивно | список в манифесте | манифест из Lua не прочитать, лишняя точка рассинхрона; папка — единственный источник |
| Валидация — гейт билда | предупреждать и экспортировать | правило проекта: билд отказывает с внятным текстом; ИИ получает те же коды через MCP |
| Удаление нод без подтверждения | модалка | внутри документа есть undo; файл не тронут до сохранения; закрытие грязной вкладки спрашивает и называет ассет |
| Outline и генерируемый заголовок файла, `pos` необязателен | «граф читается только на канвасе» | требование пользователя: нейронка создаёт и правит квесты текстом, без скриншотов; outline — та же функция для писателя, `quest_get` и `quest_list`, поэтому не расходится |
| Канонический порядок нод — топологический | порядок создания | файл читается сверху вниз как история квеста; порядок устойчив, диффы не прыгают |
