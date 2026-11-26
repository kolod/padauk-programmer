# pdkprog — клієнт програматора PADAUK

Клієнт на Python ≥ 3.8 (Windows 7 і новіші, Linux, macOS). Уся інформація про мікросхеми — з прошивки
програматора (`LIST_CHIPS`), тож нова мікросхема не потребує нового клієнта. Протокол —
[`docs/protocol.md`](../../docs/protocol.md).

## Встановлення

```sh
pip install -e software/client          # з репозиторію; залежність — pyserial
pip install -e "software/client[dev]"   # + pytest для тестів
```

Порт знаходиться сам (USB `1209:0001`); інакше `-p COM5`, `-p /dev/ttyACM0` або `-p tcp://127.0.0.1:5555`
для симулятора `pdk-sim` ([`software/firmware/README.md`](../firmware/README.md)). На Windows 7 COM-порту
потрібен [`software/drivers/windows/padauk-programmer.inf`](../drivers/windows/).

## Підкоманди

| Команда | Приклад | Що робить |
|---|---|---|
| `info` | `pdkprog info` | версії, межі, можливості збірки, стан пакета й самоперевірки |
| `list` | `pdkprog list` | мікросхеми з таблиці прошивки |
| `write` | `pdkprog -n PFS154 write hello.ihx` | зберегти пакет і записати мікросхему (`upload` + `program`) |
| `upload` | `pdkprog upload -n PFS154 hello.ihx --fuse 0x3FFD --runvdd 3.3` | лише зберегти пакет у програматорі (далі — локальний запуск без PC) |
| `program` | `pdkprog program` | записати мікросхему збереженим пакетом |
| `verify` | `pdkprog verify` | порівняти мікросхему з пакетом (з урахуванням калібрування й серійного номера) |
| `probe` | `pdkprog probe` | визначити мікросхему в панельці |
| `erase`, `blankcheck` | `pdkprog erase -n PFS154` | стерти FLASH / перевірити чистоту |
| `read` | `pdkprog read -n PFS154 dump.hex` (`-b` — сирий bin) | прочитати мікросхему у файл, з перевіркою CRC |
| `copy` | `pdkprog copy --ihrc snap --bg restore` | скопіювати мікросхему в новий пакет, відновивши калібрування |
| `restore-master` | `pdkprog restore-master` | повернути еталон, якщо `copy --ihrc measure` перервано |
| `start` | `pdkprog start -r 3.3` | запустити мікросхему з UART-мостом: текст від неї на екран, клавіатура до неї, Esc — стоп |
| `patch` | `pdkprog patch -n PFS154 --expect-package 0x1F0=0x0123` | дописати окремі слова (лише 1 → 0) після перевірки вмісту |
| `pkg` | `pdkprog pkg info` / `read f.bin` / `delete` / `check` | збережений пакет |
| `status`, `monitor` | `pdkprog monitor --count 3` | стан; операції наживо, зокрема локальні (кнопка, конвеєр) |
| `stats` | `pdkprog stats` / `stats reset` | лічильники вдалих і невдалих записів |
| `config` | `pdkprog config copy --ihrc snap --bg restore` | режими локального `COPY` |
| `selftest` | `pdkprog selftest` | перевірити всі рівні VDD/VPP з порожньою панелькою |
| `calibrate`, `supply` | `pdkprog supply 5 8.5 --hold 30` | калібрування джерел за зовнішнім мультиметром; довільні VDD/VPP для перевірки (нічого не підключено!) |

`write`, `program` та інші операції показують етапи й прогрес, у кінці — результат калібрування, серійний
номер і суми вмісту мікросхеми. Код виходу: 0 — успіх, 1 — помилка операції, 3 — немає зв'язку.

### `copy`

Режими для кожного генератора: `--ihrc keep|target|snap|measure`, `--ilrc keep|target|measure`,
`--bg keep|restore` (типово `restore`); `target` потребує `--ihrc-freq` / `--ilrc-freq`.

- `keep` — копії отримують значення підстроювання еталона як є (розкид генераторів лишається);
- `target` — копії калібруються до заданої частоти;
- `snap` — до найближчої стандартної (16, 8, 4, 2, 1 МГц, 500 кГц), якщо копія при значенні еталона в межах 10 %;
- `measure` — до фактичної частоти еталона: програматор перезаписує FLASH-еталон по разу на кожну вставку і ще
  раз для відновлення (`--allow-modify-master`, ресурс стирань). Обрив — `pdkprog restore-master`.

Якщо калібрування в еталоні не завершене (вимірювальний цикл), `copy` запропонує скасувати або задати ціль.

## Відповідність `easypdkprog`

| `easypdkprog` | `pdkprog` |
|---|---|
| `list`, `probe` | те саме |
| `-n X read f` | те саме (Intel HEX; `-b` — bin) |
| `-n X write f [--fuse --allowsecfuse --securefill --noerase --noblankchk --noverify]` | те саме; пакет зберігається в програматорі й пишеться далі без PC |
| `-n X erase` | те саме |
| `start [-r V]` | те саме (`-r` у вольтах; Esc зупиняє) |
| `--nocalibrate` | не підтримується: приберіть макрос калібрування з програми |
| `--serial` | не потрібен: кожна вставка `EASY_PDK_SERIALNUM` отримує випадковий 64-бітний номер |

Порядок аргументів як в `easypdkprog` (`-n X write f`) і звичний (`write -n X f`) рівнозначні.
`--noblankchk` для OTP клієнт відхиляє: повторний запис OTP можливий лише після перевірки сумісності вмісту.

## Калібрування й серійний номер

- Макроси `EASY_PDK_CALIBRATE_IHRC/ILRC/BG` з `easypdk/calibrate.h` лишаються в програмі: програматор
  знаходить вставку, записує замість неї вимірювальний цикл, калібрує й дописує результат. Прибирати їх не
  треба; програма без вставок записується з попередженням.
- `EASY_PDK_SERIALNUM` дає кожній мікросхемі власний випадковий номер; повторний запис OTP зберігає вже
  записаний. `copy` еталона з номером копіює номер як є (попередження `SERIAL_DUPLICATED`).
- Калібрування не вдалося — вставка лишається вимірювальним циклом, мікросхему можна дописати пізніше
  (`program` дасть `RESUMED`); `--finalize-on-calib-fail` записує найкраще знайдене значення.

## Тести

```sh
pytest software/client/tests/unit
pytest software/client/tests --sim build/host/pdk-sim     # + контрактні проти симулятора
PDK_FAST=1 pytest software/client/tests --sim build/host/pdk-sim   # менше ітерацій ін'єкції помилок
```
