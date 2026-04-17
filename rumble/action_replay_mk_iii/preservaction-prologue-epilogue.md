# Project Preservaction

[Table Of Contents](preservaction-toc.md)

## Prologue/Epilogue Variants

It's interesting to see that the prologue and epilogue sections belong together as pairs: the prologue sets up the environment for the cheat data blocks, and the epilogue restores the original state and jumps back into the game so that execution can continue uninterrupted.

Taken in context together with the cheat data blocks, they form tiny self-contained opcode programs that temporarily take control of the CPU to modify memory before returning to normal gameplay.

### Variant 1 (Cybernator UK)

**Prologue**

```hex
8F004200
```

| Byte       | Opcode                | Meaning                                     |
|------------|-----------------------|---------------------------------------------|
| `8F`       | `STA` (absolute long) | Store A to address $004200                  |
| `00 42 00` | Operand               | The NMI/timer control register in I/O space |

> This prologue doesn't perform any CPU setup - instead, it preserves the original game logic. These four bytes correspond to the instruction that was overwritten by the trainer hook. The trainer replays this instruction first to maintain the game's intended behavior before applying any cheat modifications.

**Epilogue**

```hex
5C7C8280
```

| Byte       | Opcode  | Meaning                                           |
|------------|---------|---------------------------------------------------|
| `5C`       | `JML`   | Long jump to absolute 24-bit address.             |
| `7C 82 80` | Operand | Jump target `$80:827C` (start address + 4 bytes). |

> The epilogue performs a long jump back to the instruction immediately following the hook point at `$80:827C`. This returns control to the game's normal execution flow once all cheat operations have completed, ensuring the gameplay continues seamlessly without disruption.

---

### Variant 2 (Mystical Ninja 2 JP)

**Prologue**

```hex
E230EAEA
```

| Byte    | Opcode     | Effect                                                                                              |
|---------|------------|-----------------------------------------------------------------------------------------------------|
| `E2 30` | `SEP #$30` | Set processor status bits for 8-bit accumulator **and** 8-bit index registers (sets M and X flags). |
| `EA EA` | `NOP; NOP` | No operation (padding / alignment).                                                                 |

> This prologue prepares the CPU for the cheat operations by switching both the accumulator and index registers to 8-bit mode. This guarantees that all immediate load instructions (`A9 vv`) in the cheat data blocks are interpreted as 8-bit values. (The two NOP bytes serve only as padding and alignment to maintain consistent block size and timing.)

**Epilogue**

```hex
C2105CB8
87800000
```

| Byte          | Opcode         | Meaning                                               |
|---------------|----------------|-------------------------------------------------------|
| `C2 10`       | `REP #$10`     | Clear X flag <- set **16-bit index registers** again. |
| `5C B8 87 80` | `JML $80:87B8` | Return to address `$80:87B8` (entry + 4).             |
| `00 00`       | -              | Padding.                                              |

> The epilogue restores the index registers to 16-bit mode, reversing the changes made by the prologue. After all cheat data blocks have executed, the routine performs a long jump back to `$80:87B8`, the instruction immediately following the hook point. This ensures the trainer exits cleanly and the game resumes normal execution without any disruption.

---

### Variant 3 (Super Taz Mania US)

**Prologue**

```hex
08E220EA
```

| Byte    | Opcode     | Meaning                                                                                    |
|---------|------------|--------------------------------------------------------------------------------------------|
| `08`    | `PHP`      | Push Processor Status Register onto the stack (saves current flags).                       |
| `E2 20` | `SEP #$20` | Set only the **M flag** <- forces **8-bit accumulator**, leaves index registers untouched. |
| `EA`    | `NOP`      | Padding byte.                                                                              |

> This prologue safeguards the current processor state by saving the status flags to the stack, then switches the CPU into 8-bit accumulator mode to ensure the cheat data blocks execute consistently. (The single NOP instruction acts as harmless filler for alignment and timing consistency within the trainer code.)

**Epilogue**

```hex
2868AF11
42005C15
FDC00000
```

| Byte          | Opcode         | Meaning                                                     |
|---------------|----------------|-------------------------------------------------------------|
| `28`          | `PLP`          | Pull Processor Status (restore flags).                      |
| `68`          | `PLA`          | Restore Accumulator.                                        |
| `5C 15 FD C0` | `JML $C0:FD15` | Jump to same offset as start, different bank (ROM mapping). |
| `00 00`       | -              | Padding.                                                    |

> The epilogue cleanly restores both the processor flags and the accumulator, reversing the changes made by the prologue. It then performs a long jump to `$C0:FD15`, which corresponds to the same offset as the hook entry (`$00:FD15`) but within the active ROM execution bank. This ensures that once all cheat operations are complete, the game continues seamlessly from its original instruction flow.

---

### Variant 4 (Wolfenstein 3D)

**Prologue**

```hex
4808E230
```

| Byte    | Opcode     | Meaning                                                                       |
|---------|------------|-------------------------------------------------------------------------------|
| `48`    | `PHA`      | Push Accumulator onto the stack (save A).                                     |
| `08`    | `PHP`      | Push Processor Status Register onto the stack (save flags).                   |
| `E2 30` | `SEP #$30` | Set bits M and X → force **8-bit accumulator** and **8-bit index registers**. |

> This prologue saves the accumulator and CPU status flags, then switches the processor into 8-bit mode for both the accumulator and index registers. This ensures that the following cheat data blocks - which use 8-bit immediate loads (`A9 vv`) - operate correctly regardless of the game's current CPU mode.

---

**Epilogue**

```hex
2868AF11
42005C15
FDC00000
```

| Byte          | Opcode         | Meaning                                                                                             |
|---------------|----------------|-----------------------------------------------------------------------------------------------------|
| `28`          | `PLP`          | Pull Processor Status from stack (restore flags).                                                   |
| `68`          | `PLA`          | Pull Accumulator from stack (restore A).                                                            |
| `AF 11 42 00` | `LDA $004211`  | Read hw register `$004211` (NMI flag / joypad latch). Commonly a safe timing read before returning. |
| `5C 15 FD C0` | `JML $C0:FD15` | Long jump back to `$C0:FD15` - the same offset as the hook address, but in the executable ROM bank. |
| `00 00`       | -              | Padding / alignment bytes.                                                                          |

> The epilogue restores both the accumulator and the status register, performs a harmless read from a hardware register (a timing-safe filler operation), and then jumps back to `$C0:FD15`. This address corresponds to the same offset as the hook entry (`$00:FD15`) but in the active ROM bank. Together with the prologue, this forms a complete, reversible routine: the prologue establishes a controlled CPU environment, the cheat data blocks perform memory writes, and the epilogue restores the state and returns execution to the game.
