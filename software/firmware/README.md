# Прошивка програматора

Ядро на C11 без залежностей від заліза (`core/`), інтерфейси заліза (`hal/`) і платформи (`platform/`):

| Платформа | Що це | Рушій мікросхем |
|---|---|---|
| `host` | симулятор `pdk-sim` (протокол через TCP) і модульні тести | модель мікросхеми `platform/host/chip_model.c` |
| `stm32f072` | ціль налагодження: плати Easy PDK Programmer | заглушка (`OP_UNSUPPORTED`; фіча `002-chip-engine`) |
| `stm32g474` | плата WeAct STM32G474 + `hardware/stm32g474-board` | заглушка |

Протокол і формат пакета — [`docs/protocol.md`](../../docs/protocol.md).

## Збірка

Потрібні CMake ≥ 3.20 і gcc/clang (на Windows — MinGW-w64; MSVC не підтримується). Для STM32 —
`arm-none-eabi-gcc` ≥ 12. CMSIS і TinyUSB завантажуються під час конфігурації (FetchContent, фіксовані
версії), у репозиторії їх немає.

```sh
# симулятор і модульні тести
cmake -S software/firmware -B build/host -G Ninja -DPLATFORM=host
cmake --build build/host
ctest --test-dir build/host --output-on-failure

# STM32F072 (Easy PDK)
cmake -S software/firmware -B build/f072 -G Ninja -DPLATFORM=stm32f072 -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/software/firmware/cmake/arm-none-eabi.cmake
cmake --build build/f072          # → build/f072/pdkprog.bin

# STM32G474 (WeAct)
cmake -S software/firmware -B build/g474 -G Ninja -DPLATFORM=stm32g474 -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/software/firmware/cmake/arm-cortex-m4f.cmake
cmake --build build/g474          # → build/g474/pdkprog-g474.bin
```

Опції збірки:

| Опція | Типово | Що робить |
|---|---|---|
| `PDK_FEATURE_MEASURE_MASTER` | `ON` | `COPY` у режимі `MEASURE_MASTER` (біт 9 `INFO.capabilities`; вимкнено → режим 3 дає `OP_UNSUPPORTED`) |
| `PDK_FEATURE_PATCH` | `ON` | операція `PATCH` (біт 6) |
| `PDK_G474_SUPPLIES` | `ON` | джерела VDD/VPP плати `hardware/stm32g474-board`; `OFF` — голий модуль WeAct без живлення |

Після компонування збірка перевіряє розмір (`cmake/check_size.cmake`): `.text + .data` понад 44 КБ на F072
або 256 КБ (банк 1) на G474 — помилка з порадою вимкнути функції. Зараз F072 займає близько 28 КБ
(без `MEASURE_MASTER` і `PATCH` — близько 26 КБ).

## Прошивка через DFU

Обидві STM32 мають вбудований завантажувач USB DFU (`0483:df11`): утримати BOOT0, натиснути й відпустити
NRST, відпустити BOOT0.

```sh
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/f072/pdkprog.bin
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/g474/pdkprog-g474.bin
```

Після старту програматор — USB CDC `1209:0001`. На Windows 7 завантажувачу потрібен WinUSB (Zadig), а
COM-порту — [`software/drivers/windows/padauk-programmer.inf`](../drivers/windows/); Windows 10 і новіші
обходяться без драйверів. Подробиці G474 — [`platform/stm32g474/README.md`](platform/stm32g474/README.md).

## Розмітка flash

Пакет зберігається у слоті, активний слот і лічильники — у журналі станів (записи по 40 байт із власним CRC у
двох сторінках, research R6). Новий пакет завжди пишеться в неактивний слот і стає активним одним записом
журналу, тож обрив живлення лишає або старий, або новий пакет.

| Область | STM32F072 (64 КБ) | STM32G474 (512 КБ, DBANK = 1) |
|---|---|---|
| прошивка | `0x08000000` – `0x0800AFFF` (44 КБ) | банк 1: `0x08000000` – `0x0803FFFF` |
| слоти пакетів | 2 × 8 КБ: `0x0800B000`, `0x0800D000` | 4 × 16 КБ: `0x08040000`, `0x08044000`, `0x08048000`, `0x0804C000` |
| журнал станів | `0x0800F000`, `0x0800F800` (по 2 КБ) | `0x08050000`, `0x08051000` |

На G474 сховище в банку 2: стирання не зупиняє процесор (read-while-write). Розмір слота визначає
`INFO.max_package_size`.

## Симулятор `pdk-sim`

```sh
build/host/pdk-sim --tcp 127.0.0.1:5555 --chip PFS154 --chip-state chip.bin
pdkprog --port tcp://127.0.0.1:5555 info
```

| Параметр | Що робить |
|---|---|
| `--flash FILE` | емульований flash програматора у файлі (переживає перезапуск) |
| `--chip none \| NAME[:опції]` | мікросхема в панельці; опції моделі: `offset=±N` (зсув генератора, %), `osc=broken`, `weak=ADDR`, `order=reverse`, `deaf`, `pa0=float`, `slow=US`, `protected`, `halt=ADDR` (програма не доходить далі ADDR) |
| `--chip-state FILE` | пам'ять мікросхеми у файлі |
| `--power-fail-after N` | «вимкнути живлення» на N-й операції запису flash або мікросхеми (код виходу 99) |
| `--power-fault RAIL:MODE`, `--power-scale RAIL:OUT:MEAS` | несправне або неточне джерело VDD/VPP |
| `--corrupt-flash-during-write` | збої читання flash під час запису мікросхеми |

Команди зі stdin (органи керування програматора): `local-start program|copy|verify|selftest|restore`,
`local-cancel`, `swap-chip SPEC` (вставити іншу, чисту мікросхему). Модель мікросхеми в `RUN` шле `'U'` і
`"hello\n"`, потім повертає ехом усе отримане; кожне стирання й байт `UART_TX` видно в stderr.
