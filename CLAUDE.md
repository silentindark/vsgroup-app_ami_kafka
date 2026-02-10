# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Asterisk module (`app_ami_kafka`) that captures all AMI (Asterisk Manager Interface) events via a manager hook and publishes them to Apache Kafka using `res_kafka`. Supports JSON or raw AMI output formats, event filtering, and enriches messages with system identification fields and Kafka headers.

## Build Commands

```bash
make                # Build app_ami_kafka.so (and test module)
make test           # Build test_app_ami_kafka.so only
make install        # Install module + XML docs to Asterisk directories
make install-test   # Install test module
make samples        # Install ami_kafka.conf.sample to /etc/asterisk/
make clean          # Remove .o and .so files
```

**Build requires Asterisk headers** installed at system paths (e.g. `/usr/include/asterisk/`). Local builds on dev machines without Asterisk will fail with `asterisk.h file not found` — this is expected. The module also needs `res_kafka` headers from the sibling directory `../vsgroup-res_kafka/`.

## Running Tests

Tests use Asterisk's TEST_FRAMEWORK and run inside a live Asterisk instance:

```bash
asterisk -rx "test execute category /app/ami_kafka/"
```

There are 21 tests covering JSON parsing, filter parsing, filter logic, and advanced filter matching. The test module (`test_app_ami_kafka.so`) must be loaded, and it depends on both `app_ami_kafka` and `res_kafka`.

## Architecture

### Data Flow (Hot Path)

```
AMI Event → ami_hook_callback() → should_send_event() → format (JSON/AMI) → ast_kafka_produce_hdrs()
```

`ami_hook_callback()` is called **synchronously under a read-lock** in Asterisk's manager.c for every AMI event. It must be fast:
- Filtering happens first (early reject before formatting)
- `ast_kafka_produce_hdrs()` only copies to librdkafka's internal buffer (non-blocking)
- Hostname and producer are cached globally

### Key Functions

| Function | Purpose |
|---|---|
| `ami_hook_callback()` | Entry point for every AMI event (hot path) |
| `should_send_event()` | Evaluate include/exclude filter lists |
| `match_eventdata()` | Match single filter entry against event data |
| `add_filter()` | Parse legacy or advanced filter syntax into filter entry |
| `ami_body_to_json()` | Parse `"Key: Value\r\n"` AMI body into `ast_json` object |

### Filter System

Two `ao2_container` lists (includefilters, excludefilters), each holding `event_filter_entry` objects. Logic:
- No filters → send all events
- Include only → send only matching
- Exclude only → send all except matching
- Both → must match include AND not match exclude

Supports legacy regex syntax (`eventfilter = Event: Newchannel`) and advanced syntax with named parameters (`eventfilter(action(include),header(Channel),method(starts_with)) = PJSIP/`).

### Configuration

Uses Asterisk ACO (Asterisk Configuration Objects). Config file: `ami_kafka.conf` with `[general]` and `[kafka]` sections. Custom ACO handlers for `format` and `eventfilter` options. The `eventfilter` option is registered as `ACO_REGEX` with pattern `"^eventfilter"`.

### System Identification

Events are enriched with EntityID (`ast_eid_default`, MAC format), SystemName (`ast_config_AST_SYSTEM_NAME`), hostname, and Asterisk version. These appear both in the message payload and as 8 Kafka message headers.

## Asterisk API Gotchas

- `ast_config_AST_SYSTEM_NAME` is in `asterisk/paths.h` (not `asterisk/options.h`)
- `ast_get_version()` is in `asterisk/ast_version.h` (not `asterisk/version.h` which `#error`s)
- `ast_eid_to_str()` needs a 20-char buffer for the MAC address format
- XML doc `configOption name=` must exactly match the ACO registered name (e.g. `"^eventfilter"` not `"eventfilter"`)

## File Layout

- `app_ami_kafka.c` — Main module (config, filtering, formatting, publishing)
- `test_app_ami_kafka.c` — 21 unit tests using AST_TEST_DEFINE
- `asterisk/kafka.h` — res_kafka public API header (from sibling project)
- `documentation/app_ami_kafka_config-en_US.xml` — Asterisk XML config docs
- `ami_kafka.conf.sample` — Sample configuration file
