# Data Model: Протокол обміну PC ⇄ програматор

Байтові формати на лінії — у [contracts/protocol.md](contracts/protocol.md) і
[contracts/package-format.md](contracts/package-format.md). Тут — сутності, їхні зв'язки, правила і стани.

## Сутності

### Frame (кадр)
`type`, `flags`, `seq`, `code`, `status`, `len`, `payload`, `crc32`.
- Валідний ⇔ COBS декодовано, `len` = фактична довжина − 14, `len ≤ max_payload`, CRC збігається.
- Невалідний → `NAK`, не виконується.

### Session (сесія порту)
| Поле | Опис |
|---|---|
| `open` | DTR високий і USB сконфігуровано |
| `established` | було успішне `HELLO` у цій сесії |
| `client_proto` | версія клієнта з `HELLO` |
| `last_req_seq`, `last_rsp` | кеш для повтору (FR-009) |
| `evt_seq` | лічильник подій |

Зміна `open` → скидання `established`, кешу й скасування незавершеного завантаження. Поточна операція з мікросхемою **не** переривається (FR-023), окрім `RUN` (`HOST_DISCONNECTED`).

### Package (пакет прошивки)
Header + Words[N] + DefinedMask + `package_crc32` (див. контракт).
Інваріанти після комміту: `package_crc32` валідний; усі перевірки `PKG_COMMIT` пройдено; посилається на
рівно один `ChipDescriptor`.

### Upload (завантаження)
| Поле | Опис |
|---|---|
| `slot` | неактивний слот |
| `expected_size` | `package_size` із заголовка `PKG_BEGIN` |
| `received` | прийнято байтів |
| `header_copy` | заголовок з `PKG_BEGIN` |
| `erased_pages` | сторінки слота, вже стерті |

Стани:

```
IDLE ──PKG_BEGIN ok──► RECEIVING ──PKG_DATA (received == expected)──► COMPLETE ──PKG_COMMIT ok──► IDLE (новий пакет активний)
  ▲                       │  │                                           │
  └──── PKG_ABORT / помилка / обрив сесії / PKG_COMMIT помилка ──────────┴──► IDLE (активний пакет без змін)
```

### StorageSlot (слот flash)
Область фіксованого розміру (F072: 8 КБ, 4 сторінки). Вміст інтерпретується **лише** якщо на слот
вказує активний `StateRecord`. Неактивний слот — буфер завантаження або результату `READ`.

### StateRecord (запис журналу стану, 40 байт)
| Тип | Поле | Опис |
|---|---|---|
| `u32` | `magic` | `"PDKS"` |
| `u32` | `rec_seq` | монотонний номер запису |
| `u8` | `active_slot` | 0, 1 або `0xFF` — пакета немає |
| `u8` | `cfg_copy_ihrc` | режим IHRC для локального `COPY` (0 або 2), за замовчуванням 0 |
| `u16` | `flags` | біт0 `MASTER_RESTORE_PENDING` — активний пакет є дампом еталона, який змінено й не відновлено; біт15 — `supply_cal` дійсне |
| `u32` | `package_size` | |
| `u32` | `package_crc32` | |
| `i8[4]` | `supply_cal` | калібрування живлення в проміле: вимір VDD, вимір VPP, вихід VDD, вихід VPP (contracts/protocol.md, `CAL_*`); без біта15 у `flags` — `0xFFFFFFFF` (колишній лічильник серійних номерів), записи старих версій читаються як «не калібровано» |
| `u32` | `ok_count` | |
| `u32` | `fail_count` | |
| `u8` | `cfg_copy_ilrc` | режим ILRC для локального `COPY` (0), за замовчуванням 0 |
| `u8` | `cfg_copy_bg` | 1 — відновлювати BandGap при локальному `COPY` (за замовчуванням), 0 — переносити |
| `u16` | `reserved` | `0xFFFF`; доповнює запис до 40 Б (кратно 8 — для МК, що програмують flash подвійними словами) |
| `u32` | `rec_crc32` | CRC байтів 0–35 (усього запис 40 Б, 51 запис на сторінку 2 КБ) |

- Чинний запис — валідний (magic + CRC) з найбільшим `rec_seq` серед двох сторінок журналу.
- Новий запис дописується: після комміту (новий `active_slot`), після кожного
  завершеного запису мікросхеми (`ok/fail`), після `PKG_DELETE`, `STATS_RESET`,
  `CONFIG_SET`, встановлення й зняття `MASTER_RESTORE_PENDING`.
- Поки `MASTER_RESTORE_PENDING = 1`, `PKG_BEGIN`, `PKG_DELETE` і `COPY` відхиляються
  (`MASTER_RESTORE_PENDING`), щоб не втратити дамп; дозволено `RESTORE_MASTER`, `PKG_READ`, `PROBE`.
- Сторінка заповнена → стерти іншу сторінку, записати туди копію чинного запису, далі писати там.

### PackageState (обчислюється при старті і перед кожним `PROGRAM`/`VERIFY`)
| Значення | Умова |
|---|---|
| `EMPTY` | немає валідного `StateRecord` або `active_slot = 0xFF` |
| `VALID` | CRC пакета в слоті = `package_crc32` запису і = CRC у трейлері пакета |
| `CORRUPTED` | інакше; автономний і будь-який запис заблоковано (`STORE_CORRUPTED`) |

### ChipDescriptor (таблиця в прошивці)
Публічна частина (`CHIP_DESC`): ID, сімейство, обсяг, розрядність, RAM, доступні операції, службові
області, назви. **Внутрішня** частина — напруги, параметри блоку запису, кількість імпульсів стирання,
таблиці опкодів калібрування — клієнту не передається (принцип I).

### EffectiveImagePlan (RAM, на час `PROGRAM`/`VERIFY`)
| Поле | Опис |
|---|---|
| `fuse` | значення fuse або «не писати» |
| `calib_sites[≤16]` | `{addr, kind, target_hz, mV, regs}` |
| `serial_sites[≤4]` | адреси вставок серійного номера |
| `fill_nop_until` | межа `SECURE_FILL` |

Слово для адреси `a` = функція(слово/маска пакета, план, фаза калібрування, поточний серійний номер).
Будується тим самим кодом, що й валідація `PKG_COMMIT`.

### Operation (операція)
| Поле | Опис |
|---|---|
| `op_id` | u16, зростає |
| `op_type`, `source` | |
| `stage`, `stage_index`, `stage_count`, `percent` | для `EVT_PROGRESS` |
| `cancel_requested` | |
| `result` | `RESULT` після завершення |

```
IDLE ──OP_START / local_start──► RUNNING ──кроки рушія──► DONE(status)
                               │
                               ├─ OP_CANCEL ─────────► DONE(CANCELLED)
                               └─ обрив сесії ───────► продовжується (лише RUN → DONE(HOST_DISCONNECTED))
```
- Будь-який перехід у DONE MUST проходити через «безпечний стан» рушія: VDD = VPP = 0, лінії IC — Hi-Z.
- Обрив сесії не скасовує жодну операцію, крім `RUN`; результат зберігається для `OP_RESULT(0)`.
- Для `COPY` в етапі `MEASURE` скасування означає «відновити еталон і завершити»
  (див. `CopyJob`), а не негайну зупинку.
- Локальний запуск перетворюється на операцію за пріоритетом (research R16): самоперевірку не
  пройдено → `SELF_TEST`; `MASTER_RESTORE_PENDING = 1` → `RESTORE_MASTER`; інакше запитана операція
  (`PROGRAM` або `COPY` з режимами `cfg_copy_*`).
- Лише одна операція одночасно; під час `RECEIVING` операції заборонені (`BUSY`).
- Після `DONE` для `PROGRAM`: записується `StateRecord` (ok/fail); `CHIP_ALREADY_PROGRAMMED` лічильників не змінює.

### ProgramJob (операція `PROGRAM`)

```
PKG_CHECK → ID → ERASE (FLASH) → BLANK_CHECK ─┬─ чиста ─────────────► WRITE* → VERIFY → CALIBRATE(кожна вставка) → PKG_CHECK → FUSE → FINAL_VERIFY → DONE(OK)
                                              ├─ OTP, сумісна ──────► WRITE*(решта) → VERIFY → CALIBRATE(незавершені) → PKG_CHECK → FUSE → FINAL_VERIFY → DONE(OK, RESUMED)
                                              ├─ OTP, уже все ──────► DONE(CHIP_ALREADY_PROGRAMMED)
                                              └─ несумісна ─────────► DONE(CHIP_NOT_BLANK, addr)
CALIBRATE невдалий ─► (CALIB_FINALIZE_ON_FAIL і source = 0 ? завершити найкращим k, далі як OK, warnings.CALIB_FORCED
                                                            : зупинитися, нічого не писати) → DONE(CALIB_FAILED)
```

`WRITE*` — під час запису CRC пакета рахується потоково з даних, які пишуться (FR-049). `VERIFY` —
з урахуванням вставок (вставка калібрування = вимірювальний цикл); `FINAL_VERIFY` — точне порівняння
всієї пам'яті з образом, у який підставлено фактичні `k`, серійний номер і fuse (FR-048); з нього ж
`RESULT.image_crc_raw/norm`.

Сумісність слова для OTP: `(вміст AND ціль) == ціль`; для слів калібрувальної вставки ціллю може бути
будь-який із трьох станів (чиста, вимірювальний цикл, завершена форма з довільним `k`); вставка
серійного номера — чиста (буде записано новий випадковий номер) або вже записана (номер береться з мікросхеми). Перевірка не
використовує жодного стану програматора, тож повторна спроба можлива пізніше й іншим програматором.

Серійний номер — випадкове 64-бітне число, згенероване для кожної мікросхеми перед етапом `WRITE`
(research R18); стану між мікросхемами немає.

### CalibFootprint (слід калібрування в дампі)
| Поле | Опис |
|---|---|
| `addr` | позиція першого слова сліду |
| `kind` | IHRC / ILRC / BG — за адресою регістра з `ChipDescriptor` |
| `master_value` | `k` з `MOV A,#k` |
| `target_hz`, `mV` | визначаються режимом `COPY` (research R9) |

`ChipDescriptor` (внутрішня частина) містить адреси `IHRCR`, `ILRCR`, `BGTR`, `GPCC`, `GPCS` і номінал
ILRC — потрібні для розпізнавання слідів і відновлення вставок.

### CopyJob (операція `COPY`)

```
ID → READ → [вимірювальний цикл у дампі?] ── так, без ALLOW_MASTER_LOOP ──► DONE(COPY_MASTER_UNCALIBRATED), сховище без змін
           → [сліди знайдено?]
   ├─ ні / KEEP_VALUE ───────────────────────────────────────────► STORE(пакет KEEP_VALUE) → DONE
   ├─ TARGET_FROM_HOST / SNAP_STANDARD ──────────────────────────► STORE(пакет з вставками) → DONE
   └─ MEASURE_MASTER (FLASH + ALLOW_MODIFY_MASTER):
        STORE(дамп, KEEP_VALUE) → журнал: PENDING=1
        → MEASURE(слід 1) … MEASURE(слід n)        (для кожного сліду в режимі 3: стирання + запис дампу,
                                                     де лише цей слід — цикл, решта як в еталоні; вимір;
                                                     немає сигналу за 1 с → k еталона, site_status = COPIED)
        → RESTORE (стирання + запис дампу + VERIFY) → журнал: PENDING=0
        → STORE(пакет з вставками, ціль = f_master) → DONE
   Стирань еталона = n + 1 (n — сліди в режимі 3; BG не вимірюється).
   Помилка вимірювання або OP_CANCEL під час MEASURE → спершу RESTORE, потім DONE(код);
   обрив сесії нічого не перериває; невдалий RESTORE або обрив живлення →
   PENDING лишається → потрібна RESTORE_MASTER
```

### ImageChecksum (суми вмісту мікросхеми)
| Поле | Опис |
|---|---|
| `image_crc_raw` | CRC-32 користувацької пам'яті як є (без службових областей, біти вище `code_bits` = 0) |
| `image_crc_norm` | те саме після нормалізації: `k` у слідах калібрування й `imm` у вставках серійного номера → 0 |

Обчислюється потоково під час читання мікросхеми (без RAM-копії) або з ефективного образу пакета
тим самим нормалізатором. Використовується `PATCH` (обов'язкова перевірка перед записом і після) і
повертається в `RESULT` операцій, що читають мікросхему.

### PatchJob (операція `PATCH`)
`ID → READ (уся пам'ять: суми + поточні значення адрес патча) → перевірка суми → перевірка 1→0 →
WRITE (лише змінені слова) → VERIFY (уся пам'ять: raw = очікуваний raw)`. Будь-яка невдала перевірка
до `WRITE` → DONE без жодного запису.

### SelfTestState (RAM, не зберігається)
| Поле | Опис |
|---|---|
| `state` | NOT_RUN / RUNNING / PASSED / FAILED / CHIP_PRESENT (`STATUS.selftest_state`) |
| `levels[]` | `{rail, set_mV, meas_mV, settle_us, ok}` останньої самоперевірки |

```
старт ─► SELF_TEST(source=1) ─┬─ PASSED ───────────► локальний запис дозволено
                              ├─ FAILED ───────────► LOCAL_START_BLOCKED (local_start → SELF_TEST)
                              └─ CHIP_PRESENT ─────► LOCAL_START_BLOCKED (local_start → SELF_TEST)
SELF_TEST з PC оновлює той самий стан.
```

### PowerFault (у `RESULT` при `VDD_FAULT`/`VPP_FAULT`)
`v_rail`, `v_set_mV`, `v_meas_mV` — шина, задана й остання виміряна напруга (contracts §6.1).

### ReadResult
Сирі слова в неактивному слоті + `data_words`, `data_crc32`, `chip_id12` у RAM. Знищується наступним
`PKG_BEGIN`, `READ`, `COPY`.

## Зв'язки

```
Session 1──0..1 Upload ──► StorageSlot (неактивний)
StateRecord ──active_slot──► StorageSlot ──містить──► Package ──chip_id12──► ChipDescriptor
Operation(PROGRAM) ──використовує──► Package + EffectiveImagePlan ──оновлює──► StateRecord
Operation(READ/COPY) ──пише──► StorageSlot (неактивний) ──COPY: комміт──► StateRecord
```
