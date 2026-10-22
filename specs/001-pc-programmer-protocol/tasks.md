---

description: "Task list for 001-pc-programmer-protocol"
---

# Tasks: Протокол обміну PC ⇄ програматор

**Input**: Design documents from `specs/001-pc-programmer-protocol/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md),
[contracts/protocol.md](contracts/protocol.md), [contracts/package-format.md](contracts/package-format.md),
[contracts/chip-engine.md](contracts/chip-engine.md), [quickstart.md](quickstart.md)

**Tests**: обов'язкові за принципом VI конституції — модульні тести ядра на PC, контрактні тести клієнта
проти симулятора `pdk-sim` з ін'єкцією помилок, задачі перевірки на залізі. У кожній історії тести пишуться
першими й мають падати до реалізації.

**Organization**: задачі згруповано за історіями користувача (US1…US6 зі spec.md).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: можна виконувати паралельно (інші файли, без залежності від незавершених задач)
- **[Story]**: історія користувача (US1…US6)

## Path Conventions

- Прошивка: `software/firmware/` (`core/`, `hal/`, `platform/stm32f072/`, `platform/host/`, `tests/`)
- Клієнт: `software/client/` (`padauk_prog/`, `tests/unit/`, `tests/contract/`)
- Спільні тестові вектори: `software/tests/vectors/`
- Усі байтові формати — строго за `contracts/protocol.md`, `contracts/package-format.md`; внутрішні
  інтерфейси — за `contracts/chip-engine.md`
- Кожен новий файл починається з `SPDX-License-Identifier: GPL-3.0-or-later`; код, перенесений з
  `easy-pdk-programmer-software`, зберігає копірайт free-pdk. Субмодулі не змінюються й не є залежністю збірки

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: каркас підпроєктів, збірка для двох платформ, тестові фреймворки

- [X] T001 Створити дерево каталогів за plan.md: `software/firmware/{core/{proto,package,storage,power,ops,chips},hal,platform/{stm32f072,host},tests,cmake}`, `software/client/{padauk_prog,tests/{unit,contract}}`, `software/tests/vectors/` (з `.gitkeep` у порожніх)
- [X] T002 Створити `software/firmware/CMakeLists.txt`: C11, `-Wall -Wextra -Werror`, опція `PLATFORM` (`host` | `stm32f072`), бібліотека `pdk_core` з `core/*`, для `host` — виконуваний `pdk-sim` і тести CTest, для `stm32f072` — `pdkprog.elf/.bin`
- [X] T003 [P] Створити тулчейн-файл `software/firmware/cmake/arm-none-eabi.cmake` (Cortex-M0, `-mcpu=cortex-m0 -mthumb -Os -flto -ffunction-sections -fdata-sections`, `--gc-sections`, `--specs=nano.specs`)
- [X] T004 [P] Створити мінімальний тестовий фреймворк `software/firmware/tests/pdk_test.h` (макроси `TEST`, `ASSERT_EQ`, `ASSERT_MEM_EQ`, реєстрація й запуск, ненульовий код виходу при падінні) і `software/firmware/tests/CMakeLists.txt`, що реєструє кожен `test_*.c` у CTest
- [X] T005 [P] Створити `software/client/pyproject.toml`: пакет `padauk_prog`, Python ≥ 3.10, залежність `pyserial`, extra `dev` = `pytest`, консольний скрипт `pdkprog = padauk_prog.cli:main`; `software/client/padauk_prog/__init__.py` з `__version__ = "0.1.0"`
- [X] T006 [P] Створити `software/client/tests/conftest.py` з опцією `--sim PATH` (шлях до `pdk-sim`) і пропуском контрактних тестів, якщо симулятор не задано
- [X] T007 [P] Додати в `.gitignore` кореня: `build/`, `software/client/**/__pycache__/`, `*.egg-info/`, `.pytest_cache/`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: кадрування, CRC, сесія, HAL, політика напруг, таблиця мікросхем, симулятор і базовий клієнт — потрібні всім історіям

**⚠️ CRITICAL**: жодна історія не починається до завершення цієї фази

### Тести фундаменту (писати першими)

- [X] T008 [P] Згенерувати спільні вектори `software/tests/vectors/frames.json` скриптом `software/tests/vectors/gen_frames.py` на `zlib`: CRC-32 (`"123456789"` → `0xCBF43926`, порожній рядок, 1 КБ псевдовипадкових байтів), COBS (порожній, `00`, `11 00 22`, 254 і 255 ненульових байтів, лише нулі), 6 кадрів (REQ/RSP/EVT/NAK) у hex до й після COBS; опція `--c-header PATH` генерує `software/firmware/tests/vectors_frames.h`
- [X] T009 [P] Модульні тести CRC-32 і COBS проти векторів у `software/firmware/tests/test_crc_cobs.c`
- [X] T010 [P] Модульні тести кадру в `software/firmware/tests/test_frame.c`: кодування/декодування всіх типів, відхилення неправильної довжини, CRC, `len > max_payload`, обрізаного COBS, ресинхронізація на наступному `0x00` після сміття
- [X] T011 [P] Модульні тести сесії в `software/firmware/tests/test_session.c`: дубльований `seq` → та сама відповідь без повторного виконання (лічильник викликів обробника = 1), `NAK` на пошкоджений кадр, команди до `HELLO` → `NO_SESSION`, `HELLO` з іншою мажорною версією → `PROTO_VERSION` + `INFO`, скидання сесії й незавершеного завантаження при зміні DTR (операція з мікросхемою не скасовується — FR-023), невідомий `code` → `UNKNOWN_COMMAND`
- [X] T012 [P] Модульні тести політики встановлення напруг у `software/firmware/tests/test_power.c` з фейковим `hal_power` (contracts/chip-engine.md §2, protocol §6.1): `power_off_all()` — перший крок `pdk_core_init()` до будь-якої іншої ініціалізації (зняття напруг після скидання, принцип III); мінімальна пауза етапу дотримується навіть при миттєвому встановленні; 3 послідовні виміри в межах ±2 % / ±30 мВ (більше з двох); тайм-аут 20 мс → `VDD_FAULT`/`VPP_FAULT` з `rail/set_mV/meas_mV` і викликом `power_off_all()`; 0 В при знятті підпадає під те саме правило
- [X] T013 [P] Модульні тести клієнтського кадрування в `software/client/tests/unit/test_framing.py` проти `software/tests/vectors/frames.json`
- [X] T014 [P] Модульні тести stop-and-wait клієнта в `software/client/tests/unit/test_reliability.py` з фейковим транспортом: повтор того ж `seq` після `NAK`, після пошкодженої відповіді, після тайм-ауту 300 мс; помилка сесії після 5 повторів; події між відповідями доставляються окремо

### Реалізація фундаменту — прошивка

- [X] T015 [P] Оголосити коди статусу, команд, подій, типів операцій, етапів, `warnings`, `site_status`, `STATUS.flags/selftest_state` з `contracts/protocol.md` §4–§6 у `software/firmware/core/proto/proto_defs.h` (enum-и з точними числовими значеннями; зарезервовані коди позначити коментарем) і `PDK_PROTO_MAJOR 1`, `PDK_PROTO_MINOR 0`
- [X] T016 [P] Реалізувати CRC-32/ISO-HDLC (півбайтова таблиця 16×u32, інкрементальний API `crc32_init/update/final`) у `software/firmware/core/proto/crc32.c` і `crc32.h`
- [X] T017 [P] Реалізувати COBS-кодер і потоковий декодер з ресинхронізацією в `software/firmware/core/proto/cobs.c` і `cobs.h`
- [X] T018 Реалізувати кодування/розбір кадру (`frame_encode`, `frame_parse` → `FRAME_ERROR`) у `software/firmware/core/proto/frame.c` і `frame.h` (залежить від T016, T017)
- [X] T019 [P] Оголосити інтерфейси HAL у `software/firmware/hal/` за `contracts/chip-engine.md`: `hal_transport.h` (read/write, `is_open()` = DTR, подія зміни стану), `hal_flash.h` (геометрія слотів і журналу, erase_page, program, read), `hal_crc.h` (апаратний CRC-32 з фолбеком на `core/proto/crc32.c`), `hal_power.h` (`power_set`, `power_measure`, `power_off_all`, `power_limits`), `hal_entropy.h` (сирі біти шуму, UID МК), `hal_ui.h` (`local_start(request)` як подія від платформи, `ui_state(state)`), `hal_time.h` (мілісекунди, мікросекунди), `chip_engine.h` (примітиви й 5 гарантій із §3, колбек `poll`)
- [X] T020 Реалізувати політику встановлення напруг `power_apply(rail, mV, min_pause_us)` за contracts/chip-engine.md §2 у `software/firmware/core/power/power.c` і `power.h` (залежить від T019)
- [X] T021 Реалізувати сесію (стан порту, `HELLO`, кеш останньої відповіді, лічильник подій, `NAK`, обмеження payload) у `software/firmware/core/proto/session.c` і `session.h` (залежить від T018, T019)
- [X] T022 Реалізувати диспетчер команд (таблиця `code → handler`, прапорець «дозволено під час операції», `BUSY`/`UNKNOWN_COMMAND`/`BAD_PAYLOAD`) і обробники `HELLO`, `GET_INFO` (структура `INFO` з contracts §4, біти можливостей) у `software/firmware/core/proto/dispatch.c` і `dispatch.h` (залежить від T021)
- [X] T023 Перенести таблицю мікросхем із `easy-pdk-programmer-software/fpdkicdata.c` (13 записів, з копірайтом free-pdk) у `software/firmware/core/chips/chip_table.c` і `chip_table.h`: публічна частина (`CHIP_DESC`) і внутрішня (напруги, мінімальні паузи етапів, параметри блоку запису, імпульси стирання, IO-адреси `IHRCR`/`ILRCR`/`BGTR`/`GPCC`/`GPCS` з `easy-pdk-programmer-software/Examples/src/easypdk/*.h`; невідомі — `0xFF`); пошук за ID і назвою/варіантом
- [X] T024 Реалізувати команду `LIST_CHIPS` з пагінацією за `first_index` у `software/firmware/core/chips/chip_list_cmd.c` і зареєструвати в диспетчері (залежить від T022, T023)
- [X] T025 Реалізувати головний цикл ядра `pdk_core_init()` (першим кроком — `power_off_all()` і лінії IC у Hi-Z) / `pdk_core_poll()` (транспорт → декодер → сесія → диспетчер → відправка) у `software/firmware/core/pdk_core.c` і `pdk_core.h` (залежить від T022)
- [X] T026 [P] Реалізувати хост-HAL: транспорт stdio і TCP (`--tcp host:port`, DTR = TCP-з'єднання), час, програмний CRC, ентропія з ОС у `software/firmware/platform/host/hal_host_transport.c`, `hal_host_misc.c`
- [X] T027 [P] Реалізувати емуляцію flash для хоста з геометрією F072 (сторінки 2 КБ, слоти `0x0800B000`/`0x0800D000`, журнал `0x0800F000`), файлом стану (`--flash FILE`), правилом «програмувати лише стерте», ін'єкцією обриву живлення після N-ї операції запису/стирання (`--power-fail-after N`, код виходу 99) і пошкодженням читання під час запису (`--corrupt-flash-during-write`) у `software/firmware/platform/host/hal_host_flash.c`
- [X] T028 [P] Реалізувати модель живлення для хоста (джерела VDD/VPP з постійною часу встановлення, шум вимірювання, несправності `--power-fault vdd:low|vpp:slow|vpp:stuck`, лічильник часу під напругою для перевірки SC-011) у `software/firmware/platform/host/hal_host_power.c`
- [ ] T029 Реалізувати `pdk-sim` (`main`: аргументи `--tcp`, `--flash`, `--chip`, `--chip-state`, `--power-fail-after`, `--power-fault`, `--corrupt-flash-during-write`, команди stdin `local-start program|copy|verify|selftest|restore`, `local-cancel`, `swap-chip SPEC`; цикл `pdk_core_poll`; вивід `ui_state` у stderr) у `software/firmware/platform/host/sim_main.c` (залежить від T025, T026, T027, T028)
- [X] T030 [P] Реалізувати платформу цілі налагодження STM32F072 (плати Easy PDK; основна МК — наступна специфікація): `startup.s`, лінкер-скрипт із розміткою flash з research R5 (`FLASH 44K`, символи `__slot0`, `__slot1`, `__journal`), тактування 48 МГц від HSE + HSI48 для USB у `software/firmware/platform/stm32f072/` (`startup.s`, `stm32f072c8.ld`, `system.c`)
- [X] T031 Інтегрувати TinyUSB (FetchContent з фіксованим тегом у `software/firmware/CMakeLists.txt`; CDC ACM, VID/PID `0x1209:0x0001`, дескриптори), транспорт HAL з DTR через `tud_cdc_connected()` у `software/firmware/platform/stm32f072/usb_descriptors.c`, `tusb_config.h`, `hal_f072_transport.c` (залежить від T030)
- [X] T032 [P] Реалізувати HAL для F072: flash (стирання сторінки, програмування півслів), апаратний CRC (`REV_IN`/`REV_OUT`), SysTick і мікросекундний таймер, ентропія (молодші біти АЦП VREFINT/датчика температури + UID 96 біт), живлення (ЦАП + АЦП VDD/VPP за схемою Easy PDK, активний розряд — через ЦАП = 0), заглушка `hal_ui` (кнопка PA15 → `local_start(PROGRAM)`, LED PC13–PC15) у `software/firmware/platform/stm32f072/hal_f072_flash.c`, `hal_f072_crc.c`, `hal_f072_power.c`, `hal_f072_misc.c`
- [X] T033 Реалізувати `main()` для F072 (на самому початку — ЦАП VDD/VPP = 0 і лінії IC у Hi-Z до тактування й USB; потім ініціалізація, TinyUSB task + `pdk_core_poll`) та заглушку `chip_engine` (усі примітиви → `OP_UNSUPPORTED`, `safe_state` = `power_off_all()` + лінії IC у Hi-Z) у `software/firmware/platform/stm32f072/main.c`, `chip_engine_stub.c` (залежить від T031, T032)

### Реалізація фундаменту — клієнт

- [X] T034 [P] Реалізувати CRC/COBS/кадри в `software/client/padauk_prog/framing.py` (`zlib.crc32`, потоковий декодер COBS)
- [X] T035 [P] Реалізувати транспорти `serial` (pyserial, DTR=1) і `tcp://host:port` з автопошуком порту за VID/PID `0x1209:0x0001` у `software/client/padauk_prog/transport.py`
- [X] T036 Реалізувати `software/client/padauk_prog/protocol.py`: enum-и кодів з contracts §4–§6, клас `Programmer` (stop-and-wait, повтори, черга подій, `hello()`, `get_info()`, `list_chips()` з розбором `INFO` і `CHIP_DESC`), виняток `ProgrammerError(status, payload)` (залежить від T034, T035)
- [X] T037 Реалізувати TCP-проксі ін'єкції помилок `software/client/tests/contract/fault_proxy.py` (пошкодження біта, обрізання, дублювання, затримка кадрів в обох напрямах, детермінований seed, примусовий розрив з'єднання) і фікстури `pdk_sim`, `fault_proxy`, вимір латентності кожного RSP у `software/client/tests/contract/conftest.py` (залежить від T029, T036)

**Checkpoint**: `ctest` і `pytest tests/unit` зелені; бібліотека клієнта отримує `INFO` і `LIST_CHIPS` від `pdk-sim` і від F072

---

## Phase 3: User Story 1 — Завантаження пакета прошивки в програматор (Priority: P1) 🎯 MVP

**Goal**: клієнт формує пакет із HEX, передає його з гарантією цілісності; програматор перевіряє за своєю таблицею й атомарно зберігає, повертає CRC

**Independent Test**: завантажити пакет, «перезапустити» `pdk-sim` (той самий `--flash`), `PKG_INFO` → CRC, ім'я й заголовок збігаються з клієнтськими

### Tests for User Story 1

- [X] T038 [P] [US1] Згенерувати вектори пакетів `software/tests/vectors/packages/` скриптом `software/tests/vectors/gen_packages.py`: валідний PFS154 із 3 слів (еталон із package-format), валідний `helloworld_pfs154` із вставкою IHRC, валідний пакет зі вставкою серійного номера, пакет без калібрувальних вставок; по одному невалідному на кожен рядок таблиць перевірок `package-format.md` (зокрема: порожнє/некоректне `name`, ненульові зарезервовані біти й поля, `CREATED_BY_COPY` від клієнта, OTP-пакет PMS150C із `SKIP_BLANK_CHECK`, невідома вставка з 6+ `AND A`) з очікуваним кодом і `addr` у `manifest.json`
- [X] T039 [P] [US1] Модульні тести перевірки заголовка й комміту в `software/firmware/tests/test_package_validate.c` за `manifest.json` (кожен невалідний вектор → точний код і `addr`; `calib_sites`/`serial_sites` для валідних)
- [X] T040 [P] [US1] Модульні тести пошуку калібрувальних вставок і вставок серійного номера в `software/firmware/tests/test_sites.c`: шаблони для 13/14/15-бітних ядер, позиція будь-де в пам'яті, до 16 вставок, ≥ 6 невідомих `AND A` → `PKG_CALIB_UNKNOWN` з адресою, відсутність хибних збігів на прикладах із `easy-pdk-programmer-software/Examples/*.ihx`
- [X] T041 [P] [US1] Модульні тести журналу стану в `software/firmware/tests/test_journal.c`: запис 40 Б і CRC байтів 0–35, вибір чинного запису за `rec_seq`, ігнорування недописаного запису, переповнення сторінки й перехід на другу, обрив живлення під час стирання другої сторінки
- [X] T042 [P] [US1] Модульні тести завантаження в `software/firmware/tests/test_upload.c`: послідовні `PKG_DATA`, `PKG_OFFSET` з `expected`, розбіжність заголовка `PKG_DATA` і `PKG_BEGIN`, `PKG_ABORT`, обрив сесії, поступове стирання сторінок (≤ 1 сторінка на виклик), атомарний комміт
- [X] T043 [P] [US1] Модульні тести клієнта в `software/client/tests/unit/test_ihex_package.py`: розбір Intel HEX (байтові адреси → слова LE, маска заданих слів), ім'я за замовчуванням з імені файлу, побудова пакета побітово ідентична векторам T038
- [X] T044 [P] [US1] Контрактний тест `software/client/tests/contract/test_upload.py`: завантаження → `PKG_INFO`/`PKG_READ` побайтово збігаються; перезапуск симулятора зберігає пакет; кожен невалідний вектор → точний код; активний пакет не змінюється після відхилення; пакет без вставок → `calib_sites = 0` і попередження клієнта (FR-050)
- [X] T045 [P] [US1] Контрактний тест SC-001 `software/client/tests/contract/test_frame_corruption.py`: 1000 завантажень пакета `max_package_size` через `fault_proxy` з ≥ 1 % пошкоджених/втрачених/дубльованих кадрів у кожному напрямі; кожне завантаження → збережений пакет ідентичний вихідному
- [X] T046 [P] [US1] Контрактний тест SC-007 `software/client/tests/contract/test_power_loss.py`: ≥ 100 запусків `pdk-sim --power-fail-after N` з випадковим N посеред завантаження; після перезапуску `pkg_state = VALID` і CRC = старому або новому пакету
- [X] T047 [P] [US1] Контрактний тест `software/client/tests/contract/test_duplicate_seq.py`: повтор `PKG_DATA` із тим самим `seq` не дублює дані; повтор `PKG_COMMIT` не робить другого запису журналу
- [X] T048 [P] [US1] Контрактний тест FR-024 `software/client/tests/contract/test_latency.py`: кожен RSP ≤ 100 мс, окремо — `PKG_DATA` на межі сторінки flash, `PKG_COMMIT` пакета `max_package_size`, запис журналу з переходом на іншу сторінку

### Implementation for User Story 1

- [X] T049 [P] [US1] Реалізувати структуру й розбір заголовка (поля з package-format, `name`/`label` UTF-8, зарезервовані біти й поля = 0), перевірки рівня `PKG_BEGIN` (зокрема `SKIP_BLANK_CHECK` для OTP → `PKG_OTP_BLANK_CHECK_REQUIRED`, `CREATED_BY_COPY` від клієнта → `PKG_HEADER_INVALID`) у `software/firmware/core/package/header.c` і `header.h`
- [X] T050 [P] [US1] Перенести шаблони калібрувальних вставок (IHRC/ILRC/BG для 13/14/15 біт, вимірювальні цикли й пост-калібрувальні патчі з `easy-pdk-programmer-software/fpdkiccalib.c`, з копірайтом) і пошук вставок за research R8 у `software/firmware/core/chips/calib_sites.c` і `calib_sites.h`
- [X] T051 [P] [US1] Перенести шаблони вставки серійного номера (`RET 'F','P','S','E','R','I','A','L'` для 13/14/15/16 біт, з `fpdkicserial.c`) і пошук позицій у `software/firmware/core/chips/serial_sites.c` і `serial_sites.h`
- [X] T052 [US1] Реалізувати повну перевірку пакета при комміті (CRC з flash через `hal_crc`, службові області, fuse з останнього слова vs `FUSE_PRESENT`, біт захисту vs `ALLOW_SECURITY_FUSE`, розпізнаваність вставок; результат `calib_sites`, `serial_sites`, `effective_words`, код + `addr`) у `software/firmware/core/package/validate.c` і `validate.h` (залежить від T049, T050, T051)
- [X] T053 [P] [US1] Реалізувати слоти (адреси з `hal_flash`, вибір неактивного, поступове стирання сторінок під час запису, читання) у `software/firmware/core/storage/slots.c` і `slots.h`
- [X] T054 [P] [US1] Реалізувати журнал стану (`StateRecord` 40 Б з data-model, пошук чинного запису, дописування, переповнення сторінки, копіювання на другу сторінку) у `software/firmware/core/storage/journal.c` і `journal.h`
- [X] T055 [US1] Реалізувати `PackageState` (EMPTY/VALID/CORRUPTED) при старті й на запит у `software/firmware/core/storage/package_store.c` і `package_store.h` (залежить від T053, T054)
- [X] T056 [US1] Реалізувати автомат завантаження (IDLE/RECEIVING/COMPLETE) і команди `PKG_BEGIN`, `PKG_DATA`, `PKG_COMMIT`, `PKG_ABORT`, скидання завантаження при обриві сесії, подію `EVT_PKG_STATE` у `software/firmware/core/package/upload.c` і `upload.h` (залежить від T052, T055)
- [X] T057 [US1] Реалізувати команди `PKG_INFO`, `PKG_READ`, `PKG_DELETE`, `PKG_CHECK` і зареєструвати всі `PKG_*` у диспетчері в `software/firmware/core/package/pkg_cmds.c` (залежить від T056)
- [X] T058 [P] [US1] Реалізувати розбір Intel HEX (записи 00/01/02/04, контрольні суми рядків) і запис Intel HEX 8-біт у `software/client/padauk_prog/ihex.py`
- [X] T059 [US1] Реалізувати побудову пакета (Header 128 Б з `header_crc32`, `name` за замовчуванням з імені файлу, `label`, слова, маска, `package_crc32`, прапорці з опцій, перевірка розміру за `CHIP_DESC` до відправки) у `software/client/padauk_prog/package.py` (залежить від T058)
- [X] T060 [US1] Додати в `Programmer` методи `upload_package()` (BEGIN, DATA чанками ≤ `max_payload − 4`, COMMIT, порівняння CRC), `package_info()`, `package_read()`, `package_delete()`, `package_check()` у `software/client/padauk_prog/protocol.py` (залежить від T059)
- [X] T061 [US1] Реалізувати CLI `software/client/padauk_prog/cli.py`: глобальні `--port`, `-v`; `upload FILE -n NAME [--fuse --allowsecfuse --securefill --noerase --noblankchk --noverify --runvdd --name --label]`; відмова на `--nocalibrate` («приберіть макрос калібрування») і `--serial` («номер випадковий, автоматично»); відмова на `--noblankchk` для OTP за `CHIP_DESC.family`; `pkg info|read FILE|delete|check`; повідомлення для кожного `PKG_*` з `addr`; попередження при `calib_sites = 0` (залежить від T060)

**Checkpoint**: US1 працює самостійно на `pdk-sim`; T044–T048 зелені

---

## Phase 4: User Story 2 — Запис мікросхеми з PC з прогресом і результатом (Priority: P1)

**Goal**: `PROGRAM` — програматор виконує всі кроки (зокрема повторну спробу OTP, калібрування, фінальну перевірку), шле прогрес і підсумок; `VERIFY` з урахуванням вставок; `PATCH`; скасування; обрив USB не перериває запис

**Independent Test**: на `pdk-sim --chip PFS154` клієнт без таблиці мікросхем записує пакет і бачить усі етапи, прогрес ≥ 1/с, `RESULT` з калібруванням, випадковим серійним номером і сумами вмісту

### Tests for User Story 2

- [ ] T062 [P] [US2] Модульні тести ефективного образу в `software/firmware/tests/test_effective_image.c`: незадані слова, `SECURE_FILL` до службових областей, виключення fuse, вставка серійного номера (молодший байт першим), вставка калібрування → вимірювальний цикл → завершена форма (порівняння з опкодами `fpdkiccalib.c`); усі переходи «цикл → завершена» лише 1→0
- [ ] T063 [P] [US2] Згенерувати вектори відкаліброваних дампів `software/tests/vectors/footprints/` скриптом `software/tests/vectors/gen_footprints.py`: для 13/14/15 біт — завершені сліди IHRC/ILRC/BG, вимірювальні цикли з довільним `MOV A,#…`, сліди з «чужою» адресою регістра, запис константи в `IHRCR` поза слідом; очікувані відновлені вставки й нормалізовані суми в `manifest.json`
- [ ] T064 [P] [US2] Модульні тести детектора слідів і циклів у `software/firmware/tests/test_calib_footprint.c` за векторами T063: позиція, тип, `k`; розпізнавання вимірювального циклу; відновлення вставки побітово як у free-pdk; невідповідна адреса регістра → не слід
- [ ] T065 [P] [US2] Модульні тести сум вмісту в `software/firmware/tests/test_image_crc.c`: пропуск службових областей і `reserved_first_word`, обнулення бітів вище `code_bits`, нормалізація слідів і вставок серійного номера; дві мікросхеми з однаковою прошивкою, різним `k` і серійним номером → однакова `image_crc_norm`, різна `image_crc_raw`; сума з ефективного образу пакета = сумі вмісту моделі після `PROGRAM`
- [ ] T066 [P] [US2] Модульні тести генератора серійних номерів у `software/firmware/tests/test_serial_rng.c` з фейковим `hal_entropy`: 64 біти, різні значення для послідовних мікросхем і для різних UID при однаковій «ентропії», тест повторів/пропорцій сирих біт → відмова джерела = операція не починається (`INTERNAL`)
- [ ] T067 [P] [US2] Модульні тести менеджера операцій у `software/firmware/tests/test_op_manager.c` з фейковим рушієм: послідовність етапів `PROGRAM` (`PKG_CHECK → ID → ERASE → BLANK_CHECK → WRITE → VERIFY → CALIBRATE → PKG_CHECK → FUSE → FINAL_VERIFY`), пропуск за прапорцями, `EVT_PROGRESS` при зміні етапу і ≤ 500 мс, `OP_CANCEL` → `CANCELLED` + `safe_state`, обрив сесії **не** скасовує операцію, `BUSY` для заборонених команд і обробка дозволених під час операції, запис `StateRecord` після завершення, кожна зміна напруги через `power_apply`
- [ ] T068 [P] [US2] Модульні тести гілок `PROGRAM` у `software/firmware/tests/test_program_branches.c`: OTP чиста / сумісна (повторна спроба, `RESUMED`) / усе вже записано (`CHIP_ALREADY_PROGRAMMED`) / несумісна (`CHIP_NOT_BLANK` без запису); вставка серійного номера чиста → новий номер, записана → номер з мікросхеми; `CALIB_FAILED` → нічого не записано після циклу, fuse не записаний; `CALIB_FINALIZE_ON_FAIL` лише при `source = 0`; розбіжність потокового CRC → `STORE_CORRUPTED` до fuse (FR-049); `FINAL_VERIFY` ловить «слабке» слово (FR-048)
- [ ] T069 [P] [US2] Модульні тести `VERIFY` і `PATCH` у `software/firmware/tests/test_verify_patch.c`: `VERIFY` з урахуванням вставок (завершений слід з будь-яким `k` → OK + фактичні `k` і серійний номер; цикл → `VERIFY_FAILED`, `site_status = FAILED`); `PATCH`: правило `(current AND value) == value`, конфлікт → `PATCH_CONFLICT` без жодного запису, `expect_mode` RAW/NORMALIZED/ACTIVE_PACKAGE → `PATCH_IMAGE_MISMATCH` без запису, службова область, `addr ≥ code_words`, `count` 0/>64, захисний fuse, `CHECK_ONLY`, повна перевірка після запису
- [ ] T070 [P] [US2] Контрактний тест `software/client/tests/contract/test_program.py`: `PROGRAM` на моделі PFS154 → усі етапи в порядку, `RESULT` з `calib_count = 1` у межах ±10 %, випадковим `serial_written`, `image_crc_raw/norm`; вміст моделі = ефективному образу з фактичними `k`; `CHIP_ID_MISMATCH` з `chip_id_found`; `CHIP_NOT_BLANK` для несумісної OTP-моделі
- [ ] T071 [P] [US2] Контрактний тест `software/client/tests/contract/test_busy_cancel.py`: під час `PROGRAM` `PKG_BEGIN`/`OP_START` → `BUSY`, `GET_STATUS`/`OP_RESULT` працюють; `OP_CANCEL` → `CANCELLED` з етапом, напруги зняті
- [ ] T072 [P] [US2] Контрактний тест SC-008 `software/client/tests/contract/test_disconnect.py`: ≥ 100 розривів TCP у довільний момент `PROGRAM` → вміст моделі як без розриву, після перепідключення `OP_RESULT(0)` → `OK`
- [ ] T073 [P] [US2] Контрактний тест SC-002 `software/client/tests/contract/test_stored_corruption.py`: по черзі пошкоджує кожен байт пакета у файлі flash симулятора; `PROGRAM` → `STORE_CORRUPTED` на `PKG_CHECK`, модель мікросхеми не змінена
- [ ] T074 [P] [US2] Контрактний тест SC-012 `software/client/tests/contract/test_otp_retry.py`: ≥ 100 «обривів живлення» в довільній точці `PROGRAM` OTP-моделі PMS150C (`--chip-state`), повторний `PROGRAM` тим самим і «іншим» симулятором (окремий `--flash`, той самий пакет) → `OK` + `RESUMED` або `CHIP_NOT_BLANK` без змін; жодного зайвого нуля; записаний серійний номер зберігається
- [ ] T075 [P] [US2] Контрактний тест SC-013 `software/client/tests/contract/test_calib_fail.py`: модель із «несправним» осцилятором → `CALIB_FAILED`, після циклу нічого не записано; з `CALIB_FINALIZE_ON_FAIL` → `CALIB_FORCED`; після «ремонту» повторний `PROGRAM` → `OK` + `RESUMED`
- [ ] T076 [P] [US2] Контрактний тест SC-011 `software/client/tests/contract/test_power_fault.py`: `--power-fault vpp:slow|vpp:stuck|vdd:low` → `VPP_FAULT`/`VDD_FAULT` ≤ 20 мс після зміни, `RESULT.v_rail/v_set_mV/v_meas_mV` заповнено, модель мікросхеми без жодного запису
- [ ] T077 [P] [US2] Контрактний тест `software/client/tests/contract/test_verify_final.py`: `VERIFY` каліброваної мікросхеми з серійним номером → `OK` з фактичними `k` і номером; незавершене калібрування → `VERIFY_FAILED`; модель із «слабким» записом слова вставки або fuse → `VERIFY_FAILED` на `FINAL_VERIFY`; `--corrupt-flash-during-write` → `STORE_CORRUPTED` до fuse
- [ ] T078 [P] [US2] Контрактний тест `software/client/tests/contract/test_patch.py` на записаній OTP-моделі PMS150C: сумісний патч з `--expect-package` → змінено й верифіковано лише задані слова; партія з трьох моделей із різним зсувом IHRC і серійними номерами проходить з однією нормалізованою сумою; мікросхема з іншою прошивкою → `PATCH_IMAGE_MISMATCH`, модель незмінна; конфлікт 0→1 → `PATCH_CONFLICT`, модель незмінна; `--check` показує обидві суми й нічого не пише

- [ ] T131 [P] [US2] Модульні тести перевірки активної вставки (FR-055) у `software/firmware/tests/test_calib_site_check.c` з фейковим рушієм: дві вставки IHRC + ILRC, мікросхема «застрягла» у вищій за адресою → `CALIB_WRONG_SITE` на першому вимірі (частота ×100 від очікуваної), нічого не записано; модель без відгуку на «+1» або з хаотичною частотою → `CALIB_WRONG_SITE`; BG із PA0, що «висить», → `CALIB_WRONG_SITE`; `CALIB_FINALIZE_ON_FAIL` не застосовується; правильний порядок — `OK`
- [ ] T132 [P] [US2] Контрактний тест `software/client/tests/contract/test_calib_wrong_site.py`: модель мікросхеми з режимом «виконує вставки не за порядком адрес» (`--chip PFS154:order=reverse`) → `PROGRAM` завершується `CALIB_WRONG_SITE`, вміст моделі після вимірювальних циклів не змінено

### Implementation for User Story 2

- [ ] T079 [US2] Реалізувати модель мікросхеми для симулятора (залежить від T050 — опкоди вимірювального циклу) (`chip_engine` на пам'яті: ID за `--chip`, FLASH/OTP-семантика 1→0, стирання, лічильник стирань; модель IHRC/ILRC: частота = f(регістр) × (1 + зсув `offset=±N%`), розпізнавання записаного вимірювального циклу й «генерація» частоти на PA3 з урахуванням імпульсів PA4; модель BG-компаратора; режими `osc=broken`, `weak=ADDR`; збереження стану `--chip-state`; `safe_state`) у `software/firmware/platform/host/chip_model.c`
- [ ] T080 [US2] Реалізувати детектор слідів калібрування й вимірювальних циклів та відновлення вставок у `software/firmware/core/chips/calib_footprint.c` і `calib_footprint.h` (залежить від T050, T023)
- [ ] T081 [US2] Реалізувати ефективний образ (`EffectiveImagePlan`: слово за адресою, стани вставок, підстановка фактичних `k` і серійного номера, fuse) у `software/firmware/core/package/effective_image.c` і `effective_image.h` (залежить від T050, T051, T052)
- [ ] T082 [US2] Реалізувати суми вмісту `image_crc_raw/norm` (потоковий нормалізатор зі спільним детектором T080 і вставок серійного номера — 8 `RET #imm`; обчислення з ефективного образу пакета) у `software/firmware/core/package/image_crc.c` і `image_crc.h` (залежить від T080, T081)
- [ ] T083 [P] [US2] Реалізувати генератор серійних номерів (≥ 256 сирих біт з `hal_entropy` + UID + `rec_seq` + лічильник RAM → BLAKE2s, 64 біти; спрощені тести повторів/пропорцій) у `software/firmware/core/package/serial_rng.c` і `serial_rng.h`
- [ ] T084 [US2] Реалізувати менеджер операцій (IDLE/RUNNING/DONE, `op_id`, етапи й відсотки, `poll` для рушія з обробкою дозволених команд, скасування, обрив сесії не перериває операцію, `RESULT`) у `software/firmware/core/ops/op_manager.c` і `op_manager.h` (залежить від T025, T020)
- [ ] T085 [US2] Реалізувати конвеєр `PROGRAM` за data-model `ProgramJob` (PKG_CHECK; ID; ERASE для FLASH; BLANK_CHECK із перевіркою сумісності OTP і повторною спробою; WRITE з потоковим CRC; VERIFY; CALIBRATE кожної вставки з політикою FR-043 і `CALIB_FINALIZE_ON_FAIL`; PKG_CHECK; FUSE; FINAL_VERIFY із сумами вмісту; випадковий серійний номер; `op_flags.SKIP_BLANK_CHECK` для OTP → `PKG_OTP_BLANK_CHECK_REQUIRED`) у `software/firmware/core/ops/op_program.c` (залежить від T081, T082, T083, T084, T055)
- [ ] T086 [US2] Реалізувати конвеєри `VERIFY` (з урахуванням вставок, FR-047), `PROBE`, `ERASE`, `BLANK_CHECK` у `software/firmware/core/ops/op_simple.c` (залежить від T081, T082, T084)
- [ ] T087 [US2] Реалізувати операцію `PATCH` (розбір `ext`, `ID` → читання всієї пам'яті → перевірка суми за `expect_mode` і сумісності всіх слів до будь-якого запису → запис лише змінених слів блоками з одиницями → повна перевірка; `CHECK_ONLY`; заборони службових областей і захисного fuse) у `software/firmware/core/ops/op_patch.c` (залежить від T082, T084)
- [ ] T088 [US2] Реалізувати команди `OP_START`, `OP_CANCEL`, `OP_RESULT`, `GET_STATUS` (структури `STATUS`, `RESULT` з усіма полями contracts §4) і події `EVT_OP_STARTED`, `EVT_PROGRESS`, `EVT_OP_DONE` у `software/firmware/core/ops/op_cmds.c` (залежить від T085, T086, T087)
- [ ] T089 [US2] Під'єднати модель мікросхеми в `pdk-sim` (`--chip NAME[:offset=..,osc=..,weak=..]|none|blank|written`) у `software/firmware/platform/host/sim_main.c` (залежить від T079, T088)
- [ ] T090 [US2] Додати в `Programmer` методи `start_op()`, `cancel_op()`, `op_result()`, `get_status()`, `patch()`, ітератор подій із розбором `EVT_PROGRESS`/`RESULT` у `software/client/padauk_prog/protocol.py` (залежить від T060)
- [ ] T091 [US2] Додати CLI-підкоманди `write FILE -n NAME [опції upload]` (= upload + `PROGRAM`), `program`, `verify`, `probe`, `erase -n NAME`, `blankcheck -n NAME`, `status`; прогрес-рядок; підсумок з калібруванням, серійним номером, сумами, адресою розбіжності, `RESUMED`/`CALIB_FORCED`; Ctrl+C → `OP_CANCEL`; `--finalize-on-calib-fail` у `software/client/padauk_prog/cli.py` (залежить від T090)
- [ ] T133 [US2] Реалізувати перевірки FR-055 у примітиві `calibrate` (правдоподібність першого виміру 1/4…4 від `target_hz / 7`, монотонність відгуку на крок, PA0 ведеться для BG) у рушії мікросхем і моделі мікросхеми (`order=reverse`, «без відгуку») у `software/firmware/platform/host/chip_model.c`; клієнт показує `CALIB_WRONG_SITE` з поясненням «вставки виконуються не в порядку адрес» (залежить від T079, T085)
- [ ] T092 [US2] Додати CLI-підкоманду `patch -n NAME ADDR=VALUE … (--expect-crc CRC [--normalized] | --expect-package) [--check] [--allowsecfuse]` (без очікуваної суми — лише `--check`; конфлікт у двійковому вигляді) у `software/client/padauk_prog/cli.py` (залежить від T090)

**Checkpoint**: US1 + US2 працюють на `pdk-sim`; T062–T078 зелені

---

## Phase 5: User Story 4 — Інформація про програматор, мікросхеми й самоперевірка (Priority: P2)

**Goal**: клієнт показує версії, можливості, таблицю мікросхем із програматора; несумісна версія блокує операції; `SELF_TEST` без мікросхеми (автоматично при старті й з PC)

**Independent Test**: незмінений клієнт бачить додаткову мікросхему зі збірки `pdk-sim-extra`; `pdkprog selftest` з порожньою «панелькою» показує всі рівні VDD/VPP, з мікросхемою — відмова без високих напруг

### Tests for User Story 4

- [ ] T093 [P] [US4] Модульні тести самоперевірки в `software/firmware/tests/test_selftest.c` з фейковими `hal_power` і рушієм: рівні = усі різні значення VDD/VPP з таблиці мікросхем у порядку зростання + 0 В після кожної шини; інша шина = 0; probe першим, відповідь мікросхеми → `SELFTEST_CHIP_PRESENT` без рівнів вище probe; перший недосягнутий рівень → `SELFTEST_FAILED`, `safe_state`; `RESULT.levels[]`; автозапуск при старті з `source = 1`; `STATUS.selftest_state`
- [ ] T094 [P] [US4] Контрактний тест `software/client/tests/contract/test_info.py`: `INFO` (версія 1.0, `max_payload`, `max_package_size`, `max_read_words`, біти можливостей), `LIST_CHIPS` з пагінацією, мажорна версія 2 → `PROTO_VERSION`, `OP_START`/`PKG_BEGIN` → `NO_SESSION`; SC-006 з `pdk-sim-extra`
- [ ] T095 [P] [US4] Контрактний тест `software/client/tests/contract/test_selftest.py`: `--chip none` → `OK` з усіма рівнями; `--chip PFS154` → `SELFTEST_CHIP_PRESENT`; `--power-fault vpp:stuck` → `SELFTEST_FAILED` з рівнем; після старту симулятора `STATUS.selftest_state` = 2/3/4 відповідно

### Implementation for User Story 4

- [ ] T096 [US4] Реалізувати операцію `SELF_TEST` (contracts §4 `SELF_TEST`, research R15) і автозапуск при старті ядра у `software/firmware/core/ops/op_selftest.c` та `software/firmware/core/pdk_core.c` (залежить від T020, T084, T023)
- [ ] T097 [US4] Додати тестовий запис мікросхеми під `#ifdef PDK_TEST_EXTRA_CHIP` у `software/firmware/core/chips/chip_table.c` і ціль `pdk-sim-extra` у `software/firmware/CMakeLists.txt`
- [ ] T098 [US4] Додати CLI-підкоманди `info` (версії, платформа, можливості, стан пакета, `selftest_state`), `list` (як `easypdkprog list`: назва, ID, FLASH/OTP, слова, розрядність, RAM, `(RO)`), `selftest` (підтвердження «панелька порожня», таблиця рівнів задано/виміряно/час) у `software/client/padauk_prog/cli.py` (залежить від T090)

**Checkpoint**: US4 перевірено; клієнт не містить таблиці мікросхем (`grep` назв мікросхем у `software/client/padauk_prog/` порожній)

---

## Phase 6: User Story 3 — Автономний запис з перевіркою цілісності (Priority: P2)

**Goal**: локальний запуск без PC; пошкоджений пакет не записується; локальний запис заблоковано до успішної самоперевірки; кожна мікросхема отримує випадковий серійний номер; статистика переживає вимкнення

**Independent Test**: `pdk-sim` з пошкодженим flash → `local-start program` не записує, індикація помилки; з цілісним пакетом — три локальні запуски дають три різні серійні номери; при `--power-fault` після старту локальний запуск виконує `SELF_TEST`, а не `PROGRAM`

### Tests for User Story 3

- [ ] T099 [P] [US3] Модульні тести локального запуску в `software/firmware/tests/test_local_start.c`: повний набір дій FR-054 (`PROGRAM`, `COPY`, `VERIFY`, `SELF_TEST`, `RESTORE_MASTER`, `local_cancel`); пріоритет (самоперевірку не пройдено → `SELF_TEST`; `MASTER_RESTORE_PENDING` → `RESTORE_MASTER`); `RESTORE_MASTER` без `PENDING` ігнорується; послідовність `ui_state` для кожного результату з правильним класом причини; під час операції запит ігнорується; `EMPTY`/`CORRUPTED` → без напруг, `ui_state` помилки цілісності; локальна операція не скасовується обривом сесії; `CALIB_FINALIZE_ON_FAIL` ігнорується
- [ ] T100 [P] [US3] Контрактний тест `software/client/tests/contract/test_autonomous.py`: три `local-start program` → три різні `serial_written`; перезапуск симулятора зберігає `ok/fail`; з підключеним клієнтом приходять `EVT_OP_STARTED(source=1)` і `EVT_OP_DONE`; пошкоджений пакет → жодного запису моделі; несправне живлення при старті → `LOCAL_START_BLOCKED`, `local-start` → `SELF_TEST`; `local-start verify` і `local-start selftest` дають відповідні `RESULT`; `local-cancel` посеред запису → `CANCELLED`, напруги зняті; вивід `ui_state` симулятора відповідає FR-054

### Implementation for User Story 3

- [ ] T101 [US3] Реалізувати локальні дії FR-054 (contracts/chip-engine.md §4: `local_start(request)` для всіх запитів, `local_cancel()`) з перетворенням за пріоритетом research R16, перевірку `PackageState` перед записом і `STATUS.flags.LOCAL_START_BLOCKED` у `software/firmware/core/ops/local_start.c` і `local_start.h` (залежить від T085, T096)
- [ ] T102 [P] [US3] Реалізувати стани індикації `ui_state(state, detail)` за contracts/chip-engine.md §4 (`READY`, `BUSY` з етапом і відсотком, `SUCCESS`, `FAIL` із класом причини й кодом, `BLOCKED`) у `software/firmware/core/ops/indication.c` і `indication.h` та відображення на 3 LED у `software/firmware/platform/stm32f072/hal_f072_misc.c`
- [ ] T103 [US3] Реалізувати `STATS_GET` і `STATS_RESET` (ok/fail у журналі) у `software/firmware/core/storage/stats_cmds.c` (залежить від T054)
- [ ] T104 [US3] Додати CLI-підкоманди `stats`, `stats reset` і `monitor` (показ локальних операцій і їхніх результатів) у `software/client/padauk_prog/cli.py` (залежить від T090)

**Checkpoint**: автономний режим працює на `pdk-sim`; на F072 — з рушієм-заглушкою блокує запис при пошкодженому пакеті й невдалій самоперевірці

---

## Phase 7: User Story 5 — Читання та копіювання мікросхеми (Priority: P3)

**Goal**: читання в клієнт із CRC; копіювання в пакет із відновленням калібрувальних вставок окремо для IHRC/ILRC/BG (`KEEP_VALUE`, `TARGET_FROM_HOST`, `SNAP_STANDARD`, `MEASURE_MASTER`), захист еталона, обробка вимірювального циклу й чужих форматів, серійний номер не обробляється

**Independent Test**: `read` → файл = записаному образу; `copy --ihrc measure --allow-modify-master` з еталона 8 МГц (зсув +3 %) → копії зі зсувами −4/0/+5 % у межах 1 % від еталона, еталон побітово незмінний

### Tests for User Story 5

- [ ] T105 [P] [US5] Модульні тести формування пакета з дампу в `software/firmware/tests/test_copy.c`: fuse з останнього слова, `CREATED_BY_COPY`, `name` з `ext` або `copy <chip> <crc>`, вставки за режимами для кожного типу, `bg_restore`, `SNAP_STANDARD` лише IHRC (кодування `0xFF0000kk`), байти серійного номера як є + `SERIAL_DUPLICATED`, `CALIB_UNRECOGNIZED` з адресою, захищений fuse → `CHIP_PROTECTED`, мікросхема без запису → `OP_UNSUPPORTED`
- [ ] T106 [P] [US5] Модульні тести автомата `CopyJob` у `software/firmware/tests/test_copy_job.c` з фейковим рушієм: вимірювальний цикл у дампі без `ALLOW_MASTER_LOOP` → `COPY_MASTER_UNCALIBRATED` без запису в сховище, з дозволом і без `TARGET_FROM_HOST` → `BAD_PAYLOAD`; `MEASURE_MASTER`: комміт дампу й `PENDING = 1` до першого стирання, по одному перезапису на слід (n + 1 стирань), слід без сигналу 1 с → `COPIED` і продовження, `OP_CANCEL` у `MEASURE` → `RESTORE` → DONE, невдалий `RESTORE` → `PENDING` лишається; OTP → `MASTER_NOT_REWRITABLE`; без прапорця → `MASTER_MODIFY_NOT_ALLOWED`; `COPY`/`PKG_BEGIN`/`PKG_DELETE` при `PENDING` → `MASTER_RESTORE_PENDING`
- [ ] T107 [P] [US5] Контрактний тест `software/client/tests/contract/test_read_copy.py`: `READ` → `data_crc32` = CRC даних `RESULT_READ`; `COPY` (`KEEP_VALUE`, `TARGET_FROM_HOST`, `SNAP_STANDARD`, BG restore/keep) → новий пакет, `EVT_PKG_STATE`, `RESULT.calib[]` і `warnings`; захищена модель → `CHIP_PROTECTED`; `RESULT_READ` після `PKG_BEGIN` → `NOT_ALLOWED_NOW`
- [ ] T108 [P] [US5] Контрактний тест SC-009 `software/client/tests/contract/test_copy_measure.py`: еталон (зсув +3 %) зі вставкою IHRC 8 МГц; `COPY` `MEASURE_MASTER` → дамп до й після однаковий, стирань +2; `PROGRAM` трьох копій (−4 %, 0, +5 %) → у межах 1 % від еталона; `KEEP_VALUE` дає розбіжність > 5 %; варіант IHRC + ILRC + BG → стирань +3, обидві RC-цілі й BG відновлено; варіант зі слідом, до якого програма не доходить → `COPIED` з попередженням
- [ ] T109 [P] [US5] Контрактний тест SC-010 `software/client/tests/contract/test_copy_power_loss.py`: ≥ 100 обривів живлення в довільній точці `COPY(MEASURE_MASTER)`, зокрема між вимірами двох слідів; після перезапуску `PENDING = 0` і еталон цілий, або `PENDING = 1` і `RESTORE_MASTER` (з PC і локальним запуском) повертає еталон побітово
- [ ] T110 [P] [US5] Контрактний тест `software/client/tests/contract/test_copy_special.py`: еталон із вимірювальним циклом — локальне `COPY` і `COPY` з PC без дозволу → `COPY_MASTER_UNCALIBRATED`, `PKG_INFO` без змін; з `ALLOW_MASTER_LOOP` + `TARGET_FROM_HOST` → вставка з заданою ціллю (FR-051); еталон із записом константи в `IHRCR` без сліду і з серійним номером free-pdk → `OK`, код як є, `warnings` = `CALIB_UNRECOGNIZED` + `SERIAL_DUPLICATED` (FR-052, FR-053)

### Implementation for User Story 5

- [ ] T111 [US5] Реалізувати конвеєр `READ` (дані в неактивний слот, `ReadResult` у RAM, перевірка захисту за R10, `image_crc_raw/norm`) і команду `RESULT_READ` з інвалідизацією при `PKG_BEGIN`/`READ`/`COPY` у `software/firmware/core/ops/op_read.c` і `software/firmware/core/ops/op_cmds.c` (залежить від T082, T088, T056)
- [ ] T112 [US5] Розширити `software/firmware/hal/chip_engine.h` примітивом `measure_freq(site, start_value, vdd_mV)` і реалізувати його в `software/firmware/platform/host/chip_model.c` та `software/firmware/platform/stm32f072/chip_engine_stub.c` (`OP_UNSUPPORTED`) (залежить від T079, T033)
- [ ] T113 [US5] Реалізувати операцію `COPY` за автоматом `CopyJob` (розбір `ext` 14/46 байт, режими для IHRC/ILRC/BG, вимірювальний цикл і `ALLOW_MASTER_LOOP`, `MEASURE_MASTER` з n + 1 стираннями й тайм-аутом сліду 1 с, комміт дампу й `PENDING` до першого стирання, відновлення з верифікацією, фінальний пакет із `CREATED_BY_COPY` і `name`, `warnings` `SERIAL_DUPLICATED`/`CALIB_UNRECOGNIZED`/`MASTER_MODIFIED`) у `software/firmware/core/ops/op_copy.c` (залежить від T080, T111, T112)
- [ ] T114 [US5] Реалізувати `RESTORE_MASTER`, блокування `PKG_BEGIN`/`PKG_DELETE`/`COPY` при `MASTER_RESTORE_PENDING` і `STATUS.flags` у `software/firmware/core/ops/op_restore.c`, `software/firmware/core/package/upload.c`, `software/firmware/core/ops/op_cmds.c` (залежить від T113)
- [ ] T115 [US5] Реалізувати `SNAP_STANDARD` у `PROGRAM`: вставка з частотою `0xFF0000kk` → вимір копії при `kk`, округлення до {16, 8, 4, 2, 1 МГц, 500 кГц} при відхиленні ≤ 10 %, інакше `kk` з `CALIB_VALUE_COPIED` у `software/firmware/core/package/effective_image.c` і `software/firmware/core/ops/op_program.c` (залежить від T085, T080)
- [ ] T116 [US5] Реалізувати `CONFIG_GET`/`CONFIG_SET` (`cfg_copy_ihrc` 0/2, `cfg_copy_ilrc` 0, `cfg_copy_bg` 0/1; типово 0, 0, 1) і використання конфігурації локальним `COPY` у `software/firmware/core/storage/config_cmds.c` та `software/firmware/core/ops/local_start.c` (залежить від T101, T113)
- [ ] T117 [US5] Розширити модель мікросхеми: режим «захищена» (`:protected`), OTP-еталон, еталон із вимірювальним циклом, запис константи в `IHRCR` без сліду, команда stdin `swap-chip` у `software/firmware/platform/host/chip_model.c` і `software/firmware/platform/host/sim_main.c` (залежить від T079)
- [ ] T118 [US5] Додати CLI-підкоманди `read FILE -n NAME [-b]` (Intel HEX 8-біт / raw bin, перевірка `data_crc32`), `copy [-n NAME] [--ihrc keep|target|snap|measure] [--ilrc keep|target|measure] [--bg keep|restore] [--ihrc-freq HZ] [--ilrc-freq HZ] [--calib-mv MV] [--allow-modify-master] [--name NAME]` (для `measure` — підтвердження з попередженням про ресурс стирання; при `COPY_MASTER_UNCALIBRATED` — інтерактивний вибір «скасувати / задати ціль» і повтор з `ALLOW_MASTER_LOOP`; показ `SERIAL_DUPLICATED` і `CALIB_UNRECOGNIZED`), `restore-master`, `config copy [--ihrc keep|snap] [--bg keep|restore]` у `software/client/padauk_prog/cli.py` (залежить від T090)

**Checkpoint**: US5 перевірено на `pdk-sim`, зокрема SC-009 і SC-010

---

## Phase 8: User Story 6 — Запуск мікросхеми з обміном даними (Priority: P3)

**Goal**: `RUN` з напругою, міст UART в обидва боки, зупинка; обрив сесії зупиняє `RUN`

**Independent Test**: модель у `pdk-sim` шле `"U"` + `"hello\n"` — клієнт показує `Connected @… baud` і текст; введені символи повертаються ехом

### Tests for User Story 6

- [ ] T119 [P] [US6] Контрактний тест `software/client/tests/contract/test_run.py`: `OP_START(RUN, 5000)` → `EVT_UART_LINK`, `EVT_UART_RX`; `UART_TX` доходить до моделі; `UART_TX` поза `RUN` → `NOT_ALLOWED_NOW`; `OP_CANCEL` → напруга знята, `OK`; розрив сесії → `HOST_DISCONNECTED`, напруга знята

### Implementation for User Story 6

- [ ] T120 [P] [US6] Додати інтерфейс UART-мосту `software/firmware/hal/hal_uart.h` (старт з автовизначенням швидкості, відправка, прийом у кільцевий буфер, подія «швидкість визначено») і хост-реалізацію «hello + ехо» у `software/firmware/platform/host/hal_host_uart.c`
- [ ] T121 [US6] Реалізувати конвеєр `RUN` (VDD через `power_apply` з `param`/`run_vdd_mV`/5000, VPP = 0, UART-міст, `EVT_UART_LINK`, `EVT_UART_RX`, зупинка `OP_CANCEL` і обривом сесії) і команду `UART_TX` у `software/firmware/core/ops/op_run.c` (залежить від T084, T120)
- [ ] T122 [P] [US6] Реалізувати UART-міст для F072 (USART1 PB6/PB7, автовизначення швидкості по старт-біту, DMA-прийом, пропуск першого `'U'`) у `software/firmware/platform/stm32f072/hal_f072_uart.c`
- [ ] T123 [US6] Додати CLI-підкоманду `start [-r VDD]` (вивід `EVT_UART_RX`, клавіатура → `UART_TX`, Esc → `OP_CANCEL`) у `software/client/padauk_prog/cli.py` (залежить від T090)

**Checkpoint**: усі історії перевірено на `pdk-sim`

---

## Phase 9: Polish & Cross-Cutting Concerns

- [ ] T124 [P] Опублікувати протокол: `docs/protocol.md` = `contracts/protocol.md` + `contracts/package-format.md` + `contracts/chip-engine.md` з розділами «Версіонування», «Коди статусу», «Відповідність easypdkprog»; посилання з кореневого `README.md` (FR-029)
- [ ] T125 [P] Додати перевірку розміру прошивки для кожної цілі в `software/firmware/CMakeLists.txt` (post-build `arm-none-eabi-size`; для F072 — помилка, якщо `.text + .data` > 44 КБ, і опції збірки `PDK_FEATURE_MEASURE_MASTER`, `PDK_FEATURE_PATCH` для вимкнення функцій з відображенням в `INFO.capabilities`)
- [ ] T126 [P] Додати GitHub Actions `.github/workflows/ci.yml`: збірка `PLATFORM=host` + `ctest` (Linux, Windows), збірка `PLATFORM=stm32f072` з перевіркою розміру, `pytest` unit + contract проти `pdk-sim` (зменшені ітерації ін'єкції помилок; повні — `workflow_dispatch`)
- [ ] T127 [P] Написати `software/client/README.md` (встановлення, приклади `pdkprog` для кожної підкоманди, відповідність `easypdkprog`, порада щодо макросів калібрування й серійного номера) і `software/firmware/README.md` (збірка, DFU, розмітка flash)
- [ ] T128 Пройти сценарії 1–4 з `quickstart.md` і виправити розбіжності в документації `specs/001-pc-programmer-protocol/quickstart.md`
- [ ] T129 Перевірка на залізі цілі налагодження (F072 + заглушка рушія), результат у `specs/001-pc-programmer-protocol/hw-validation.md`: розмір прошивки і вимкнені функції; апаратна межа напруги — максимум VPP/VDD при ЦАП = максимум і при «зависанні» прошивки (принцип III); напруги зняті одразу після скидання/увімкнення; `pdkprog info/list/selftest` (точність ±2 % / ±30 мВ і встановлення < 20 мс на кожному рівні, з навантаженням-імітатором цільової плати); завантаження пакета і перевірка після вимкнення живлення; 20 ручних висмикувань USB/живлення під час завантаження (пакет завжди `VALID`); локальний запуск із пошкодженим пакетом і з несправним/відключеним VPP; якість ентропії (≥ 10⁵ згенерованих номерів без повторів, тест пропорцій); швидкість завантаження 8 КБ ≤ 3 с (SC-003)
- [ ] T130 Оновити `specs/001-pc-programmer-protocol/plan.md` і `research.md`, якщо реалізація відхилилась від рішень (розмір прошивки → STM32F072CB, BLAKE2s → інша хеш-функція тощо); зафіксувати, що SC-004 і SC-009 на реальних мікросхемах перевіряються у фічі `002-chip-engine`, а порт на основну МК, розмітка flash і кількість слотів — у специфікації вибору МК

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)** → без залежностей
- **Foundational (Phase 2)** → після Setup; **блокує всі історії** (включно з політикою напруг `core/power` і `LIST_CHIPS`)
- **US1 (Phase 3)** → після Foundational
- **US2 (Phase 4)** → після US1 (збережений пакет, `validate`, `calib_sites`, `serial_sites`)
- **US4 (Phase 5)** → після US2 (менеджер операцій для `SELF_TEST`); `info`/`list` — лише після Foundational
- **US3 (Phase 6)** → після US2 і US4 (локальний запуск блокується до успішної самоперевірки)
- **US5 (Phase 7)** → після US2 (детектор слідів, `PROGRAM`) і US3 (локальне копіювання, `local_start`)
- **US6 (Phase 8)** → після US2 (менеджер операцій); незалежна від US3–US5
- **Polish (Phase 9)** → після потрібних історій

### Граф історій

```
Setup → Foundational → US1 → US2 ─┬─► US4 ─► US3 ─► US5
                                  └─► US6
```

### Within Each User Story

- Тести → падають → реалізація ядра → команди → клієнт/CLI
- Спільні структури (`proto_defs.h`, `chip_table`, HAL, `core/power`) — у Foundational; задачі історій змінюють чужі файли лише для реєстрації команд у диспетчері та розширення інтерфейсів (T112, T114, T115, T116 — явно позначені)

---

## Parallel Example: User Story 1

```text
# Тести (різні файли):
T038 вектори   T039 test_package_validate.c   T040 test_sites.c   T041 test_journal.c   T042 test_upload.c
T043 test_ihex_package.py   T044–T048 контрактні тести
# Незалежні модулі:
T049 header.c   T050 calib_sites.c   T051 serial_sites.c   T053 slots.c   T054 journal.c   T058 ihex.py
# Потім: T052 → T055 → T056 → T057;  T059 → T060 → T061
```

## Parallel Example: User Story 2

```text
T062–T069 модульні тести   T070–T078 контрактні тести   T079 chip_model.c   T083 serial_rng.c
# Потім: T080 → T081 → T082;  T084;  T085, T086, T087 → T088 → T089;  T090 → T091, T092
```

## Parallel Example: US4 і US6 після US2

```text
Розробник A: T093–T095 → T096 → T097 → T098 (US4) → US3
Розробник B: T119, T120, T122 → T121 → T123 (US6)
```

---

## Implementation Strategy

### MVP First (User Story 1)

1. Phase 1 Setup → Phase 2 Foundational (з тестами, зокрема політика напруг)
2. Phase 3 US1 → **STOP and VALIDATE**: T044–T048 на `pdk-sim`, потім завантаження на F072
3. Результат: надійне завантаження й збереження пакета — фундамент автономності

### Incremental Delivery

1. US1 → цілісне завантаження (MVP)
2. US2 → запис, верифікація, `PATCH` на моделі (на залізі — разом з `002-chip-engine`)
3. US4 → `info` / `list` / `selftest`
4. US3 → автономний режим
5. US5 → читання/копіювання
6. US6 → запуск із UART

### Notes

- [P] = інші файли, без залежностей від незавершених задач
- Кожна історія завершується checkpoint-ом із зеленими тестами
- Комітити після кожної задачі або логічної групи
- Реальні алгоритми PADAUK (`chip_engine` для F072) — фіча `002-chip-engine`; тут — заглушка й модель
