# SUPER STREETFIGHTER II DEADCODES (US VERSION)

> (ACTION REPLAY II ONLY)

[Dead Code Overview](preservaction-deadc0de.md)


## Downloads

- [Hexdump](deadc0de/hexdumps/super_streetfighter_2_us.hex)
- [Binary](deadc0de/binaries/super_streetfighter_2_us.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/super_streetfighter_2_us.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/super_streetfighter_2_us.asm)

## Next Stage

> Player 1 moves on to the next battle when the select button is pressed

### Input

```hex
DEADCODE
C0025C07
ADA800C9
0020D00A
E230A9FF
8F71077E
C230AD1A
42AA5C60
02C00000
```
> The original cheat code includes a wrong length byte (should be `07` instead of `09`)

**Original:**

```hex
DEADCODE
C0025C09
ADA800C9
0020D00A
E230A9FF
8F71077E
C230AD1A
42AA5C60
02C00000
```

## Analysis

**Header**

```
DE AD C0 DE
C0 02 5C 07
```
Translates to:
- Signature = `DEADC0DE`
- Hook address = `$C0:025C`
- Lengeth = `0x07` -> 7 * 4 = 28 bytes of code follow

**Segment Disassembly (65C816)**

```assambler
--------------------------------
LDA $00A8           ; AD A8 00      - load A from address $00A8 (address uses current DBR; in game
                    ;                 context this is typically WRAM)
CMP #$00            ; C9 00         - compare A with 0x00 (sets N/Z/C flags)
JSR $0AD0           ; 20 D0 0A      - call original subroutine at $0AD0; returns with RTS. This
                    ;                 replays original game logic that was overwritten by the hook
SEP #$30            ; E2 30         - set M and X flags (force 8-bit accumulator and 8-bit index)
LDA #$FF            ; A9 FF         - load immediate 0xFF into A (now 8-bit)
STA $7E:0771        ; 8F 71 07 7E   - store A to WRAM $7E:0771   <-- core cheat write
REP #$30            ; C2 30         - clear M and X flags (restore 16-bit accumulator/index)
LDA $421A           ; AD 1A 42      - read I/O register $421A (timing/status read)
TAX                 ; AA            - transfer A -> X (may be used for side-effect or register sync)
JML $C0:0260        ; 5C 60 02 C0   - long jump to $C0:0260 (hook + 4)
00 00               ;               - (padding / alignment)
```

**Operational notes**
- The header's len byte denotes how many 4-byte words the loader will copy/execute.
  Here len=`07` -> 28 bytes follow; the trainer loader treats following bytes as a contiguous
  machine-code blob (the CPU fetches bytes serially; instruction boundaries can cross segment
  boundaries).
- The routine intentionally replays original instructions (`LDA/CMP/JSR`) that were replaced
  at the hook site so original semantics are preserved.
- `SEP/REP` pair is critical: `SEP #$30` forces `A/X` to 8-bit so that `LDA #imm` behaves as a
  single-byte load and `STA` writes one byte. `REP` restores the original width before returning control.
- `STA` long (opcode `8F`) includes an explicit bank byte: `8F 71 07 7E` -> store to `$7E:0771` (WRAM).
- The final `JML` targets the same offset in the execution bank; conventions typically use
  "return = hook_offset + 4" so the CPU resumes at the instruction following the overwritten bytes.
- `DBR` (data bank register) affects absolute 16-bit loads/stores (`AD` / `8D`): `LDA $00A8` reads `DBR:$00A8`.
  The long `STA` (`8F`) used here is independent of `DBR` because its operand includes the bank byte.
- Padding bytes are used to align the blob; they are not executed.
