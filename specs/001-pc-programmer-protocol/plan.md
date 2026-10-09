# Implementation Plan: Протокол обміну PC ⇄ програматор

**Branch**: `001-pc-programmer-protocol` | **Date**: 2026-10-09 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/001-pc-programmer-protocol/spec.md`

## Summary

Бінарний протокол поверх USB CDC: кадри COBS із CRC-32 і порядковими номерами, stop-and-wait із
повтором і кешем відповіді. PC передає один **пакет прошивки** (заголовок 128 Б + слова + маска заданих
слів + наскрізний CRC-32), який програматор перевіряє за власною таблицею мікросхем і атомарно зберігає в
одному з двох слотів flash; активний слот, налаштування й статистику фіксує журнал стану
з власним CRC. Операції з мікросхемою (`PROBE`, `READ`, `ERASE`, `BLANK_CHECK`, `PROGRAM`, `VERIFY`,
`COPY`, `RUN`) запускаються однією командою; програматор сам виконує всі кроки, надсилає події прогресу
й підсумок. Ядро — платформонезалежний C з HAL; збирається для STM32F072 і для PC (симулятор для тестів).
Еталонний клієнт — Python з CLI у стилі `easypdkprog`. `COPY` розпізнає сліди калібрування в дампі
еталона й відновлює калібрувальні вставки; ціль — виміряна на тимчасово перезаписаному FLASH-еталоні
(з попереднім збереженням дампу й гарантованим відновленням), задана з PC, округлена до стандартної
частоти або перенесене значення. `PATCH` дописує окремі слова в уже записану мікросхему лише після
перевірки контрольної суми всієї прошивки й правила «тільки 1→0». Кожна зміна VDD/VPP підтверджується
АЦП (мінімальна пауза free-pdk **і** ±2 % / ±30 мВ, тайм-аут 20 мс) — спільна політика ядра
(`core/power`) для рушія й `SELF_TEST`; самоперевірка без мікросхеми виконується при кожному старті.
Локальні дії (кнопки, перемикачі, конвеєр — окрема фіча) приходять у ядро як абстрактний
`local_start`. Калібрування пропускається й копіюється окремо для IHRC/ILRC/BG; невдале калібрування
нічого не дописує, а перерваний чи невдалий запис OTP можна повторити, якщо записане сумісне з образом
(R17). Рішення обґрунтовано в [research.md](research.md).

## Technical Context

**Language/Version**: прошивка — C11 (`arm-none-eabi-gcc` ≥ 12; хост — gcc/clang/MSVC); клієнт — Python ≥ 3.10

**Primary Dependencies**: TinyUSB (MIT) — CDC ACM; STM32 LL/CMSIS лише в шарі платформи; клієнт — `pyserial`

**Storage**: внутрішня flash МК: 2 слоти пакета × 8 КБ + журнал стану 2 × 2 КБ (розмітка задається платформою)

**Testing**: ядро — модульні тести на C (CMake/CTest, мінімальний вбудований фреймворк або Unity);
клієнт і контракти — `pytest` проти симулятора `pdk-sim`; приймальні — на залізі

**Target Platform**: основна — STM32 з вищими ресурсами (≥ 128 КБ flash, ≥ 32 КБ RAM, USB FS, ЦАП/АЦП,
бажано RNG і CRC), **модель обирається наступною специфікацією** разом із розміткою flash і кількістю
слотів; у цій фічі — **ціль налагодження STM32F072C8T6** (плати Easy PDK) і хост (симулятор, клієнт:
Windows / Linux / macOS)

**Project Type**: вбудована прошивка + бібліотека/CLI на PC

**Performance Goals**: відповідь на кожен запит ≤ 100 мс; завантаження пакета `max_package_size` (8 КБ) ≤ 3 с; прогрес ≥ 1 раз/с; встановлення напруги ≤ 20 мс

**Constraints**: для цілі налагодження F072 прошивка ≤ 44 КБ flash (частину функцій MAY вимикати збіркою); для основної МК — визначить її специфікація; RAM без буфера образу (трансформація на льоту з flash);
найдовший атомарний крок рушія < 100 мс; обрив USB не перериває операцію (окрім `RUN`), усі дані — у програматорі до старту; апаратні вимоги: джерела VDD/VPP з активним розрядом до 0 В, встановлення < 20 мс під навантаженням цільової плати (pogo), АЦП з похибкою < ±1 %, апаратне обмеження максимальної напруги

**Scale/Scope**: один клієнт; один пакет; ≤ 4096 слів читання (`max_read_words`), пакет запису ≤ 8 КБ (`max_package_size`; найбільший записуваний у таблиці — 3072 слова);
13 типів мікросхем у таблиці (з free-pdk 1.3)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Принцип / обмеження | Як виконується | До дизайну | Після дизайну |
|---|---|---|---|
| I. Розумний програматор, тонкий клієнт | пакет без напруг/таймингів (FR-005); fuse, калібрування, серійний номер і валідація — у прошивці (R8); клієнт лише розбирає HEX і показує `LIST_CHIPS` | ✅ | ✅ |
| II. Автономна робота | пакет і стан у flash із CRC; перевірка перед кожним записом; `COPY` без PC; випадкові серійні номери без координації (R18) | ✅ | ✅ |
| III. Безпека (NON-NEGOTIABLE) | будь-який DONE через безпечний стан рушія; перевірка ID до HV у контракті `chip_engine`; обрив USB не перериває операцію (дані завантажено до старту; `RUN` зупиняється безпечно); невдале калібрування — далі нічого не записується; повторна спроба запису OTP лише після перевірки сумісності «тільки 1→0» з ефективним образом до подачі HV (задокументоване дописування); захисний fuse лише з `ALLOW_SECURITY_FUSE`; кожна зміна VDD/VPP — через `power_apply` з підтвердженням АЦП (FR-040, contracts/chip-engine.md); автоматична самоперевірка при старті блокує локальний запис до успіху (FR-041); коди `VDD_FAULT`/`VPP_FAULT` з виміряними значеннями; для OTP перевірка чистоти обов'язкова (`PKG_OTP_BLANK_CHECK_REQUIRED`), дописування — лише `PATCH` з перевіркою «тільки 1→0» до запису; еталон змінюється лише з `ALLOW_MODIFY_MASTER`, після комміту дампу, з відновленням і верифікацією та стійким прапорцем `MASTER_RESTORE_PENDING` | ✅ | ✅ |
| IV. Апаратна абстракція | ядро без регістрів МК; HAL: flash, CRC, транспорт, живлення (`hal_power`), органи керування (лише `local_start`/`ui_state`), `chip_engine`; TinyUSB переносний; розмітка flash від платформи; збірка `PLATFORM=host` | ✅ | ✅ |
| V. Версіонований протокол | CDC, явна довжина, CRC кожного кадру, `HELLO` з версіями, прогрес, коди помилок; контракт → `docs/protocol.md` | ✅ | ✅ |
| VI. Перевірюваність | модульні тести ядра на PC; контрактні тести з ін'єкцією помилок і обривів живлення на симуляторі; валідація на залізі в quickstart | ✅ | ✅ |
| Обмеження: C11, arm-none-eabi-gcc, CMake без IDE | так | ✅ | ✅ |
| Обмеження: клієнт кросплатформений, без драйверів | Python + pyserial, стандартний CDC | ✅ | ✅ |
| Обмеження: ліцензії | прошивка/клієнт GPL-3.0-or-later; TinyUSB MIT сумісна; код із free-pdk — з авторством | ✅ | ✅ |
| Обмеження: субмодули лише довідка | код (таблиця, вставки калібрування) переноситься в `software/`; збірка від субмодулів не залежить | ✅ | ✅ |

Порушень немає — Complexity Tracking не заповнюється.

## Project Structure

### Documentation (this feature)

```text
specs/001-pc-programmer-protocol/
├── spec.md
├── plan.md                 # цей файл
├── research.md             # Phase 0
├── data-model.md           # Phase 1
├── quickstart.md           # Phase 1
├── contracts/
│   ├── protocol.md         # кадри, команди, події, коди статусу
│   ├── package-format.md   # формат пакета прошивки
│   └── chip-engine.md      # внутрішні контракти: живлення, рушій, локальний запуск
├── checklists/requirements.md
└── tasks.md                # Phase 2 (/speckit-tasks)
```

### Source Code (repository root)

```text
software/
├── firmware/
│   ├── CMakeLists.txt                # PLATFORM=stm32f072 | host
│   ├── cmake/arm-none-eabi.cmake
│   ├── core/                         # платформонезалежний C11
│   │   ├── proto/                    # cobs.c, crc32.c, frame.c, session.c, dispatch.c
│   │   ├── package/                  # header.c, validate.c, upload.c, pkg_cmds.c, effective_image.c, image_crc.c, serial_rng.c
│   │   ├── storage/                  # slots.c, journal.c, package_store.c, stats_cmds.c, config_cmds.c
│   │   ├── power/                    # power.c — політика встановлення напруг (FR-040)
│   │   ├── ops/                      # op_manager.c, op_cmds.c, op_program.c, op_simple.c, op_patch.c, op_read.c, op_copy.c,
│   │   │                             #   op_restore.c, op_selftest.c, op_run.c, local_start.c, indication.c
│   │   └── chips/                    # chip_table.c, calib_sites.c, calib_footprint.c, serial_sites.c
│   ├── hal/                          # hal_flash.h, hal_crc.h, hal_transport.h, hal_power.h, hal_entropy.h, hal_ui.h, chip_engine.h
│   ├── platform/
│   │   ├── stm32f072/                # ціль налагодження: startup, linker (розмітка flash), LL, TinyUSB CDC, hal_*.c
│   │   ├── <main-mcu>/               # основна МК — наступна специфікація
│   │   └── host/                     # mock flash з «обривом живлення», модель живлення (з несправностями), модель мікросхеми, pdk-sim (stdio/TCP)
│   └── tests/                        # модульні тести ядра (CTest)
├── client/
│   ├── pyproject.toml
│   ├── padauk_prog/
│   │   ├── transport.py              # serial / tcp://
│   │   ├── framing.py                # COBS, CRC, кадри, stop-and-wait
│   │   ├── protocol.py               # команди, події, коди статусу
│   │   ├── package.py                # побудова пакета (Header, маска, CRC)
│   │   ├── ihex.py                   # Intel HEX → слова
│   │   └── cli.py                    # pdkprog: info/list/probe/read/write/erase/start/stats
│   └── tests/
│       ├── unit/
│       └── contract/                 # проти pdk-sim, ін'єкція помилок
└── tests/vectors/                    # спільні тестові пакети й кадри для C і Python
docs/
└── protocol.md                       # опублікована версія contracts/ (FR-029)
```

**Structure Decision**: прошивка і клієнт — окремі підпроєкти в `software/`; спільні тестові вектори в
`software/tests/vectors/` гарантують, що C- і Python-реалізації кадрів і пакета побітово сумісні.
Рушій мікросхем підключається через `hal/chip_engine.h`: у цій фічі — модель для симулятора й
заглушка для STM32; реальні алгоритми PADAUK — фіча `002-chip-engine` (R14).

## Complexity Tracking

Не потрібно — порушень конституції немає.
