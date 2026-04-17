#!/usr/bin/env python3
"""
upgrade_effects.py — Upgrade all community CSV rumble profiles to use richer effects.

Run from the repo root:
    python rumble/upgrade_effects.py

What it does:
- Reads every .csv in rumble/community_csv/snes/ and genesis/
- Rewrites effect, amplitude, duration_ms, and cooldown_ms columns
  based on the event_id keyword patterns below
- Writes the modified files back in-place (originals overwritten)
"""

import csv
import io
import os
import re
import sys

# ---------------------------------------------------------------------------
# Rules: keyed by lowercase pattern found anywhere in event_id
# Each rule: (effect, min_amplitude, min_duration_ms, cooldown_ms)
# None means "leave as-is"
# ---------------------------------------------------------------------------
RULES = [
    # Death / game over — heartbeat is the most dramatic
    (r'death|game_over|gameover',
     'heartbeat', 1.00, 300, 400),

    # Level complete — celebratory heartbeat
    (r'level_complete|levelcomplete|stage_clear|stageclear|world_clear|worldclear',
     'heartbeat', 0.90, 260, 400),

    # Life lost — dramatic swell
    (r'life_lost|lifelost|lives_lost|liveslost',
     'fade_in_out', 0.92, 220, 250),

    # Damage taken — sharp fade-out (impact feel)
    (r'damage|hit_taken|hittaken|hurt|health_dec|healthdec',
     'fade_out', 0.78, 110, 80),

    # Life gained — reward build-up
    (r'life_gain|lifegain|lives_gain|livesgain|extra_life|extralife',
     'fade_in', 0.70, 130, 200),

    # Powerup gained — triumphant swell
    (r'powerup|power_up|powergain|star|invincib',
     'fade_in', 0.68, 120, 150),

    # Enemy defeated — rapid burst (satisfying)
    (r'enemy|boss_hit|bosshit|kill|defeat|destroy',
     'burst', 0.55, 80, 50),

    # Score increase — light machine-gun
    (r'^score',
     'burst', 0.35, 70, 15),

    # Pickup / collect — normal stereo sweep (fast, light)
    (r'pickup|pick_up|collect|coin|ring|gem|item|grab',
     'normal', 0.28, 22, 12),
]

# Compile the patterns
COMPILED_RULES = [(re.compile(pat, re.IGNORECASE), eff, min_amp, min_dur, cd)
                  for pat, eff, min_amp, min_dur, cd in RULES]


def classify(event_id: str):
    """Return (effect, min_amplitude, min_duration_ms, cooldown_ms) or None."""
    for pattern, effect, min_amp, min_dur, cd in COMPILED_RULES:
        if pattern.search(event_id):
            return effect, min_amp, min_dur, cd
    return None


def upgrade_csv(path: str) -> bool:
    """Read, upgrade, and write back a single CSV file. Returns True if changed."""
    with open(path, 'r', encoding='utf-8', newline='') as f:
        raw = f.read()

    lines = raw.splitlines(keepends=True)
    header_idx = None
    header_cols = None

    # Find the header row (first non-comment non-empty line)
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped and not stripped.startswith('#'):
            header_cols = [c.strip().lower() for c in stripped.split(',')]
            header_idx = i
            break

    if header_idx is None or header_cols is None:
        return False  # no header found

    # Required columns
    required = {'event_id', 'effect', 'amplitude', 'duration_ms', 'cooldown_ms'}
    if not required.issubset(set(header_cols)):
        return False

    col = {name: idx for idx, name in enumerate(header_cols)}

    changed = False
    out_lines = list(lines)

    for i in range(header_idx + 1, len(lines)):
        line = lines[i]
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue

        # Parse fields preserving quotes
        reader = csv.reader(io.StringIO(stripped))
        try:
            fields = next(reader)
        except StopIteration:
            continue

        # Pad if needed
        while len(fields) <= max(col.values()):
            fields.append('')

        event_id = fields[col['event_id']].strip()
        rule = classify(event_id)
        if rule is None:
            continue

        effect, min_amp, min_dur, cd = rule

        # Apply effect
        old_effect = fields[col['effect']].strip()
        if old_effect != effect:
            fields[col['effect']] = effect
            changed = True

        # Apply minimum amplitude
        try:
            amp = float(fields[col['amplitude']])
        except ValueError:
            amp = 0.0
        new_amp = max(amp, min_amp)
        new_amp = min(new_amp, 1.0)
        if abs(new_amp - amp) > 0.001:
            fields[col['amplitude']] = f'{new_amp:.2f}'
            changed = True

        # Apply minimum duration
        try:
            dur = int(fields[col['duration_ms']])
        except ValueError:
            dur = 0
        new_dur = max(dur, min_dur)
        if new_dur != dur:
            fields[col['duration_ms']] = str(new_dur)
            changed = True

        # Apply cooldown
        try:
            old_cd = int(fields[col['cooldown_ms']])
        except ValueError:
            old_cd = cd
        if old_cd != cd:
            fields[col['cooldown_ms']] = str(cd)
            changed = True

        if changed:
            # Rebuild the line preserving the original line ending
            ending = ''
            if line.endswith('\r\n'):
                ending = '\r\n'
            elif line.endswith('\n'):
                ending = '\n'
            out_buf = io.StringIO()
            writer = csv.writer(out_buf)
            writer.writerow(fields)
            out_lines[i] = out_buf.getvalue().rstrip('\r\n') + ending

    if not changed:
        return False

    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.writelines(out_lines)
    return True


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    csv_root = os.path.join(script_dir, 'community_csv')

    total = 0
    updated = 0
    for system in ('snes', 'genesis'):
        system_dir = os.path.join(csv_root, system)
        if not os.path.isdir(system_dir):
            continue
        for fname in sorted(os.listdir(system_dir)):
            if not fname.endswith('.csv'):
                continue
            path = os.path.join(system_dir, fname)
            total += 1
            try:
                if upgrade_csv(path):
                    updated += 1
                    print(f'  updated: {system}/{fname}')
            except Exception as e:
                print(f'  ERROR {fname}: {e}', file=sys.stderr)

    print(f'\nDone: {updated}/{total} files updated.')


if __name__ == '__main__':
    main()
