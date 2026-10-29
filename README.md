# Ledger C++

[![Ledger contracts](https://github.com/ASimonyan1/ledger-cpp/actions/workflows/cpp.yml/badge.svg)](https://github.com/ASimonyan1/ledger-cpp/actions)

**Перевод либо сохраняется целиком, либо не меняет баланс.**

Учебный backend на C++20 и PostgreSQL: счета, переводы, журнал проводок и повторные запросы. HTTP реализован через cpp-httplib, JSON — nlohmann/json, доступ к БД — libpq с RAII и параметризованными запросами. Версии заголовков и SHA-256 закреплены в CMake.

## Что можно проверить

| Сценарий | Поведение |
|---|---|
| Клиент повторяет перевод после сетевого сбоя | Тот же `Idempotency-Key` и payload возвращают прежнюю операцию без второго списания |
| Тот же ключ, другая сумма | 409 `idempotency_conflict` |
| Несколько клиентов тратят один баланс | Строки блокируются, отрицательный баланс запрещён |
| Переводы в противоположных направлениях | Блокировки счетов берутся в одном порядке |
| Ошибка БД после списания | Откатываются оба баланса, перевод и проводки |
| Ответ COMMIT потерян | 503 предлагает повторить запрос с прежним ключом |

Деньги представлены целыми минимальными единицами одной условной валюты. Дробные числа и `bool` не принимаются. Сумма перевода — до 10^12, баланс — до 9×10^15. Проверки приложения дополнены ограничениями БД.

## Запуск за несколько команд

Нужны Docker и Docker Compose:

```sh
cp .env.example .env
docker compose up --build -d
curl --fail --retry 40 --retry-all-errors --retry-delay 2 http://localhost:8080/health
```

В PowerShell: `Copy-Item .env.example .env`; используйте `curl.exe`. Порт опубликован только на localhost. Пример использует демонстрационный токен из `.env.example`:

```sh
curl -H 'Authorization: Bearer local-ledger-token-change-me' -H 'Content-Type: application/json' \
  -d '{"id":"alice","opening_balance":10000}' http://localhost:8080/v1/accounts
curl -H 'Authorization: Bearer local-ledger-token-change-me' -H 'Content-Type: application/json' \
  -d '{"id":"bob","opening_balance":0}' http://localhost:8080/v1/accounts
curl -H 'Authorization: Bearer local-ledger-token-change-me' -H 'Idempotency-Key: demo-transfer-1' \
  -H 'Content-Type: application/json' -d '{"from":"alice","to":"bob","amount":2500}' \
  http://localhost:8080/v1/transfers
```

Первый перевод: 201, повтор: 200 с `replayed: true`. Балансы — 7500 и 2500. Перезапуск сохраняет данные в томе PostgreSQL. Схема применяется при первом создании пустого тома; обычный рестарт её не пересоздаёт.

## API

Все `/v1/*` требуют `Authorization: Bearer <API_TOKEN>`. `/health` проверяет доступ к таблице счетов.

| Метод | Путь | Результат |
|---|---|---|
| POST | `/v1/accounts` | Счёт с начальным балансом, 201; существующий ID — 409 |
| GET | `/v1/accounts/{id}` | Текущий баланс |
| POST | `/v1/transfers` | Перевод; обязателен `Idempotency-Key` |
| GET | `/v1/accounts/{id}/entries` | Первые 100 проводок, сортировка по ключу перевода |

Идентификаторы: 1–80 символов `A-Z a-z 0-9 _ -`. JSON содержит ровно поля из примеров. Ошибки имеют вид `{"error":"insufficient_funds"}`. 400 — неверный запрос, 401 — токен, 404 — счёт не найден, 409 — конфликт, 503 — БД/тайм-аут; при неоднозначном результате повторяется **тот же ключ**.

## Проверки и сборка

Linux: C++20, CMake 3.20+, libpq-dev, Python 3, psql. Сервер собирается и на Windows при установленном PostgreSQL SDK; CI проверяет Linux.

```sh
export DATABASE_URL='host=127.0.0.1 dbname=ledger_test user=ledger password=... connect_timeout=3'
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f sql/001_schema.sql
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Используйте отдельную тестовую БД: тесты создают счета и временно добавляют ограничение для проверки отката. Восемь HTTP/БД-сценариев: валидация/авторизация, повторы и проводки, повтор после отказа, предел баланса, сбой после списания, конкурентное исчерпание баланса, один ключ из нескольких потоков, встречные переводы. CI повторяет их с ASan/UBSan и отдельно проверяет Docker Compose.

[Архитектура и ограничения](docs/DESIGN.md) · [Как разбирать код](docs/WALKTHROUGH.md)

## Границы

Это демонстрация транзакционной обработки, не банковская система. Начальный баланс задаётся при создании счёта; реального ввода/вывода денег нет. Один сервисный токен, без пользователей, прав на отдельные счета, TLS, конвертации валют и внешних платёжных интеграций. Соединение с БД создаётся на запрос: просто и изолированно, но без пула. Число HTTP-обработчиков и ожидающих соединений ограничено. Записи истории не очищаются; идемпотентность хранится бессрочно. Не заявляется измеренная производительность или готовность к production.
