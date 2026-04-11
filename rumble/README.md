# Rumble Data

This folder contains two different kinds of rumble data.

## `merged_inventory/`

This is an internal inventory.

It is not the final community-facing format. It is a merged collection built from the sources we scraped or generated, such as:

- `openai-retro`
- manually added profiles
- other imported mappings

So `merged_inventory` mostly means:

- what we found
- what looked useful
- what we merged into one place for packaging

Some entries are stronger than others. Some are well-tested, some are only best-effort mappings. It is mainly useful as a source pool for building the app bundle.

## `community_csv/`

This is the editable community format.

These CSV files are the format the app is designed to understand at runtime from the Quest storage folder. If users place matching CSV files in:

- `/storage/emulated/0/QuestRetroDepth/rumble/snes`
- `/storage/emulated/0/QuestRetroDepth/rumble/genesis`

then the app will try to use those files first.

Runtime source order is:

1. user CSV in `QuestRetroDepth/rumble/...`
2. bundled internal profile from the APK
3. no profile

If a user CSV matches the current game and is valid, it overrides the bundled profile.

If a user CSV matches but is broken, the app logs the parse error and falls back to the bundled profile when possible.

## Quest Folder Layout

Current external storage layout on the headset:

- ROMs:
  - `/storage/emulated/0/QuestRetroDepth/roms/snes`
  - `/storage/emulated/0/QuestRetroDepth/roms/genesis`
- Rumble CSV:
  - `/storage/emulated/0/QuestRetroDepth/rumble/snes`
  - `/storage/emulated/0/QuestRetroDepth/rumble/genesis`
- Settings:
  - `/storage/emulated/0/QuestRetroDepth/config/snes`
  - `/storage/emulated/0/QuestRetroDepth/config/genesis`

## Community CSV Format

Each game uses one CSV file.

Recommended location examples:

- `QuestRetroDepth/rumble/snes/supermarioworld_snes.csv`
- `QuestRetroDepth/rumble/genesis/sonicthehedgehog_genesis.csv`

Each file can begin with metadata comment lines:

```csv
# schema=qrd_rumble_csv_v1
# game_name=Sonic The Hedgehog
# match_names=sonic the hedgehog|sonicthehedgehog
```

Then one CSV header row and one row per rumble event:

```csv
event_id,address,size,value_type,condition,compare_value,controller,amplitude,effect,start_ms,duration_ms,cooldown_ms,priority,notes
pickup_rings,0xFFFE20,2,uint,prev_lt_curr,,right,0.50,normal,0,36,35,20,Rings increased
```

## Column Reference

### `event_id`

Unique identifier for the rumble event.

Examples:

- `pickup_rings`
- `damage_taken`
- `life_lost`

This is mainly for readability, debugging, and logs.

### `address`

Absolute emulator RAM address to watch.

Examples:

- SNES: `0x7E0DBE`
- Genesis: `0xFFFE20`

The runtime watches this address every frame.

### `size`

Number of bytes to read.

Allowed values:

- `1`
- `2`
- `4`

### `value_type`

How the watched bytes should be interpreted.

Supported values:

- `uint`
- `int`
- `bcd`
- `nibble`

Use `uint` unless the value is known to be signed or specially encoded.

### `condition`

Defines what change should trigger rumble.

Supported values:

- `prev_gt_curr`
- `prev_lt_curr`
- `changed`
- `became_zero`
- `became_nonzero`
- `equals`
- `not_equals`

Examples:

- health decreased -> `prev_gt_curr`
- score increased -> `prev_lt_curr`
- mode/state changed -> `changed`
- death flag or lives reaching zero -> `became_zero`

### `compare_value`

Optional value used only with:

- `equals`
- `not_equals`

Leave blank for the other conditions.

Example:

```csv
mode_flag,0x7E1234,1,uint,equals,3,both,0.80,fade_in_out,0,120,100,70,Boss mode entered
```

### `controller`

Which controller side the event belongs to.

Allowed values:

- `left`
- `right`
- `both`

Runtime behavior:

- `left`: left-first stereo wave, then right
- `right`: right-first stereo wave, then left
- `both`: moving wave across both controllers

### `amplitude`

Vibration intensity from `0.0` to `1.0`.

Examples:

- `0.25` = light
- `0.50` = medium
- `0.90` = strong

The runtime may scale this for feel, but the CSV value is the authored base intensity.

### `effect`

Amplitude envelope for the pulse.

Supported values:

- `normal`
- `fade_in`
- `fade_out`
- `fade_in_out`

Meaning:

- `normal`: constant strength
- `fade_in`: grows stronger
- `fade_out`: starts strong and decays
- `fade_in_out`: swells and relaxes

### `start_ms`

Delay before the rumble starts, in milliseconds.

Examples:

- `0` = immediate
- `40` = wait 40 ms before starting

### `duration_ms`

How long the pulse lasts, in milliseconds.

Examples:

- `18`
- `60`
- `180`

This is the pulse length, not the cooldown.

### `cooldown_ms`

Minimum delay before the same event can trigger again.

This reduces spam for rapidly changing values.

Examples:

- pickups: `20` to `50`
- damage: `80` to `150`
- life lost / death: `150` to `400`

### `priority`

Used when several events fire at the same time.

Higher number = more important.

Typical pattern:

- score/pickup -> lower priority
- damage/life lost -> higher priority
- death/game over -> highest priority

### `notes`

Optional human-readable explanation.

Ignored by runtime. Safe place to explain what the row does.

## Example

```csv
# schema=qrd_rumble_csv_v1
# game_name=Super Mario World
# match_names=super mario world|supermarioworld|super mario world esp
event_id,address,size,value_type,condition,compare_value,controller,amplitude,effect,start_ms,duration_ms,cooldown_ms,priority,notes
coin_pickup,0x7E0DBF,1,uint,prev_lt_curr,,right,0.30,normal,0,24,30,20,Coin increased
damage_taken,0x7E0DBE,1,uint,prev_gt_curr,,left,0.85,fade_out,0,80,100,70,Player hurt
life_lost,0x7E0DBE,1,uint,prev_gt_curr,,both,1.00,fade_in_out,0,220,250,90,Life count decreased
```

## Current Workflow

1. Internal sources are merged into `merged_inventory/`
2. Those are packaged into internal `.qrr` files for the APK
3. We can export those `.qrr` files into `community_csv/`
4. Users can edit or replace CSV files in the Quest `rumble/` folder
5. The app reads those user CSV files directly at runtime

So:

- `merged_inventory` = internal collected source material
- `community_csv` = human-editable runtime format

## Notes

- The exported CSV collection is a starting point, not a perfect round-trip.
- Some values in exported CSV are inferred from internal profiles, especially cooldown and priority.
- If a CSV is malformed, the app logs the error through the rumble log tag and may fall back to the bundled profile.
