# Первичные источники и границы выводов

Реестр объясняет, на каких версиях основаны технические решения. **Исходники** — вывод из закреплённого кода; **документация** — утверждение upstream; **эксперимент** — наблюдение на целевом устройстве; **гипотеза** — свойство ещё не подтверждено. Успешная компиляция не переводит гипотезу в аппаратный результат.

Результаты конкретных сборок собраны в [сводке проверок](validation.md). Лицензии и происхождение зависимостей — в [third_party](../third_party/README.md).

## Закреплённые версии

| Источник | Версия / commit |
| --- | --- |
| HomeSpan | 2.1.8, `107ffc07f4455754ea89068d8cf2e992de3583e6` |
| Arduino-ESP32 | 3.3.8, ESP-IDF 5.5.4 |
| Arduino CLI | 1.5.1, `01f3d4f2b` |
| Tion protocol | `dentra/esphome-tion`, `5d1c5b4b148b1f985874b105793ba4520dc1b429` |
| ESP32 USB disable | `dentra/esphome-components`, `b63386eba2beb545dd41027cdfdf9b08737ccafd` |
| LilyGO T-Dongle-S3 | `bb7654607d280fc2a1451d24abf6ed027287d416` |

Версии инструментов и чистота HomeSpan проверяются [сборочным скриптом](../scripts/toolchain.sh). Архив CLI сверяется с сохранённым [SHA-256 manifest](../third_party/arduino-cli-1.5.1.sha256). Полного lockfile каждого файла SDK нет; побайтовая воспроизводимость между ОС и каталогами не заявляется.

## HomeSpan и HomeKit

| Вывод | Источник | Статус и ограничение |
| --- | --- | --- |
| HomeSpan 2.1.8 поддерживает ESP32-S3, указывает core 3.3.8 как испытанный и требует HomePod/Apple TV для поддерживаемой работы | [README](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/README.md), Requirements | Документация этой версии; не рекомендация обновляться на произвольный новый core |
| `Fan`, `HeaterCooler`, `AirPurifier` и их характеристики задают стандартную модель сервисов | [ServiceList](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/docs/ServiceList.md), [Span.h](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/src/Span.h) | Исходники/документация; выбор для бризера описан в [ADR](adr-001-homekit.md), независимая HAP validation не завершена |
| Диапазон температуры можно расширить до −50…100 °C через `setRange` | [Пример наружного датчика](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/examples/12-ServiceLoops/DEV_Sensors.h) | Исходники; код проекта использует этот диапазон, отрицательные значения в Apple «Дом» аппаратно NOT RUN |
| `FilterMaintenance` связан с `AirPurifier`; `ResetFilterIndication` и `StatusFault` не показываются в Home App по описанию HomeSpan | [ServiceList](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/docs/ServiceList.md) | Документация; проект использует отдельный Switch для сброса, реальный сброс ресурса NOT RUN |
| WebLog — HTTP-страница без HAP-аутентификации; встроенная часть содержит идентификаторы | [HAP.cpp](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/src/HAP.cpp), `getStatusURL`; [Reference](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/docs/Reference.md), `enableWebLog` | Исходники и наблюдение; включается только в диагностическом варианте |
| При отсутствии verifier библиотека может создать общий setup code; `setPairingCode` не проверяет результаты записи NVS | [HAP.cpp](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/src/HAP.cpp), `HAPClient::init`; [HomeSpan.cpp](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/src/HomeSpan.cpp), `Span::setPairingCode` | Исходники; собственный [bootstrap](../firmware/TionHomeKit/PairingBootstrap.h) проверяет сохранение и останавливает запуск HAP при ошибке |
| HomeSpan инициализирует радио/NVS до `setup()`, а `begin()` содержит задержку 2 с | [HomeSpan.cpp](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/src/HomeSpan.cpp), `Span::Span`, `Span::init`, `Span::begin` | Исходники; UART проекта начинается до polling/provisioning, но не до всей инициализации радио |
| Command Mode выбирается удержанием кнопки 3 с; длительное удержание 10 с вызывает factory reset | [UserGuide](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/docs/UserGuide.md), Device Command Mode | Документация; GPIO0 задан кодом, кнопочная процедура на целевой плате NOT RUN |
| `resetIID(16)` сохраняет номера после удалённых IID 13–15; изменение базы повышает configuration number | [HomeSpan.cpp](https://github.com/HomeSpan/HomeSpan/blob/107ffc07f4455754ea89068d8cf2e992de3583e6/src/HomeSpan.cpp), `Span::resetIID`, `Span::updateDatabase` | Исходники, host-модель и обновление до GM1 с сохранением pairing; полная проверка базы отдельно не выполнена |

Setup AP имеет общеизвестный пароль `homespan`; проект ограничивает время работы AP до 300 с в [скетче](../firmware/TionHomeKit/TionHomeKit.ino). Случайный bootstrap verifier не является пользовательским кодом: собственный код задаётся в форме настройки каждого стика.

## Плата и UART

| Вывод | Источник | Статус и ограничение |
| --- | --- | --- |
| Базовая T-Dongle-S3: 16 МБ Flash, без PSRAM, BOOT GPIO0, APA102 GPIO39/40; ROM download с удержанием BOOT при подаче USB | [Руководство LilyGO](https://github.com/Xinyuan-LilyGO/T-Dongle-S3/blob/bb7654607d280fc2a1451d24abf6ed027287d416/docs/en/t-dongle-s3/REAMDE.MD) | Документация; ROM и Flash подтверждены экспериментом, маркировка/ревизия и PSRAM требуют отдельной проверки |
| Питание и USB-линии платы не определяют электрические свойства порта Tion | [Схема LilyGO](https://github.com/Xinyuan-LilyGO/T-Dongle-S3/blob/bb7654607d280fc2a1451d24abf6ed027287d416/schematic/T-Dongle-S3-QWIIC.pdf) | Документация/схема; уровни и запас питания Tion прибором не измерены |
| Upstream использует UART 9600, TX GPIO20, RX GPIO19 и отключение USB pads | [esp32_uart.yaml](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/packages/esp32_uart.yaml), [esp32_s3_uart.yaml](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/packages/esp32_s3_uart.yaml), [esp32_usb_dis.cpp](https://github.com/dentra/esphome-components/blob/b63386eba2beb545dd41027cdfdf9b08737ccafd/components/esp32_usb_dis/esp32_usb_dis.cpp) | Исходники; самостоятельный адаптер подтвердил обмен на целевой конфигурации, ESPHome runtime не перенесён |
| `CDCOnBoot=default` отключает CDC на старте, `huge_app` даёт APP 3 МБ без OTA | [boards.txt](https://github.com/espressif/arduino-esp32/blob/3.3.8/boards.txt), [huge_app.csv](https://github.com/espressif/arduino-esp32/blob/3.3.8/tools/partitions/huge_app.csv) | Исходники и проверка настроек; исходную разметку каждого стика всё равно сверяют перед записью |
| `HardwareSerial::flush()` не задаёт явный предел ожидания TX | [HardwareSerial.cpp](https://github.com/espressif/arduino-esp32/blob/3.3.8/cores/esp32/HardwareSerial.cpp), [esp32-hal-uart.c](https://github.com/espressif/arduino-esp32/blob/3.3.8/cores/esp32/esp32-hal-uart.c) | Исходники; проект использует `uart_wait_tx_done` с 100 мс |
| Task watchdog может завершить работу с отдельным reset reason | [ESP-IDF v5.5.4 task_wdt.c](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_system/task_wdt/task_wdt.c), [ESP32-S3 reset_reason.c](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_system/port/soc/esp32s3/reset_reason.c) | Исходники; бюджет восстановления проверен платформенными тестами, аппаратное зависание NOT RUN |

## Протокол Tion 4S

Все ссылки этого раздела относятся к commit `5d1c5b4b148b1f985874b105793ba4520dc1b429`.

| Вывод | Источник | Статус и ограничение |
| --- | --- | --- |
| Heartbeat `0x3932`, запрос состояния `0x3232`; комментарий задаёт 3 с, реализация upstream — 5 с, README предупреждает о питании порта через 8–10 с без обмена | [Внутренний формат](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s-internal.h), [UART vport](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion_4s_uart/tion_4s_uart_vport.cpp), [README](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/README.md) | Источники расходятся по периоду; 3 с — выбор проекта, критерий паузы менее 5 с не равен измеренному пределу отключения |
| `DEV_INFO` сообщает тип 4S `0x8003`, режим и версии; состояние содержит питание, скорость, температуры, нагрев, заслонку и ошибки | [tion-api-internal.h](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-internal.h), [формат 4S](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s-internal.h) | Исходники; тип, режим и `02D0` подтверждены UART на целевых экземплярах |
| Upstream требует UART-прошивку от `02D2`, отдельно исключает `03CD` | [README, раздел UART](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/README.md) | Документация; проверенные команды работают на `02D0`, поэтому он допущен отдельно. Другие версии аппаратно не подтверждены |
| Чтение использует общий request ID 1, запись — другой ID; чтение не несёт уникального ID запроса | [tion-api-4s.cpp](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s.cpp), `read_frame`, `request_state_`, `write_state` | Исходники; на `02D0` чтения наблюдались с другими ID. Порядок и число ответов остаются ограничением, описанным в архитектуре |
| Запись передаёт полное состояние, звук/LED используют `STATE_SAV`, основные команды — `STATE_SET` | [Формат 4S](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s-internal.h), [реализация](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s.cpp) | Исходники, host и аппаратные команды; атомарность относительно пульта отсутствует, сохранение звука/LED после power cycle NOT RUN |
| Raw `heater_mode` и `heater_state` различаются; нормализованное upstream `heater_state` выводится из mode | [tion-api-4s.cpp](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s.cpp), `update_state_` | Исходники; raw-бит наблюдался установленным при выключенном бризере. Проект использует `heater_percent` для текущего нагрева |
| Рабочая уставка по умолчанию 1–25 °C; upstream различает наличие ТЭНа и запрещает нагрев в рециркуляции | [Определения](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-defines.h), [tion-api.cpp](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api.cpp) | Исходники; код проекта проверяет ограничения, длительная терморегуляция NOT RUN |
| `current_temperature` не доказывает температуру комнаты | [CONFIGURATION.md](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/CONFIGURATION.md) | Документация и сравнение с Tion Remote; точное место датчика не подтверждено независимым измерением |
| Native Turbo помечен как BLE-only; `filter_time` — остаток ресурса, сброс передаёт специальный флаг | [Формат 4S](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s-internal.h), [реализация](https://github.com/dentra/esphome-tion/blob/5d1c5b4b148b1f985874b105793ba4520dc1b429/components/tion-api/tion-api-4s.cpp) | Исходники; ускорение проекта — локальный таймер. Настоящий сброс ресурса аппаратно NOT RUN |

## Проверки на границе платформы

Host-модели не заменяют целевые определения ESP-IDF. В частности, `portMUX_INITIALIZER_UNLOCKED` задаёт ненулевой свободный owner через `SPINLOCK_INITIALIZER`, тогда как тестовый `std::mutex` выглядит для анализатора как обычная инициализация. В [spinlock.h](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_hw_support/include/spinlock.h) и [portmacro.h](https://github.com/espressif/esp-idf/blob/v5.5.4/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h) закреплена причина сохранения этой инициализации.

Тестовый `esp_reset_reason_t` сохраняет обычное представление enum по [esp_system.h](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_system/include/esp_system.h). Исключения `clang-tidy` ограничены соответствующими объявлениями и пояснены в коде; глобальные проверки включены.

При изменении upstream повторно проверяйте затронутый вывод, обновляйте версию источника и тесты. Не переносите аппаратный PASS на новый бинарник только по совпадению номера версии.
