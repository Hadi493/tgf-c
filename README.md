<div align="center">
  <img src="tgf.png" width="166" height="144" alt="TGF" />
  <h1>TGF - Telegram Feed</h1>

  <p><strong>Open-source Telegram content forwarding bot written in C</strong></p>

  <p>
    <img src="https://img.shields.io/badge/version-1.3.1-blue" alt="version" />
    <img src="https://img.shields.io/badge/GPL-2.0%20license-blue" alt="GPL-2.0 license" />
  </p>
</div>

TGF monitors Telegram channels and automatically forwards new messages to a destination channel. Lightweight, single-binary, no *bloat*.

## Features

- **Multi-source** – Forward from many channels into one
- **Reply chains** – Forwards replies together with their parent messages
- **Media albums** – Groups album messages and forwards them as a batch
- **Deduplication** – Tracks forwarded messages so nothing is sent twice
- **Configurable delay** – Set a delay between forwards to avoid rate limits
- **History window** – Only forward messages from the last N hours
- **Sequential forwarding** – Collects all history first, then forwards messages in chronological order (asc/desc)
- **Backfill resume** – Persists scan state across restarts so no messages are missed
- **Dashboard** – Live terminal UI showing status (disable with `-d`)
- **Single binary** – TDLib is vendored; no system deps needed

## Requirements

- GCC or Clang
- A [Telegram API ID and hash](https://my.telegram.org/apps)
- CMake & `make` (only if rebuilding TDLib from source)

## Quick Start

```bash
git clone https://github.com/Hadi493/tgf-c.git
cd tgf-c

cp config.json.example config.json
# edit config.json with your API credentials and channels

gcc -o nob nob.c
./nob
./tgf
```

## Configuration

Edit `config.json`:

```json
{
    "api_id": YOUR_API_ID,
    "api_hash": "YOUR_API_HASH",
    "source_channels": ["@channel1", "@channel2"],
    "dest_channel": "@me",
    "history_file": "history.txt",
    "forward_delay_sec": 10,
    "enable_sequential_forwarding": true,
    "sequence_direction": "asc",
    "history_window_hours": 24
}
```

| Field | Description |
|---|---|
| `api_id` / `api_hash` | From [my.telegram.org](https://my.telegram.org/apps) |
| `source_channels` | Channels to monitor |
| `dest_channel` | Where to forward (`@me` for Saved Messages) |
| `forward_delay_sec` | Seconds between forwards (default: 1) |
| `enable_sequential_forwarding` | Enable sequential (ordered) forwarding (default: false) |
| `sequence_direction` | Sort order: `"asc"` (old→new) or `"desc"` (new→old) (default: asc) |
| `history_window_hours` | Only forward messages newer than this (default: 24, `0` = all) |

## Usage

```bash
./tgf         # normal mode – shows dashboard
./tgf -d      # debug mode – verbose logs, no dashboard
```

On first run you'll be prompted for your phone number, verification code, and 2FA password (if enabled). Session is cached in `tdlib_db/`.

## License

[GNU GPLv2](LICENSE)

## Contributing

No strict guidelines – just follow the existing code style and keep it simple.
