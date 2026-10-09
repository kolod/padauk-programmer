# PADAUK Smart Programmer

Автономний програматор для мікроконтролерів PADAUK (PFS/PMS/PMC) з USB CDC-інтерфейсом і клієнтом на PC.
Натхненний проєктом [free-pdk / Easy PDK Programmer](https://free-pdk.github.io), але не є його копією.

## Структура

| Каталог | Вміст |
|---|---|
| `hardware/` | схема, плата, корпус |
| `software/` | прошивка програматора та клієнт на PC |
| `docs/` | документація, даташити, опис протоколів |
| `easy-pdk-programmer-*` | оригінальні проєкти free-pdk (git-субмодулі, лише для довідки) |

## Ліцензії

| Частина | Ліцензія | Файл |
|---|---|---|
| Апаратна частина (`hardware/`) | [CERN-OHL-S-2.0](https://ohwr.org/cern_ohl_s_v2.txt) | [LICENSE-HARDWARE](LICENSE-HARDWARE) |
| Програмне забезпечення (`software/`) | [GPL-3.0-or-later](https://www.gnu.org/licenses/gpl-3.0.html) | [LICENSE-SOFTWARE](LICENSE-SOFTWARE) |

Документація в `docs/`, створена в цьому проєкті, поширюється на умовах ліцензії тієї частини,
яку вона описує. Даташити PADAUK у `docs/` належать їхнім правовласникам і не підпадають під
ліцензії проєкту. Субмодулі `easy-pdk-programmer-*` мають власні ліцензії (CC-BY-SA-4.0 / GPL-3.0).

Джерело апаратного дизайну (Source Location згідно з CERN-OHL-S-2.0):
<https://github.com/kolod/padauk-programmer>
