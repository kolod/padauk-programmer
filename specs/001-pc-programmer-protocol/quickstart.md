# Quickstart: перевірка протоколу PC ⇄ програматор

Сценарії, що доводять роботу фічі. Шляхи — відповідно до структури в [plan.md](plan.md).
Формати — [contracts/](contracts/).

## Передумови

- CMake ≥ 3.20, хост-компілятор C11 (gcc/clang/MSVC), `arm-none-eabi-gcc` ≥ 12 (для прошивки)
- Python ≥ 3.10, `pip install -e software/client[dev]` (pyserial, pytest)
- Для сценаріїв на залізі: ціль налагодження — плата Easy PDK (STM32F072), PFS154 у панельці

## 1. Модульні тести ядра (без заліза)

```sh
cmake -S software/firmware -B build/host -DPLATFORM=host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Очікування: усі тести пройдено, зокрема COBS, CRC-32 (еталон `"123456789"` → `0xCBF43926`), розбір
кадрів, валідація пакетів на векторах із `software/tests/vectors/`, атомарність журналу й слотів.

## 2. Клієнт проти симулятора

```sh
build/host/pdk-sim --tcp 127.0.0.1:5555 --chip PFS154 &
pdkprog --port tcp://127.0.0.1:5555 info
pdkprog --port tcp://127.0.0.1:5555 list
pdkprog --port tcp://127.0.0.1:5555 -n PFS154 write software/tests/vectors/helloworld_pfs154.ihx
```

Очікування: `info` показує протокол 1.0 і `max_package_size`; `list` — таблицю з прошивки;
`write` показує етапи `PKG_CHECK → ID → ERASE → BLANK_CHECK → WRITE → VERIFY → CALIBRATE → FUSE`,
прогрес щонайменше раз на секунду й підсумок `OK` з результатом калібрування. CRC, показаний клієнтом після
завантаження, збігається з `PKG_INFO`.

## 3. Контрактні тести з ін'єкцією помилок (SC-001, SC-002, SC-007)

```sh
pytest software/client/tests/contract --sim build/host/pdk-sim
```

| Тест | Що робить | Очікування |
|---|---|---|
| `test_frame_corruption` | 1000 завантажень максимального пакета, ≥ 1 % кадрів у кожному напрямі пошкоджено/втрачено/продубльовано | 100 % збережених пакетів ідентичні вихідному |
| `test_stored_corruption` | по черзі пошкоджує кожен байт збереженого пакета в емульованому flash | `PROGRAM` завжди → `STORE_CORRUPTED`, мікросхема-модель не змінена |
| `test_power_loss` | ≥ 100 «вимкнень живлення» в довільній операції запису flash під час завантаження | після «перезапуску» активний або старий, або новий пакет, `pkg_state = VALID` |
| `test_duplicate_seq` | повторює запит із тим самим `seq` | команда виконується один раз, відповідь та сама |
| `test_busy_cancel` | команди під час `PROGRAM`, потім `OP_CANCEL` | `BUSY` для заборонених, `CANCELLED` з етапом |
| `test_disconnect` | ≥ 100 разів закриває порт у довільний момент `PROGRAM` (SC-008); те саме під час `RUN` | `PROGRAM` завершується, вміст моделі = як без обриву, `OP_RESULT(0)` після перепідключення — `OK`; `RUN` → `HOST_DISCONNECTED`, напруги зняті |
| `test_otp_retry` | ≥ 100 «обривів живлення» в довільній точці `PROGRAM` OTP-моделі PMS150C зі збереженням стану моделі, потім повторний `PROGRAM` тим самим і «іншим» симулятором (окремий `--flash`, той самий пакет) (SC-012, FR-046) | `OK` + `RESUMED` або `CHIP_NOT_BLANK` без змін моделі; жодного зайвого нуля; повністю записана → `CHIP_ALREADY_PROGRAMMED`, лічильники без змін; уже записаний серійний номер зберігається; чиста вставка отримує новий випадковий номер |
| `test_calib_fail` | модель з «несправним» осцилятором → `PROGRAM`; локальний запуск; з PC з `CALIB_FINALIZE_ON_FAIL`; потім «ремонт» і повторний `PROGRAM` (SC-013) | `CALIB_FAILED`, після циклу нічого не записано, fuse не записаний; локальний запуск прапорець ігнорує; з прапорцем — `CALIB_FORCED`; повторний запис → `OK` + `RESUMED` |
| `test_copy_measure` (кілька слідів) | еталон з IHRC + ILRC + BG у `MEASURE_MASTER` для обох RC; варіант зі слідом, до якого програма не доходить; обрив живлення між вимірами слідів (M7) | стирань еталона +3, обидві RC-цілі відновлено, BG — вставка; недосяжний слід → `COPIED` з попередженням, решта оброблена; після обриву — `MASTER_RESTORE_PENDING` і повне відновлення |
| `test_copy_special` (цикл в еталоні) | еталон з незавершеним калібруванням (вимірювальний цикл): локальне `COPY`; `COPY` з PC без дозволу; потім з `ALLOW_MASTER_LOOP` + `TARGET_FROM_HOST` (FR-051) | локально і без дозволу — `COPY_MASTER_UNCALIBRATED`, `PKG_INFO` без змін; з дозволом — новий пакет із вставкою на місці циклу, ціль = задана |
| `test_copy_special` (чужий формат, серійний номер) | еталон із записом константи в `IHRCR` без сліду free-pdk і з серійним номером free-pdk (FR-052, FR-053) | `COPY` → `OK`, код скопійовано як є, `warnings` = `CALIB_UNRECOGNIZED` (+ адреса) і `SERIAL_DUPLICATED` |
| `test_verify_final` (VERIFY з урахуванням вставок) | `VERIFY` каліброваної мікросхеми з серійним номером; мікросхеми з незавершеним калібруванням (FR-047) | перша → `OK`, `calib[].value` і `serial_written` з мікросхеми; друга → `VERIFY_FAILED`, `site_status = FAILED` |
| `test_verify_final` (FINAL_VERIFY, потоковий CRC) | модель «слабко» записує слово завершення вставки або fuse (FR-048); `pdk-sim --corrupt-flash-during-write` (FR-049); пакет без калібрувальних вставок (FR-050) | `VERIFY_FAILED` на `FINAL_VERIFY` з адресою; `STORE_CORRUPTED` до запису fuse; `PKG_COMMIT.calib_sites = 0` і попередження клієнта |
| `test_upload` (невалідні вектори) | вектори з помилками заголовка, CRC, службових областей, fuse, вставок | точні коди з контракту та `addr` |
| `test_latency` | вимірює час кожного RSP, зокрема `PKG_DATA` на межі сторінки flash, `PKG_COMMIT`, запис журналу з переходом на іншу сторінку (FR-024) | усі відповіді ≤ 100 мс |
| `test_selftest` | `pdk-sim --chip none` → `SELF_TEST`; `--chip PFS154` → `SELF_TEST`; перезапуск симулятора | `OK` з усіма рівнями; `SELFTEST_CHIP_PRESENT` без рівнів вище probe; після старту `STATUS.selftest_state = 2` |
| `test_power_fault` | `pdk-sim --power-fault vpp:slow\|vpp:stuck\|vdd:low` → `PROGRAM`, `SELF_TEST` (SC-011) | `VPP_FAULT`/`VDD_FAULT` ≤ 20 мс після зміни, `RESULT.v_*` заповнено, модель мікросхеми без змін; після старту з несправністю локальний запуск → `SELF_TEST`, а не `PROGRAM` |

## 4. Копіювання з відновленням калібрування (симулятор, SC-009, SC-010)

```sh
# термінал 1 (симулятор приймає команди з stdin):
build/host/pdk-sim --tcp 127.0.0.1:5555 --chip PFS154:offset=+3% --chip-state master.bin
# термінал 2:
pdkprog --port tcp://127.0.0.1:5555 -n PFS154 write software/tests/vectors/helloworld_pfs154.ihx   # еталон, IHRC 8 МГц
pdkprog --port tcp://127.0.0.1:5555 copy --ihrc measure --allow-modify-master
# у консолі pdk-sim (stdin): swap-chip PFS154:offset=-4%     — «вставити» чисту мікросхему-копію
pdkprog --port tcp://127.0.0.1:5555 program
pytest software/client/tests/contract/test_copy_measure.py software/client/tests/contract/test_copy_power_loss.py --sim build/host/pdk-sim
```

Очікування: `copy` показує етапи `ID → READ → STORE → MEASURE → RESTORE → STORE`, у підсумку
`IHRC master_value=… target≈8 000 000 Hz mode=MEASURE_MASTER`, попередження `MASTER_MODIFIED`;
дамп еталона до й після однаковий; `program` копії зі зсувом −4 % дає частоту в межах 1 % від еталона.
Обрив живлення під час `MEASURE`/`RESTORE` → після перезапуску `pdkprog status` показує
`MASTER_RESTORE_PENDING`, `pdkprog restore-master` повертає еталон.

## 5. На залізі (ціль налагодження STM32F072 + PFS154)

```sh
cmake -S software/firmware -B build/f072 -DPLATFORM=stm32f072 -DCMAKE_TOOLCHAIN_FILE=software/firmware/cmake/arm-none-eabi.cmake
cmake --build build/f072          # перевірити, що розмір прошивки ≤ 44 КБ
dfu-util -d 0483:df11 -a 0 --dfuse-address 0x08000000 -D build/f072/pdkprog.bin
pdkprog info
pdkprog selftest          # без мікросхеми: таблиця рівнів VDD/VPP, задано / виміряно / час
pdkprog -n PFS154 write helloworld_pfs154.ihx
pdkprog stats
```

Потім **без PC** (живлення від USB-зарядки): вставити PFS154, тричі виконати локальний запуск (на поточній платі — кнопкою) для трьох мікросхем.
Підключити PC: `pdkprog stats` → `ok = 4`; для прошивки зі вставкою серійного номера — `pdkprog verify` на кожній мікросхемі показує різні серійні номери.

Запис реальних мікросхем залежить від фічі `002-chip-engine`; до неї сценарій 5 перевіряє лише
протокол, сховище й автономну перевірку цілісності (з рушієм-заглушкою).
