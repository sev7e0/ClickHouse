# Форматы входных и выходных данных {#formats}

ClickHouse может принимать (`INSERT`) и отдавать (`SELECT`) данные в различных форматах.

Поддерживаемые форматы и возможность использовать их в запросах `INSERT` и `SELECT` перечислены в таблице ниже.

| Формат | INSERT | SELECT |
| ------- | -------- | -------- |
| [TabSeparated](#tabseparated) | ✔ | ✔ |
| [TabSeparatedRaw](#tabseparatedraw) | ✗ | ✔ |
| [TabSeparatedWithNames](#tabseparatedwithnames) | ✔ | ✔ |
| [TabSeparatedWithNamesAndTypes](#tabseparatedwithnamesandtypes) | ✔ | ✔ |
| [CSV](#csv) | ✔ | ✔ |
| [CSVWithNames](#csvwithnames) | ✔ | ✔ |
| [Values](#data-format-values) | ✔ | ✔ |
| [Vertical](#vertical) | ✗ | ✔ |
| [JSON](#json) | ✗ | ✔ |
| [JSONCompact](#jsoncompact) | ✗ | ✔ |
| [JSONEachRow](#jsoneachrow) | ✔ | ✔ |
| [TSKV](#tskv) | ✔ | ✔ |
| [Pretty](#pretty) | ✗ | ✔ |
| [PrettyCompact](#prettycompact) | ✗ | ✔ |
| [PrettyCompactMonoBlock](#prettycompactmonoblock) | ✗ | ✔ |
| [PrettyNoEscapes](#prettynoescapes) | ✗ | ✔ |
| [PrettySpace](#prettyspace) | ✗ | ✔ |
| [Protobuf](#protobuf) | ✔ | ✔ |
| [Parquet](#data-format-parquet) | ✔ | ✔ |
| [RowBinary](#rowbinary) | ✔ | ✔ |
| [Native](#native) | ✔ | ✔ |
| [Null](#null) | ✗ | ✔ |
| [XML](#xml) | ✗ | ✔ |
| [CapnProto](#capnproto) | ✔ | ✗ |

Вы можете регулировать некоторые параметры работы с форматами с помощью настроек ClickHouse. За дополнительной информацией обращайтесь к разделу [Настройки](../operations/settings/settings.md).

## TabSeparated {#tabseparated}

В TabSeparated формате данные пишутся по строкам. Каждая строчка содержит значения, разделённые табами. После каждого значения идёт таб, кроме последнего значения в строке, после которого идёт перевод строки. Везде подразумеваются исключительно unix-переводы строк. Последняя строка также обязана содержать перевод строки на конце. Значения пишутся в текстовом виде, без обрамляющих кавычек, с экранированием служебных символов.

Этот формат также доступен под именем `TSV`.

Формат `TabSeparated` удобен для обработки данных произвольными программами и скриптами. Он используется по умолчанию в HTTP-интерфейсе, а также в batch-режиме клиента командной строки. Также формат позволяет переносить данные между разными СУБД. Например, вы можете получить дамп из MySQL и загрузить его в ClickHouse, или наоборот.

Формат `TabSeparated` поддерживает вывод тотальных значений (при использовании WITH TOTALS) и экстремальных значений (при настройке extremes выставленной в 1). В этих случаях, после основных данных выводятся тотальные значения, и экстремальные значения. Основной результат, тотальные значения и экстремальные значения, отделяются друг от друга пустой строкой. Пример:

```sql
SELECT EventDate, count() AS c FROM test.hits GROUP BY EventDate WITH TOTALS ORDER BY EventDate FORMAT TabSeparated``
```

```
2014-03-17      1406958
2014-03-18      1383658
2014-03-19      1405797
2014-03-20      1353623
2014-03-21      1245779
2014-03-22      1031592
2014-03-23      1046491

0000-00-00      8873898

2014-03-17      1031592
2014-03-23      1406958
```

### Форматирование данных

Целые числа пишутся в десятичной форме. Числа могут содержать лишний символ "+" в начале (игнорируется при парсинге, а при форматировании не пишется). Неотрицательные числа не могут содержать знак отрицания. При чтении допустим парсинг пустой строки, как числа ноль, или (для знаковых типов) строки, состоящей из одного минуса, как числа ноль. Числа, не помещающиеся в соответствующий тип данных, могут парсится, как некоторое другое число, без сообщения об ошибке.

Числа с плавающей запятой пишутся в десятичной форме. При этом, десятичный разделитель - точка. Поддерживается экспоненциальная запись, а также inf, +inf, -inf, nan. Запись числа с плавающей запятой может начинаться или заканчиваться на десятичную точку.
При форматировании возможна потеря точности чисел с плавающей запятой.
При парсинге, допустимо чтение не обязательно наиболее близкого к десятичной записи машинно-представимого числа.

Даты выводятся в формате YYYY-MM-DD, парсятся в том же формате, но с любыми символами в качестве разделителей.
Даты-с-временем выводятся в формате YYYY-MM-DD hh:mm:ss, парсятся в том же формате, но с любыми символами в качестве разделителей.
Всё это происходит в системном часовом поясе на момент старта клиента (если клиент занимается форматированием данных) или сервера. Для дат-с-временем не указывается, действует ли daylight saving time. То есть, если в дампе есть времена во время перевода стрелок назад, то дамп не соответствует данным однозначно, и при парсинге будет выбрано какое-либо из двух времён.
При парсинге, некорректные даты и даты-с-временем могут парситься с естественным переполнением или как нулевые даты/даты-с-временем без сообщения об ошибке.

В качестве исключения, поддерживается также парсинг даты-с-временем в формате unix timestamp, если он состоит ровно из 10 десятичных цифр. Результат не зависит от часового пояса. Различение форматов YYYY-MM-DD hh:mm:ss и NNNNNNNNNN делается автоматически.

Строки выводятся с экранированием спецсимволов с помощью обратного слеша. При выводе, используются следующие escape-последовательности: `\b`, `\f`, `\r`, `\n`, `\t`, `\0`, `\'`, `\\`. Парсер также поддерживает последовательности `\a`, `\v`, и `\xHH` (последовательности hex escape) и любые последовательности вида `\c`, где `c` — любой символ (такие последовательности преобразуются в `c`). Таким образом, при чтении поддерживаются форматы, где перевод строки может быть записан как `\n` и как `\` и перевод строки. Например, строка `Hello world`, где между словами вместо пробела стоит перевод строки, может быть считана в любом из следующих вариантов:

```
Hello\nworld

Hello\
world
```

Второй вариант поддерживается, так как его использует MySQL при записи tab-separated дампа.

Минимальный набор символов, которых вам необходимо экранировать при передаче в TabSeparated формате: таб, перевод строки (LF) и обратный слеш.

Экранируется лишь небольшой набор символов. Вы можете легко наткнуться на строковое значение, которое испортит ваш терминал при выводе в него.

Массивы форматируются в виде списка значений через запятую в квадратных скобках. Элементы массива - числа форматируются как обычно, а даты, даты-с-временем и строки - в одинарных кавычках с такими же правилами экранирования, как указано выше.

[NULL](../query_language/syntax.md) форматируется как `\N`.

## TabSeparatedRaw {#tabseparatedraw}

Отличается от формата `TabSeparated` тем, что строки выводятся без экранирования.
Этот формат подходит только для вывода результата выполнения запроса, но не для парсинга (приёма данных для вставки в таблицу).

Этот формат также доступен под именем `TSVRaw`.

## TabSeparatedWithNames {#tabseparatedwithnames}

Отличается от формата `TabSeparated` тем, что в первой строке пишутся имена столбцов.
При парсинге, первая строка полностью игнорируется. Вы не можете использовать имена столбцов, чтобы указать их порядок расположения, или чтобы проверить их корректность.
(Поддержка обработки заголовка при парсинге может быть добавлена в будущем.)

Этот формат также доступен под именем `TSVWithNames`.

## TabSeparatedWithNamesAndTypes {#tabseparatedwithnamesandtypes}

Отличается от формата `TabSeparated` тем, что в первой строке пишутся имена столбцов, а во второй - типы столбцов.
При парсинге, первая и вторая строка полностью игнорируется.

Этот формат также доступен под именем `TSVWithNamesAndTypes`.

## TSKV {#tskv}

Похож на TabSeparated, но выводит значения в формате name=value. Имена экранируются так же, как строки в формате TabSeparated и, дополнительно, экранируется также символ =.

```
SearchPhrase=   count()=8267016
SearchPhrase=интерьер ванной комнаты    count()=2166
SearchPhrase=яндекс     count()=1655
SearchPhrase=весна 2014 мода    count()=1549
SearchPhrase=фриформ фото       count()=1480
SearchPhrase=анджелина джоли    count()=1245
SearchPhrase=омск       count()=1112
SearchPhrase=фото собак разных пород    count()=1091
SearchPhrase=дизайн штор        count()=1064
SearchPhrase=баку       count()=1000
```

[NULL](../query_language/syntax.md) форматируется как `\N`.

```sql
SELECT * FROM t_null FORMAT TSKV
```

```
x=1	y=\N
```

При большом количестве маленьких столбцов, этот формат существенно неэффективен, и обычно нет причин его использовать. Впрочем, он не хуже формата JSONEachRow по производительности.

Поддерживается как вывод, так и парсинг данных в этом формате. При парсинге, поддерживается расположение значений разных столбцов в произвольном порядке. Допустимо отсутствие некоторых значений - тогда они воспринимаются как равные значениям по умолчанию. В этом случае в качестве значений по умолчанию используются нули и пустые строки. Сложные значения, которые могут быть заданы в таблице не поддерживаются как значения по умолчанию.

При парсинге, в качестве дополнительного поля, может присутствовать `tskv` без знака равенства и без значения. Это поле игнорируется.

## CSV {#csv}

Формат Comma Separated Values ([RFC](https://tools.ietf.org/html/rfc4180)).

При форматировании, строки выводятся в двойных кавычках. Двойная кавычка внутри строки выводится как две двойные кавычки подряд. Других правил экранирования нет. Даты и даты-с-временем выводятся в двойных кавычках. Числа выводятся без кавычек. Значения разделяются символом-разделителем, по умолчанию  — `,`. Символ-разделитель определяется настройкой [format_csv_delimiter](../operations/settings/settings.md#settings-format_csv_delimiter). Строки разделяются unix переводом строки (LF). Массивы сериализуются в CSV следующим образом: сначала массив сериализуется в строку, как в формате TabSeparated, а затем полученная строка выводится в CSV в двойных кавычках. Кортежи в формате CSV сериализуются, как отдельные столбцы (то есть, теряется их вложенность в кортеж).

```
clickhouse-client --format_csv_delimiter="|" --query="INSERT INTO test.csv FORMAT CSV" < data.csv
```

&ast;По умолчанию — `,`. См. настройку [format_csv_delimiter](../operations/settings/settings.md#settings-format_csv_delimiter) для дополнительной информации.

При парсинге, все значения могут парситься как в кавычках, так и без кавычек. Поддерживаются как двойные, так и одинарные кавычки. Строки также могут быть без кавычек. В этом случае они парсятся до символа-разделителя или перевода строки (CR или LF). В нарушение RFC, в случае парсинга строк не в кавычках, начальные и конечные пробелы и табы игнорируются. В качестве перевода строки, поддерживаются как Unix (LF), так и Windows (CR LF) и Mac OS Classic (LF CR) варианты.

`NULL` форматируется в виде `\N` или `NULL` или пустой неэкранированной строки (см. настройки [input_format_csv_unquoted_null_literal_as_null](../operations/settings/settings.md#settings-input_format_csv_unquoted_null_literal_as_null) и [input_format_defaults_for_omitted_fields](../operations/settings/settings.md#session_settings-input_format_defaults_for_omitted_fields)).

Если установлена настройка [input_format_defaults_for_omitted_fields = 1](../operations/settings/settings.md#session_settings-input_format_defaults_for_omitted_fields) и тип столбца не `Nullable(T)`, то пустые значения без кавычек заменяются значениями по умолчанию для типа данных столбца.

Формат CSV поддерживает вывод totals и extremes аналогично `TabSeparated`.

## CSVWithNames

Выводит также заголовок, аналогично `TabSeparatedWithNames`.

## JSON {#json}

Выводит данные в формате JSON. Кроме таблицы с данными, также выводятся имена и типы столбцов, и некоторая дополнительная информация - общее количество выведенных строк, а также количество строк, которое могло бы быть выведено, если бы не было LIMIT-а. Пример:

```sql
SELECT SearchPhrase, count() AS c FROM test.hits GROUP BY SearchPhrase WITH TOTALS ORDER BY c DESC LIMIT 5 FORMAT JSON
```

```json
{
        "meta":
        [
                {
                        "name": "SearchPhrase",
                        "type": "String"
                },
                {
                        "name": "c",
                        "type": "UInt64"
                }
        ],

        "data":
        [
                {
                        "SearchPhrase": "",
                        "c": "8267016"
                },
                {
                        "SearchPhrase": "bathroom interior design",
                        "c": "2166"
                },
                {
                        "SearchPhrase": "yandex",
                        "c": "1655"
                },
                {
                        "SearchPhrase": "spring 2014 fashion",
                        "c": "1549"
                },
                {
                        "SearchPhrase": "freeform photos",
                        "c": "1480"
                }
        ],

        "totals":
        {
                "SearchPhrase": "",
                "c": "8873898"
        },

        "extremes":
        {
                "min":
                {
                        "SearchPhrase": "",
                        "c": "1480"
                },
                "max":
                {
                        "SearchPhrase": "",
                        "c": "8267016"
                }
        },

        "rows": 5,

        "rows_before_limit_at_least": 141137
}
```

JSON совместим с JavaScript. Для этого, дополнительно экранируются некоторые символы: символ прямого слеша `/` экранируется в виде `\/`; альтернативные переводы строк `U+2028`, `U+2029`, на которых ломаются некоторые браузеры, экранируются в виде `\uXXXX`-последовательностей. Экранируются ASCII control characters: backspace, form feed, line feed, carriage return, horizontal tab в виде `\b`, `\f`, `\n`, `\r`, `\t` соответственно, а также остальные байты из диапазона 00-1F с помощью `\uXXXX`-последовательностей. Невалидные UTF-8 последовательности заменяются на replacement character � и, таким образом, выводимый текст будет состоять из валидных UTF-8 последовательностей. Числа типа UInt64 и Int64, для совместимости с JavaScript, по умолчанию выводятся в двойных кавычках. Чтобы они выводились без кавычек, можно установить конфигурационный параметр [output_format_json_quote_64bit_integers](../operations/settings/settings.md#session_settings-output_format_json_quote_64bit_integers) равным 0.

`rows` - общее количество выведенных строчек.

`rows_before_limit_at_least` - не менее скольких строчек получилось бы, если бы не было LIMIT-а. Выводится только если запрос содержит LIMIT.
В случае, если запрос содержит GROUP BY, rows_before_limit_at_least - точное число строк, которое получилось бы, если бы не было LIMIT-а.

`totals` - тотальные значения (при использовании WITH TOTALS).

`extremes` - экстремальные значения (при настройке extremes, выставленной в 1).

Этот формат подходит только для вывода результата выполнения запроса, но не для парсинга (приёма данных для вставки в таблицу).

ClickHouse поддерживает [NULL](../query_language/syntax.md), который при выводе JSON будет отображен как `null`.

Смотрите также формат [JSONEachRow](#jsoneachrow) .

## JSONCompact {#jsoncompact}

Отличается от JSON только тем, что строчки данных выводятся в массивах, а не в object-ах.

Пример:

```json
{
        "meta":
        [
                {
                        "name": "SearchPhrase",
                        "type": "String"
                },
                {
                        "name": "c",
                        "type": "UInt64"
                }
        ],

        "data":
        [
                ["", "8267016"],
                ["интерьер ванной комнаты", "2166"],
                ["яндекс", "1655"],
                ["весна 2014 мода", "1549"],
                ["фриформ фото", "1480"]
        ],

        "totals": ["","8873898"],

        "extremes":
        {
                "min": ["","1480"],
                "max": ["","8267016"]
        },

        "rows": 5,

        "rows_before_limit_at_least": 141137
}
```

Этот формат подходит только для вывода результата выполнения запроса, но не для парсинга (приёма данных для вставки в таблицу).
Смотрите также формат `JSONEachRow`.

## JSONEachRow {#jsoneachrow}

При использовании этого формата, ClickHouse выводит каждую запись как объект JSON (каждый объект отдельной строкой), при этом данные в целом — невалидный JSON.

```json
{"SearchPhrase":"дизайн штор","count()":"1064"}
{"SearchPhrase":"баку","count()":"1000"}
{"SearchPhrase":"","count":"8267016"}
```

При вставке данных необходимо каждую запись передавать как отдельный объект JSON.

### Вставка данных

```
INSERT INTO UserActivity FORMAT JSONEachRow {"PageViews":5, "UserID":"4324182021466249494", "Duration":146,"Sign":-1} {"UserID":"4324182021466249494","PageViews":6,"Duration":185,"Sign":1}
```

ClickHouse допускает:

- Любой порядок пар ключ-значение в объекте.
- Пропуск отдельных значений.

ClickHouse игнорирует пробелы между элементами и запятые после объектов. Вы можете передать все объекты одной строкой. Вам не нужно разделять их переносами строк.

**Обработка пропущенных значений**

ClickHouse заменяет опущенные значения значениями по умолчанию для соответствующих [data types](../data_types/index.md).

Если указано `DEFAULT expr`, то ClickHouse использует различные правила подстановки в зависимости от настройки [input_format_defaults_for_omitted_fields](../operations/settings/settings.md#session_settings-input_format_defaults_for_omitted_fields).

Рассмотрим следующую таблицу:

```
CREATE TABLE IF NOT EXISTS example_table
(
    x UInt32,
    a DEFAULT x * 2
) ENGINE = Memory;
```

- Если `input_format_defaults_for_omitted_fields = 0`, то значение по умолчанию для `x` и `a` равняется `0` (поскольку это значение по умолчанию для типа данных `UInt32`.)
- Если `input_format_defaults_for_omitted_fields = 1`, то значение по умолчанию для `x` равно `0`, а значение по умолчанию `a` равно `x * 2`.

!!! note "Предупреждение"
    Если `insert_sample_with_metadata = 1`, то при обработке запросов ClickHouse потребляет больше вычислительных ресурсов, чем если `insert_sample_with_metadata = 0`.

### Выборка данных

Рассмотрим в качестве примера таблицу `UserActivity`:

```
┌──────────────UserID─┬─PageViews─┬─Duration─┬─Sign─┐
│ 4324182021466249494 │         5 │      146 │   -1 │
│ 4324182021466249494 │         6 │      185 │    1 │
└─────────────────────┴───────────┴──────────┴──────┘
```

Запрос `SELECT * FROM UserActivity FORMAT JSONEachRow` возвращает:

```
{"UserID":"4324182021466249494","PageViews":5,"Duration":146,"Sign":-1}
{"UserID":"4324182021466249494","PageViews":6,"Duration":185,"Sign":1}
```

В отличие от формата [JSON](#json), для `JSONEachRow` ClickHouse не заменяет невалидные UTF-8 последовательности. Значения экранируются так же, как и для формата `JSON`.

!!! note "Примечание"
    В строках может выводиться произвольный набор байт. Используйте формат `JSONEachRow`, если вы уверены, что данные в таблице могут быть представлены в формате JSON без потери информации.

### Использование вложенных структур {#jsoneachrow-nested}

Если у вас есть таблица со столбцами типа [Nested](../data_types/nested_data_structures/nested.md), то в неё можно вставить данные из JSON-документа с такой же структурой. Функциональность включается настройкой [input_format_import_nested_json](../operations/settings/settings.md#settings-input_format_import_nested_json).

Например, рассмотрим следующую таблицу:

```sql
CREATE TABLE json_each_row_nested (n Nested (s String, i Int32) ) ENGINE = Memory
```

Из описания типа данных `Nested` видно, что ClickHouse трактует каждый компонент вложенной структуры как отдельный столбец (для нашей таблицы `n.s` и `n.i`). Можно вставить данные следующим образом:

```sql
INSERT INTO json_each_row_nested FORMAT JSONEachRow {"n.s": ["abc", "def"], "n.i": [1, 23]}
```

Чтобы вставить данные как иерархический объект JSON, установите [input_format_import_nested_json=1](../operations/settings/settings.md#settings-input_format_import_nested_json).

```json
{
    "n": {
        "s": ["abc", "def"],
        "i": [1, 23]
    }
}
```

Без этой настройки ClickHouse сгенерирует исключение.

```sql
SELECT name, value FROM system.settings WHERE name = 'input_format_import_nested_json'
```

```text
┌─name────────────────────────────┬─value─┐
│ input_format_import_nested_json │ 0     │
└─────────────────────────────────┴───────┘
```

```sql
INSERT INTO json_each_row_nested FORMAT JSONEachRow {"n": {"s": ["abc", "def"], "i": [1, 23]}}
```

```text
Code: 117. DB::Exception: Unknown field found while parsing JSONEachRow format: n: (at row 1)
```

```sql
SET input_format_import_nested_json=1
INSERT INTO json_each_row_nested FORMAT JSONEachRow {"n": {"s": ["abc", "def"], "i": [1, 23]}}
SELECT * FROM json_each_row_nested
```

```text
┌─n.s───────────┬─n.i────┐
│ ['abc','def'] │ [1,23] │
└───────────────┴────────┘
```

## Native {#native}

Самый эффективный формат. Данные пишутся и читаются блоками в бинарном виде. Для каждого блока пишется количество строк, количество столбцов, имена и типы столбцов, а затем кусочки столбцов этого блока, один за другим. То есть, этот формат является "столбцовым" - не преобразует столбцы в строки. Именно этот формат используется в родном интерфейсе - при межсерверном взаимодействии, при использовании клиента командной строки, при работе клиентов, написанных на C++.

Вы можете использовать этот формат для быстрой генерации дампов, которые могут быть прочитаны только СУБД ClickHouse. Вряд ли имеет смысл работать с этим форматом самостоятельно.

## Null {#null}

Ничего не выводит. При этом, запрос обрабатывается, а при использовании клиента командной строки, данные ещё и передаются на клиент. Используется для тестов, в том числе, тестов производительности.
Очевидно, формат подходит только для вывода, но не для парсинга.

## Pretty {#pretty}

Выводит данные в виде Unicode-art табличек, также используя ANSI-escape последовательности для установки цветов в терминале.
Рисуется полная сетка таблицы и, таким образом, каждая строчка занимает две строки в терминале.
Каждый блок результата выводится в виде отдельной таблицы. Это нужно, чтобы можно было выводить блоки без буферизации результата (буферизация потребовалась бы, чтобы заранее вычислить видимую ширину всех значений.)

[NULL](../query_language/syntax.md) выводится как `ᴺᵁᴸᴸ`.

```sql
SELECT * FROM t_null
```
```
┌─x─┬────y─┐
│ 1 │ ᴺᵁᴸᴸ │
└───┴──────┘
```

В форматах `Pretty*` строки выводятся без экранирования. Ниже приведен пример для формата [PrettyCompact](#prettycompact):

```sql
SELECT 'String with \'quotes\' and \t character' AS Escaping_test
```

```
┌─Escaping_test────────────────────────┐
│ String with 'quotes' and 	 character │
└──────────────────────────────────────┘
```

Для защиты от вываливания слишком большого количества данных в терминал, выводится только первые 10 000 строк. Если строк больше или равно 10 000, то будет написано "Showed first 10 000."
Этот формат подходит только для вывода результата выполнения запроса, но не для парсинга (приёма данных для вставки в таблицу).

Формат `Pretty` поддерживает вывод тотальных значений (при использовании WITH TOTALS) и экстремальных значений (при настройке extremes выставленной в 1). В этих случаях, после основных данных выводятся тотальные значения, и экстремальные значения, в отдельных табличках. Пример (показан для формата [PrettyCompact](#prettycompact)):

```sql
SELECT EventDate, count() AS c FROM test.hits GROUP BY EventDate WITH TOTALS ORDER BY EventDate FORMAT PrettyCompact
```

```
┌──EventDate─┬───────c─┐
│ 2014-03-17 │ 1406958 │
│ 2014-03-18 │ 1383658 │
│ 2014-03-19 │ 1405797 │
│ 2014-03-20 │ 1353623 │
│ 2014-03-21 │ 1245779 │
│ 2014-03-22 │ 1031592 │
│ 2014-03-23 │ 1046491 │
└────────────┴─────────┘

Totals:
┌──EventDate─┬───────c─┐
│ 0000-00-00 │ 8873898 │
└────────────┴─────────┘

Extremes:
┌──EventDate─┬───────c─┐
│ 2014-03-17 │ 1031592 │
│ 2014-03-23 │ 1406958 │
└────────────┴─────────┘
```

## PrettyCompact {#prettycompact}

Отличается от [Pretty](#pretty) тем, что не рисуется сетка между строками - результат более компактный.
Этот формат используется по умолчанию в клиенте командной строки в интерактивном режиме.

## PrettyCompactMonoBlock {#prettycompactmonoblock}

Отличается от [PrettyCompact](#prettycompact) тем, что строки (до 10 000 штук) буферизуются и затем выводятся в виде одной таблицы, а не по блокам.

## PrettyNoEscapes {#prettynoescapes}

Отличается от Pretty тем, что не используются ANSI-escape последовательности. Это нужно для отображения этого формата в браузере, а также при использовании утилиты командной строки watch.

Пример:

```bash
watch -n1 "clickhouse-client --query='SELECT event, value FROM system.events FORMAT PrettyCompactNoEscapes'"
```

Для отображения в браузере, вы можете использовать HTTP интерфейс.

### PrettyCompactNoEscapes

Аналогично.

### PrettySpaceNoEscapes

Аналогично.

## PrettySpace {#prettyspace}

Отличается от [PrettyCompact](#prettycompact) тем, что вместо сетки используется пустое пространство (пробелы).

## RowBinary {#rowbinary}

Форматирует и парсит данные по строкам, в бинарном виде. Строки и значения уложены подряд, без разделителей.
Формат менее эффективен, чем формат Native, так как является строковым.

Числа представлены в little endian формате фиксированной длины. Для примера, UInt64 занимает 8 байт.
DateTime представлены как UInt32, содержащий unix timestamp в качестве значения.
Date представлены как UInt16, содержащий количество дней, прошедших с 1970-01-01 в качестве значения.
String представлены как длина в формате varint (unsigned [LEB128](https://en.wikipedia.org/wiki/LEB128)), а затем байты строки.
FixedString представлены просто как последовательность байт.

Array представлены как длина в формате varint (unsigned [LEB128](https://en.wikipedia.org/wiki/LEB128)), а затем элементы массива, подряд.

Для поддержки [NULL](../query_language/syntax.md#null-literal) перед каждым значением типа [Nullable](../data_types/nullable.md

## Values {#data-format-values}

Выводит каждую строку в скобках. Строки разделены запятыми. После последней строки запятой нет. Значения внутри скобок также разделены запятыми. Числа выводятся в десятичном виде без кавычек. Массивы выводятся в квадратных скобках. Строки, даты, даты-с-временем выводятся в кавычках. Правила экранирования и особенности парсинга аналогичны формату [TabSeparated](#tabseparated). При форматировании, лишние пробелы не ставятся, а при парсинге - допустимы и пропускаются (за исключением пробелов внутри значений типа массив, которые недопустимы). [NULL](../query_language/syntax.md) представляется как `NULL`.

Минимальный набор символов, которых вам необходимо экранировать при передаче в Values формате: одинарная кавычка и обратный слеш.

Именно этот формат используется в запросе `INSERT INTO t VALUES ...`, но вы также можете использовать его для форматирования результатов запросов.

## Vertical {#vertical}

Выводит каждое значение на отдельной строке, с указанием имени столбца. Формат удобно использовать для вывода одной-нескольких строк, если каждая строка состоит из большого количества столбцов.

[NULL](../query_language/syntax.md) выводится как `ᴺᵁᴸᴸ`.

Пример:

```sql
SELECT * FROM t_null FORMAT Vertical
```
```
Row 1:
──────
x: 1
y: ᴺᵁᴸᴸ
```

В формате `Vertical` строки выводятся без экранирования. Например:

```sql
SELECT 'string with \'quotes\' and \t with some special \n characters' AS test FORMAT Vertical
```

```
Row 1:
──────
test: string with 'quotes' and 	 with some special
 characters
```

Этот формат подходит только для вывода результата выполнения запроса, но не для парсинга (приёма данных для вставки в таблицу).

## XML {#xml}

Формат XML подходит только для вывода данных, не для парсинга. Пример:

```xml
<?xml version='1.0' encoding='UTF-8' ?>
<result>
        <meta>
                <columns>
                        <column>
                                <name>SearchPhrase</name>
                                <type>String</type>
                        </column>
                        <column>
                                <name>count()</name>
                                <type>UInt64</type>
                        </column>
                </columns>
        </meta>
        <data>
                <row>
                        <SearchPhrase></SearchPhrase>
                        <field>8267016</field>
                </row>
                <row>
                        <SearchPhrase>интерьер ванной комнаты</SearchPhrase>
                        <field>2166</field>
                </row>
                <row>
                        <SearchPhrase>яндекс</SearchPhrase>
                        <field>1655</field>
                </row>
                <row>
                        <SearchPhrase>весна 2014 мода</SearchPhrase>
                        <field>1549</field>
                </row>
                <row>
                        <SearchPhrase>фриформ фото</SearchPhrase>
                        <field>1480</field>
                </row>
                <row>
                        <SearchPhrase>анджелина джоли</SearchPhrase>
                        <field>1245</field>
                </row>
                <row>
                        <SearchPhrase>омск</SearchPhrase>
                        <field>1112</field>
                </row>
                <row>
                        <SearchPhrase>фото собак разных пород</SearchPhrase>
                        <field>1091</field>
                </row>
                <row>
                        <SearchPhrase>дизайн штор</SearchPhrase>
                        <field>1064</field>
                </row>
                <row>
                        <SearchPhrase>баку</SearchPhrase>
                        <field>1000</field>
                </row>
        </data>
        <rows>10</rows>
        <rows_before_limit_at_least>141137</rows_before_limit_at_least>
</result>
```

Если имя столбца не имеет некоторый допустимый вид, то в качестве имени элемента используется просто field. В остальном, структура XML повторяет структуру в формате JSON.
Как и для формата JSON, невалидные UTF-8 последовательности заменяются на replacement character � и, таким образом, выводимый текст будет состоять из валидных UTF-8 последовательностей.

В строковых значениях, экранируются символы `<` и `&` как `&lt;` и `&amp;`.

Массивы выводятся как `<array><elem>Hello</elem><elem>World</elem>...</array>`,
а кортежи как `<tuple><elem>Hello</elem><elem>World</elem>...</tuple>`.

## CapnProto {#capnproto}

Cap'n Proto - формат бинарных сообщений, похож на Protocol Buffers и Thrift, но не похож на JSON или MessagePack.

Сообщения Cap'n Proto строго типизированы и не самоописывающиеся, т.е. нуждаются во внешнем описании схемы. Схема применяется "на лету" и кешируется между запросами.

```bash
cat capnproto_messages.bin | clickhouse-client --query "INSERT INTO test.hits FORMAT CapnProto SETTINGS format_schema='schema:Message'"
```

Где `schema.capnp` выглядит следующим образом:

```
struct Message {
  SearchPhrase @0 :Text;
  c @1 :Uint64;
}
```

Десериализация эффективна и обычно не повышает нагрузку на систему.

См. также [схема формата](#formatschema).

## Protobuf {#protobuf}

Protobuf - формат [Protocol Buffers](https://developers.google.com/protocol-buffers/).

Формат нуждается во внешнем описании схемы. Схема кэшируется между запросами.
ClickHouse поддерживает как синтаксис `proto2`, так и `proto3`; все типы полей (repeated/optional/required) поддерживаются.

Пример использования формата:

```sql
SELECT * FROM test.table FORMAT Protobuf SETTINGS format_schema = 'schemafile:MessageType'
```

или

```bash
cat protobuf_messages.bin | clickhouse-client --query "INSERT INTO test.table FORMAT Protobuf SETTINGS format_schema='schemafile:MessageType'"
```

Где файл `schemafile.proto` может выглядеть так:

```
syntax = "proto3";

message MessageType {
  string name = 1;
  string surname = 2;
  uint32 birthDate = 3;
  repeated string phoneNumbers = 4;
};
```

Соответствие между столбцами таблицы и полями сообщения `Protocol Buffers` устанавливается по имени,
при этом игнорируется регистр букв и символы `_` (подчеркивание) и `.` (точка) считаются одинаковыми.
Если типы столбцов не соответствуют точно типам полей сообщения `Protocol Buffers`, производится необходимая конвертация.

Вложенные сообщения поддерживаются, например, для поля `z` в таком сообщении

```
message MessageType {
  message XType {
    message YType {
      int32 z;
    };
    repeated YType y;
  };
  XType x;
};
```

ClickHouse попытается найти столбец с именем `x.y.z` (или `x_y_z`, или `X.y_Z` и т.п.).
Вложенные сообщения удобно использовать в качестве соответствия для [вложенной структуры данных](../data_types/nested_data_structures/nested.md).

Значения по умолчанию, определённые в схеме `proto2`, например,

```
syntax = "proto2";

message MessageType {
  optional int32 result_per_page = 3 [default = 10];
}
```

не применяются; вместо них используются определенные в таблице [значения по умолчанию](../query_language/create.md#create-default-values).

ClickHouse пишет и читает сообщения `Protocol Buffers` в формате `length-delimited`. Это означает, что перед каждым сообщением пишется его длина
в формате [varint](https://developers.google.com/protocol-buffers/docs/encoding#varints). См. также [как читать и записывать сообщения Protocol Buffers в формате length-delimited в различных языках программирования](https://cwiki.apache.org/confluence/display/GEODE/Delimiting+Protobuf+Messages).

## Parquet {#data-format-parquet}

[Apache Parquet](http://parquet.apache.org/) — формат поколоночного хранения данных, который распространён в экосистеме Hadoop. Для формата `Parquet` ClickHouse поддерживает операции чтения и записи.

### Соответствие типов данных

Таблица ниже содержит поддерживаемые типы данных и их соответствие [типам данных](../data_types/index.md) ClickHouse для запросов `INSERT` и `SELECT`.

| Тип данных Parquet (`INSERT`) | Тип данных ClickHouse | Тип данных Parquet (`SELECT`) |
| -------------------- | ------------------ | ---- |
| `UINT8`, `BOOL` | [UInt8](../data_types/int_uint.md) | `UINT8` |
| `INT8` | [Int8](../data_types/int_uint.md) | `INT8` |
| `UINT16` | [UInt16](../data_types/int_uint.md) | `UINT16` |
| `INT16` | [Int16](../data_types/int_uint.md) | `INT16` |
| `UINT32` | [UInt32](../data_types/int_uint.md) | `UINT32` |
| `INT32` | [Int32](../data_types/int_uint.md) | `INT32` |
| `UINT64` | [UInt64](../data_types/int_uint.md) | `UINT64` |
| `INT64` | [Int64](../data_types/int_uint.md) | `INT64` |
| `FLOAT`, `HALF_FLOAT` | [Float32](../data_types/float.md) | `FLOAT` |
| `DOUBLE` | [Float64](../data_types/float.md) | `DOUBLE` |
| `DATE32` | [Date](../data_types/date.md) | `UINT16` |
| `DATE64`, `TIMESTAMP` | [DateTime](../data_types/datetime.md) | `UINT32` |
| `STRING`, `BINARY` | [String](../data_types/string.md) | `STRING` |
| — | [FixedString](../data_types/fixedstring.md) | `STRING` |
| `DECIMAL` | [Decimal](../data_types/decimal.md) | `DECIMAL` |

ClickHouse поддерживает настраиваемую точность для формата `Decimal`. При обработке запроса `INSERT`, ClickHouse обрабатывает тип данных Parquet `DECIMAL` как `Decimal128`.

Неподдержанные типы данных Parquet: `DATE32`, `TIME32`, `FIXED_SIZE_BINARY`, `JSON`, `UUID`, `ENUM`.

Типы данных столбцов в ClickHouse могут отличаться от типов данных соответствущих полей файла в формате Parquet. При вставке данных, ClickHouse интерпретирует типы данных в соответствии с таблицей выше, а затем [приводит](../query_language/functions/type_conversion_functions/#type_conversion_function-cast) данные к тому типу, который установлен для столбца таблицы.

### Inserting and Selecting Data

Чтобы вставить в ClickHouse данные из файла в формате Parquet, выполните команду следующего вида:

```
cat {filename} | clickhouse-client --query="INSERT INTO {some_table} FORMAT Parquet"
```

Чтобы получить данные из таблицы ClickHouse и сохранить их в файл формата Parquet, используйте команду следующего вида:

```
clickhouse-client --query="SELECT * FROM {some_table} FORMAT Parquet" > {some_file.pq}
```

Для обмена данными с экосистемой Hadoop можно использовать движки таблиц `HDFS` и `URL`.

## Схема формата {#formatschema}

Имя файла со схемой записывается в настройке `format_schema`. При использовании форматов `Cap'n Proto` и `Protobuf` требуется указать схему.
Схема представляет собой имя файла и имя типа в этом файле, разделенные двоеточием, например `schemafile.proto:MessageType`.
Если файл имеет стандартное расширение для данного формата (например `.proto` для `Protobuf`),
то можно его не указывать и записывать схему так `schemafile:MessageType`.

Если для ввода/вывода данных используется [клиент](../interfaces/cli.md) в [интерактивном режиме](../interfaces/cli.md#cli_usage), то при записи схемы можно использовать абсолютный путь или записывать путь
относительно текущей директории на клиенте. Если клиент используется в [batch режиме](../interfaces/cli.md#cli_usage), то в записи схемы допускается только относительный путь, из соображений безопасности.

Если для ввода/вывода данных используется [HTTP-интерфейс](../interfaces/http.md), то файл со схемой должен располагаться на сервере в каталоге,
указанном в параметре [format_schema_path](../operations/server_settings/settings.md#server_settings-format_schema_path) конфигурации сервера.

[Оригинальная статья](https://clickhouse.yandex/docs/ru/interfaces/formats/) <!--hide-->
