# Первая публикация на GitHub

Адрес проекта: [viventu/tion-homekit](https://github.com/viventu/tion-homekit). Собственный код — [0BSD](../LICENSE). Владелец выбрал **чистый снимок GM**: приватная история разработки остаётся в этом локальном репозитории и никогда не добавляется как remote публичного проекта. 2026-09-26 владелец разрешил публикацию исходников; создан пустой приватный репозиторий для первоначальной проверки Linux/GCC и сборки. Публичный снимок обозначается кандидатом GM1 с продолжающимся суточным прогоном. Это не выпуск испытанного GM или бинарников. Ниже зафиксирован порядок загрузки и открытия доступа.

## До создания публичного репозитория

1. Для выпуска GM закройте [аппаратные и релизные условия](releasing.md). Исходники кандидата разрешено опубликовать с явно указанными незавершёнными испытаниями по решению владельца 2026-09-26. Не переносите PASS от `rc2` на другой образ. Запишите версию, Git commit и tree ID проверенного исходника, FQBN, версии инструментария, SHA-256 **именно испытанного** APP и результаты двух устройств. Сравните тестируемый бинарник с тем, который собираетесь отдать пользователям.
2. Проверьте чистый Git checkout и выполните `scripts/test-host.sh`, `scripts/check.sh`, `git diff --check` и `python3 scripts/prepare-public-snapshot.py`. Проверка снимка ищет опасные имена файлов и типовые идентификаторы/секреты только в текущем дереве; она не заменяет ручной просмотр и специализированный secret scanner.
3. Просмотрите все файлы снимка: лицензию и сторонние notices, инструкции, номера версий, ссылки, журналы испытаний и новые строки, похожие на частные данные. Не переносите `.git`, `.private`, `.build`, `.arduino`, NVS, Flash dump, сырую диагностику и домашние адреса. Сверьте `git ls-tree -r --name-only HEAD` с ожидаемым составом.

## Чистый снимок без прежней истории

Выполняйте из исходного приватного checkout **после** финального GM-коммита. Путь результата должен быть новым и вне исходного репозитория:

```sh
python3 scripts/prepare-public-snapshot.py --output /private/tmp/tion-homekit-public-GM
cd /private/tmp/tion-homekit-public-GM
git init -b main
git config user.name "YOUR_GITHUB_NAME"
git config user.email "YOUR_GITHUB_NOREPLY_EMAIL"
git add -A
git commit -m "Initial public release"
git status --short
git rev-list --all --count
python3 scripts/prepare-public-snapshot.py
```

`git rev-list --all --count` должен вернуть `1`, `git status --short` — пустой вывод. Укажите в новом каталоге свой GitHub `noreply` email из настроек аккаунта, если не хотите публиковать личный адрес в **новых** коммитах. Старый приватный репозиторий не изменяется. Сравните `git rev-parse HEAD^{tree}` в обоих каталогах: значения должны совпасть до любых правок снимка. Новые изменения после создания снимка требуют повторной подготовки из нового GM-коммита; не переносите их через старую историю.

## Проверка на GitHub и открытие доступа

После отдельного решения владельца создайте **пустой приватный** `OWNER/tion-homekit` из каталога снимка, без второго README, license template и `.gitignore`. GitHub CLI поддерживает `gh repo create OWNER/tion-homekit --private --source=. --remote=origin --push`; команда создаёт внешний репозиторий и отправляет единственный публичный коммит, поэтому не выполняется в ходе локальной подготовки.

В приватном репозитории проверьте дерево, лицензию, Markdown и первый запуск workflow `Host and compile checks`. Перед сменой видимости просмотрите логи Actions: после открытия репозитория они тоже станут видны. Убедитесь, что опубликован только новый `main`, нет старых refs, тегов и артефактов, а собранный код соответствует испытанному GM. Заполните краткое описание и темы репозитория. После первого успешного workflow установите правило для `main`: PR и успешные статусы `host (g++)`, `host (clang++)` и `firmware`, запрет force push; учитывайте доступность правил на тарифе аккаунта.

Только затем меняйте видимость на public. Сразу включите **Private Vulnerability Reporting** в настройках Security и проверьте внешний путь `Security → Advisories → Report a vulnerability`; обновите [SECURITY.md](../SECURITY.md), если интерфейс отличается. Проверьте GitHub Community Standards, работу шаблонов issue/PR, Dependabot и secret scanning. Не размещайте реальные секреты даже в приватных GitHub Issues или Actions logs.

## GM release

Для выпуска используйте [шаблон заметок](release-notes-template.md) и tag конкретного коммита публичного снимка. Подготовьте GitHub release как draft; опубликуйте его после проверки текста, тегов, хэшей и состава assets. Если включена неизменяемость releases, все assets прикладывают до публикации draft. GitHub автоматически предлагает архивы исходников по tag.

Бинарные `.bin` допускаются только после полного реестра реально связанных компонентов, notices и проверки лицензий/условий распространения. В assets не включать `.merged.bin`, полный Flash, NVS и пользовательские настройки. Если этот gate не закрыт, первая публикация остаётся **source-only** с инструкцией локальной сборки; наличие GM на двух устройствах само по себе не разрешает раздачу бинарника.

Источники для операций GitHub (проверены 2026-09-24): [создание репозитория через CLI](https://cli.github.com/manual/gh_repo_create), [приватный email коммитов](https://docs.github.com/en/account-and-profile/how-tos/email-preferences/setting-your-commit-email-address), [смена видимости и открытие Actions logs](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/managing-repository-settings/setting-repository-visibility), [правила ветки](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches), [приватное сообщение об уязвимости](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/configure-vulnerability-reporting/configure-for-a-repository), [draft и immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases).
