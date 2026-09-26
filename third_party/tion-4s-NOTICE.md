# Происхождение UART-протокола Tion 4S

Файлы `libraries/Tion4SCore/src/tion4s/` — узкая самостоятельная реализация формата кадров, полей состояния и пакетов управления питанием, скоростью, заслонкой, звуком, LED, нагревом и фильтром, изученных в `dentra/esphome-tion` на commit `5d1c5b4b148b1f985874b105793ba4520dc1b429`. Проверочные пакеты в `tests/protocol_test.cpp` взяты из `tests/test_hw.h` того же commit; пример кодирования power-off с нулевым служебным полем — синтетический, не захват с устройства владельца. [Закреплённый upstream](https://github.com/dentra/esphome-tion/tree/5d1c5b4b148b1f985874b105793ba4520dc1b429). Его исходные файлы в этот проект не скопированы, но происхождение формата и тестовых данных сохранено.

Лицензия upstream — MIT. Это уведомление не задаёт лицензию текущему проекту.

```text
MIT License

Copyright (c) 2022-2023 Dennis Trachuk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
