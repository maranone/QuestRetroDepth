# Project Preservaction

[Table Of Contents](preservaction-toc.md)
[Dead Code Overview](preservaction-deadc0de.md)

## Opcodes Used By `DEADC0DE`

Opcodes, short for "operation codes", are the fundamental building blocks of machine language, which is the lowest level of programming language a computer or processor understands. Each opcode represents a specific instruction for the CPU to perform, like adding two numbers, moving data between memory locations, or comparing values.

In assembly language, a human-readable form of machine code, opcodes are usually written as abbreviated commands (like ADD, MOV, JMP) that correspond to specific binary or hexadecimal codes the CPU interprets directly.

### List of Verified Opcodes

These opcodes were used in dead code cheats, so they are assumed to be working:

| Opcode | Human-Readable Abbreviation             | Description                                        |
|--------|-----------------------------------------|----------------------------------------------------|
| 00     | BRK                                     | Break                                              |
| 02     | COP                                     | Coprocessor Enable                                 |
| 08     | PHP                                     | Push Processor Status                              |
| 09     | ORA Immediate                           | OR Accumulator with Immediate Value                |
| 15     | ORA Direct Page, X                      | OR Accumulator with Direct Page, X                 |
| 20     | JSR                                     | Jump to Subroutine                                 |
| 25     | AND Direct Page                         | AND Accumulator with Direct Page                   |
| 28     | PLP                                     | Pull Processor Status                              |
| 29     | AND Immediate                           | AND Accumulator with Immediate Value               |
| 48     | PHA                                     | Push Accumulator                                   |
| 5A     | PHY                                     | Push Y Register                                    |
| 5C     | JML                                     | Jump Long                                          |
| 64     | STZ Direct Page                         | Store Zero to Direct Page                          |
| 68     | PLA                                     | Pull Accumulator                                   |
| 6A     | ROR Accumulator                         | Rotate Right Accumulator                           |
| 78     | SEI                                     | Set Interrupt Disable                              |
| 7A     | PLY                                     | Pull Y Register                                    |
| 87     | STA Direct Page                         | Store Accumulator to Direct Page                   |
| 89     | BIT Immediate                           | Test Bits Immediate                                |
| 8D     | STA Absolute                            | Store Accumulator to Absolute Address              |
| 8F     | STA Absolute Long                       | Store Accumulator to Absolute Long Address         |
| 9A     | TXS                                     | Transfer X Register to Stack Pointer               |
| 9B     | TXY                                     | Transfer X to Y                                    |
| 9C     | STZ Absolute                            | Store Zero to Absolute Address                     |
| 9F     | STA Absolute Long, X                    | Store Accumulator to Absolute Long Address, X      |
| A2     | LDX Immediate                           | Load X Register with Immediate Value               |
| A5     | LDA Direct Page                         | Load Accumulator from Direct Page                  |
| A9     | LDA Immediate                           | Load Accumulator with Immediate Value              |
| AA     | TAX                                     | Transfer Accumulator to X Register                 |
| AD     | LDA Absolute                            | Load Accumulator from Absolute Address             |
| B4     | LDY Zero Page, X                        | Load Y Register from Zero Page, X                  |
| C0     | CPY Immediate                           | Compare Y Register with Immediate Value            |
| C2     | REP                                     | Reset Processor Status Bits                        |
| C9     | CMP Immediate                           | Compare Accumulator with Immediate Value           |
| D0     | BNE                                     | Branch if Not Equal                                |
| E0     | CPX Immediate                           | Compare X Register with Immediate Value            |
| E2     | SEP                                     | Set Processor Status Bits                          |
| EA     | NOP                                     | No Operation                                       |
| FC     | COP                                     | Coprocessor Interrupt Vector                       |
| F0     | BEQ                                     | Branch if Equal                                    |

### List of Unverified Opcodes

These opcodes were not used in dead code cheats and therefore were not verified as working:

| Opcode | Human-Readable Abbreviation            | Description                                          |
|--------|----------------------------------------|------------------------------------------------------|
| 01     | ORA (Direct Page, X)                   | OR Accumulator with Direct Page, X                   |
| 03     | ORA (Stack Relative)                   | OR Accumulator with Stack Relative                   |
| 04     | TSB (Direct Page)                      | Test and Set Bits in Memory (Direct Page)            |
| 05     | ORA (Direct Page)                      | OR Accumulator with Direct Page                      |
| 06     | ASL (Direct Page)                      | Arithmetic Shift Left (Direct Page)                  |
| 07     | ORA (Indirect Long)                    | OR Accumulator with Indirect Long                    |
| 0A     | ASL (Accumulator)                      | Arithmetic Shift Left (Accumulator)                  |
| 0B     | PHD                                    | Push Direct Page Register                            |
| 0C     | TSB (Absolute)                         | Test and Set Bits in Memory (Absolute)               |
| 0D     | ORA (Absolute)                         | OR Accumulator with Absolute                         |
| 0E     | ASL (Absolute)                         | Arithmetic Shift Left (Absolute)                     |
| 0F     | ORA (Absolute Long)                    | OR Accumulator with Absolute Long                    |
| 10     | BPL                                    | Branch if Positive                                   |
| 11     | ORA (Indirect, Y)                      | OR Accumulator with Indirect, Y                      |
| 12     | ORA (Indirect)                         | OR Accumulator with Indirect                         |
| 13     | ORA (Stack Relative, Y)                | OR Accumulator with Stack Relative, Y                |
| 14     | TRB (Direct Page)                      | Test and Reset Bits in Memory (Direct Page)          |
| 16     | ASL (Direct Page, X)                   | Arithmetic Shift Left (Direct Page, X)               |
| 17     | ORA (Indirect Long, Y)                 | OR Accumulator with Indirect Long, Y                 |
| 18     | CLC                                    | Clear Carry Flag                                     |
| 19     | ORA (Absolute, Y)                      | OR Accumulator with Absolute, Y                      |
| 1A     | INC (Accumulator)                      | Increment Accumulator                                |
| 1B     | TCS                                    | Transfer Accumulator to Stack Pointer                |
| 1C     | TRB (Absolute)                         | Test and Reset Bits in Memory (Absolute)             |
| 1D     | ORA (Absolute, X)                      | OR Accumulator with Absolute, X                      |
| 1E     | ASL (Absolute, X)                      | Arithmetic Shift Left (Absolute, X)                  |
| 1F     | ORA (Absolute Long, X)                 | OR Accumulator with Absolute Long, X                 |
| 21     | AND (Direct Page, X)                   | AND Accumulator with Direct Page, X                  |
| 22     | JSL                                    | Jump to Subroutine Long                              |
| 23     | AND (Stack Relative)                   | AND Accumulator with Stack Relative                  |
| 24     | BIT (Direct Page)                      | Test Bits in Accumulator (Direct Page)               |
| 26     | ROL (Direct Page)                      | Rotate Left (Direct Page)                            |
| 27     | AND (Indirect Long)                    | AND Accumulator with Indirect Long                   |
| 2A     | ROL (Accumulator)                      | Rotate Left (Accumulator)                            |
| 2B     | PLD                                    | Pull Direct Page Register                            |
| 2C     | BIT (Absolute)                         | Test Bits in Accumulator (Absolute)                  |
| 2D     | AND (Absolute)                         | AND Accumulator with Absolute                        |
| 2E     | ROL (Absolute)                         | Rotate Left (Absolute)                               |
| 2F     | AND (Absolute Long)                    | AND Accumulator with Absolute Long                   |
| 30     | BMI                                    | Branch if Minus                                      |
| 31     | AND (Indirect, Y)                      | AND Accumulator with Indirect, Y                     |
| 32     | AND (Indirect)                         | AND Accumulator with Indirect                        |
| 33     | AND (Stack Relative, Y)                | AND Accumulator with Stack Relative, Y               |
| 34     | BIT (Direct Page, X)                   | Test Bits in Accumulator (Direct Page, X)            |
| 35     | AND (Direct Page, X)                   | AND Accumulator with Direct Page, X                  |
| 36     | ROL (Direct Page, X)                   | Rotate Left (Direct Page, X)                         |
| 37     | AND (Indirect Long, Y)                 | AND Accumulator with Indirect Long, Y                |
| 38     | SEC                                    | Set Carry Flag                                       |
| 39     | AND (Absolute, Y)                      | AND Accumulator with Absolute, Y                     |
| 3A     | DEC (Accumulator)                      | Decrement Accumulator                                |
| 3B     | TSC                                    | Transfer Stack Pointer to Accumulator                |
| 3C     | BIT (Absolute, X)                      | Test Bits in Accumulator (Absolute, X)               |
| 3D     | AND (Absolute, X)                      | AND Accumulator with Absolute, X                     |
| 3E     | ROL (Absolute, X)                      | Rotate Left (Absolute, X)                            |
| 3F     | AND (Absolute Long, X)                 | AND Accumulator with Absolute Long, X                |
| 40     | RTI                                    | Return from Interrupt                                |
| 41     | EOR (Direct Page, X)                   | Exclusive OR Accumulator with Direct Page, X         |
| 42     | WDM                                    | Reserved for Future Expansion                        |
| 43     | EOR (Stack Relative)                   | Exclusive OR Accumulator with Stack Relative         |
| 44     | MVP                                    | Block Move Positive                                  |
| 45     | EOR (Direct Page)                      | Exclusive OR Accumulator with Direct Page            |
| 46     | LSR (Direct Page)                      | Logical Shift Right (Direct Page)                    |
| 47     | EOR (Indirect Long)                    | Exclusive OR Accumulator with Indirect Long          |
| 4A     | LSR (Accumulator)                      | Logical Shift Right (Accumulator)                    |
| 4B     | PHK                                    | Push Program Bank Register                           |
| 4C     | JMP                                    | Jump to Address (Absolute)                           |
| 4D     | EOR (Absolute)                         | Exclusive OR Accumulator with Absolute               |
| 4E     | LSR (Absolute)                         | Logical Shift Right (Absolute)                       |
| 4F     | EOR (Absolute Long)                    | Exclusive OR Accumulator with Absolute Long          |
| 50     | BVC                                    | Branch if Overflow Clear                             |
| 51     | EOR (Indirect, Y)                      | Exclusive OR Accumulator with Indirect, Y            |
| 52     | EOR (Indirect)                         | Exclusive OR Accumulator with Indirect               |
| 53     | EOR (Stack Relative, Y)                | Exclusive OR Accumulator with Stack Relative, Y      |
| 54     | MVN                                    | Block Move Negative                                  |
| 56     | LSR (Direct Page, X)                   | Logical Shift Right (Direct Page, X)                 |
| 57     | EOR (Indirect Long, Y)                 | Exclusive OR Accumulator with Indirect Long, Y       |
| 58     | CLI                                    | Clear Interrupt Disable                              |
| 59     | EOR (Absolute, Y)                      | Exclusive OR Accumulator with Absolute, Y            |
| 5B     | TCD                                    | Transfer Accumulator to Direct Page Register         |
| 5D     | EOR (Absolute, X)                      | Exclusive OR Accumulator with Absolute, X            |
| 5E     | LSR (Absolute, X)                      | Logical Shift Right (Absolute, X)                    |
| 5F     | EOR (Absolute Long, X)                 | Exclusive OR Accumulator with Absolute Long, X       |
| 60     | RTS                                    | Return from Subroutine                               |
| 61     | ADC (Direct Page, X)                   | Add with Carry from Direct Page, X                   |
| 63     | ADC (Stack Relative)                   | Add with Carry from Stack Relative                   |
| 65     | ADC (Direct Page)                      | Add with Carry from Direct Page                      |
| 66     | ROR (Direct Page)                      | Rotate Right (Direct Page)                           |
| 67     | ADC (Indirect Long)                    | Add with Carry from Indirect Long                    |
| 69     | ADC (Immediate)                        | Add with Carry with Immediate                        |
| 6B     | RTL                                    | Return from Subroutine Long                          |
| 6C     | JMP (Indirect)                         | Jump to Address (Indirect)                           |
| 6D     | ADC (Absolute)                         | Add with Carry from Absolute                         |
| 6E     | ROR (Absolute)                         | Rotate Right (Absolute)                              |
| 6F     | ADC (Absolute Long)                    | Add with Carry from Absolute Long                    |
| 70     | BVS                                    | Branch if Overflow Set                               |
| 71     | ADC (Indirect, Y)                      | Add with Carry from Indirect, Y                      |
| 72     | ADC (Indirect)                         | Add with Carry from Indirect                         |
| 73     | ADC (Stack Relative, Y)                | Add with Carry from Stack Relative, Y                |
| 74     | STZ (Direct Page, X)                   | Store Zero to Direct Page, X                         |
| 75     | ADC (Direct Page, X)                   | Add with Carry from Direct Page, X                   |
| 76     | ROR (Direct Page, X)                   | Rotate Right (Direct Page, X)                        |
| 77     | ADC (Indirect Long, Y)                 | Add with Carry from Indirect Long, Y                 |
| 79     | ADC (Absolute, Y)                      | Add with Carry from Absolute, Y                      |
| 7B     | TDC                                    | Transfer Direct Page Register to Accumulator         |
| 7C     | JMP (Absolute Indexed Indirect)        | Jump to Address (Absolute Indexed Indirect)          |
| 7D     | ADC (Absolute, X)                      | Add with Carry from Absolute, X                      |
| 7E     | ROR (Absolute, X)                      | Rotate Right (Absolute, X)                           |
| 7F     | ADC (Absolute Long, X)                 | Add with Carry from Absolute Long, X                 |
| 81     | STA (Direct Page, X)                   | Store Accumulator to Direct Page, X                  |
| 83     | STA (Stack Relative)                   | Store Accumulator to Stack Relative                  |
| 84     | STY (Direct Page)                      | Store Y Register to Direct Page                      |
| 85     | STA (Direct Page)                      | Store Accumulator to Direct Page                     |
| 86     | STX (Direct Page)                      | Store X Register to Direct Page                      |
| 88     | DEY                                    | Decrement Y Register                                 |
| 8A     | TXA                                    | Transfer X Register to Accumulator                   |
| 8B     | PHB                                    | Push Data Bank Register                              |
| 8C     | STY (Absolute)                         | Store Y Register to Absolute                         |
| 8E     | STX (Absolute)                         | Store X Register to Absolute                         |
| 90     | BCC                                    | Branch if Carry Clear                                |
| 91     | STA (Indirect, Y)                      | Store Accumulator to Indirect, Y                     |
| 92     | STA (Indirect)                         | Store Accumulator to Indirect                        |
| 93     | STA (Stack Relative, Y)                | Store Accumulator to Stack Relative, Y               |
| 94     | STY (Direct Page, X)                   | Store Y Register to Direct Page, X                   |
| 95     | STA (Direct Page, X)                   | Store Accumulator to Direct Page, X                  |
| 96     | STX (Direct Page, Y)                   | Store X Register to Direct Page, Y                   |
| 97     | STA (Indirect Long, Y)                 | Store Accumulator to Indirect Long, Y                |
| 98     | TYA                                    | Transfer Y Register to Accumulator                   |
| 99     | STA (Absolute, Y)                      | Store Accumulator to Absolute, Y                     |
| 9D     | STA (Absolute, X)                      | Store Accumulator to Absolute, X                     |
| 9E     | STZ (Absolute, X)                      | Store Zero to Absolute, X                            |
| A0     | LDY (Immediate)                        | Load Y Register with Immediate                       |
| A1     | LDA (Direct Page, X)                   | Load Accumulator from Direct Page, X                 |
| A3     | LDA (Stack Relative)                   | Load Accumulator from Stack Relative                 |
| A4     | LDY (Direct Page)                      | Load Y Register from Direct Page                     |
| A6     | LDX (Direct Page)                      | Load X Register from Direct Page                     |
| A7     | LDA (Indirect Long)                    | Load Accumulator from Indirect Long                  |
| A8     | TAY                                    | Transfer Accumulator to Y Register                   |
| AB     | PLB                                    | Pull Data Bank Register                              |
| AC     | LDY (Absolute)                         | Load Y Register from Absolute                        |
| AE     | LDX (Absolute)                         | Load X Register from Absolute                        |
| AF     | LDA (Absolute Long)                    | Load Accumulator from Absolute Long                  |
| B0     | BCS                                    | Branch if Carry Set                                  |
| B1     | LDA (Indirect, Y)                      | Load Accumulator from Indirect, Y                    |
| B2     | LDA (Indirect)                         | Load Accumulator from Indirect                       |
| B3     | LDA (Stack Relative, Y)                | Load Accumulator from Stack Relative, Y              |
| B5     | LDA (Direct Page, X)                   | Load Accumulator from Direct Page, X                 |
| B6     | LDX (Direct Page, Y)                   | Load X Register from Direct Page, Y                  |
| B7     | LDA (Indirect Long, Y)                 | Load Accumulator from Indirect Long, Y               |
| B8     | CLV                                    | Clear Overflow Flag                                  |
| B9     | LDA (Absolute, Y)                      | Load Accumulator from Absolute, Y                    |
| BA     | TSX                                    | Transfer Stack Pointer to X Register                 |
| BB     | TYX                                    | Transfer Y Register to X Register                    |
| BC     | LDY (Absolute, X)                      | Load Y Register from Absolute, X                     |
| BD     | LDA (Absolute, X)                      | Load Accumulator from Absolute, X                    |
| BE     | LDX (Absolute, Y)                      | Load X Register from Absolute, Y                     |
| BF     | LDA (Absolute Long, X)                 | Load Accumulator from Absolute Long, X               |
| C1     | CMP (Direct Page, X)                   | Compare Accumulator with Direct Page, X              |
| C3     | CMP (Stack Relative)                   | Compare Accumulator with Stack Relative              |
| C4     | CPY (Direct Page)                      | Compare Y Register with Direct Page                  |
| C5     | CMP (Direct Page)                      | Compare Accumulator with Direct Page                 |
| C6     | DEC (Direct Page)                      | Decrement Memory (Direct Page)                       |
| C7     | CMP (Indirect Long)                    | Compare Accumulator with Indirect Long               |
| C8     | INY                                    | Increment Y Register                                 |
| CA     | DEX                                    | Decrement X Register                                 |
| CB     | WAI                                    | Wait for Interrupt                                   |
| CC     | CPY (Absolute)                         | Compare Y Register with Absolute                     |
| CD     | CMP (Absolute)                         | Compare Accumulator with Absolute                    |
| CE     | DEC (Absolute)                         | Decrement Memory (Absolute)                          |
| CF     | CMP (Absolute Long)                    | Compare Accumulator with Absolute Long               |
| D1     | CMP (Indirect, Y)                      | Compare Accumulator with Indirect, Y                 |
| D2     | CMP (Indirect)                         | Compare Accumulator with Indirect                    |
| D3     | CMP (Stack Relative, Y)                | Compare Accumulator with Stack Relative, Y           |
| D4     | PEI                                    | Push Effective Indirect Address                      |
| D5     | CMP (Direct Page, X)                   | Compare Accumulator with Direct Page, X              |
| D6     | DEC (Direct Page, X)                   | Decrement Memory (Direct Page, X)                    |
| D7     | CMP (Indirect Long, Y)                 | Compare Accumulator with Indirect Long, Y            |
| D8     | CLD                                    | Clear Decimal Mode                                   |
| D9     | CMP (Absolute, Y)                      | Compare Accumulator with Absolute, Y                 |
| DA     | PHX                                    | Push X Register                                      |
| DB     | STP                                    | Stop the Clock                                       |
| DC     | JMP (Absolute Indirect Long)           | Jump to Address (Absolute Indirect Long)             |
| DD     | CMP (Absolute, X)                      | Compare Accumulator with Absolute, X                 |
| DE     | DEC (Absolute, X)                      | Decrement Memory (Absolute, X)                       |
| DF     | CMP (Absolute Long, X)                 | Compare Accumulator with Absolute Long, X            |
| E1     | SBC (Direct Page, X)                   | Subtract with Carry from Direct Page, X              |
| E3     | SBC (Stack Relative)                   | Subtract with Carry from Stack Relative              |
| E4     | CPX (Direct Page)                      | Compare X Register with Direct Page                  |
| E5     | SBC (Direct Page)                      | Subtract with Carry from Direct Page                 |
| E6     | INC (Direct Page)                      | Increment Memory (Direct Page)                       |
| E7     | SBC (Indirect Long)                    | Subtract with Carry from Indirect Long               |
| E8     | INX                                    | Increment X Register                                 |
| E9     | SBC (Immediate)                        | Subtract with Carry with Immediate                   |
| EB     | XBA                                    | Exchange B and A Registers                           |
| EC     | CPX (Absolute)                         | Compare X Register with Absolute                     |
| ED     | SBC (Absolute)                         | Subtract with Carry from Absolute                    |
| EE     | INC (Absolute)                         | Increment Memory (Absolute)                          |
| EF     | SBC (Absolute Long)                    | Subtract with Carry from Absolute Long               |
| F1     | SBC (Indirect, Y)                      | Subtract with Carry from Indirect, Y                 |
| F2     | SBC (Indirect)                         | Subtract with Carry from Indirect                    |
| F3     | SBC (Stack Relative, Y)                | Subtract with Carry from Stack Relative, Y           |
| F4     | PEA                                    | Push Effective Absolute Address                      |
| F5     | SBC (Direct Page, X)                   | Subtract with Carry from Direct Page, X              |
| F6     | INC (Direct Page, X)                   | Increment Memory (Direct Page, X)                    |
| F7     | SBC (Indirect Long, Y)                 | Subtract with Carry from Indirect Long, Y            |
| F8     | SED                                    | Set Decimal Mode                                     |
| F9     | SBC (Absolute, Y)                      | Subtract with Carry from Absolute, Y                 |
| FB     | PLX                                    | Pull X Register                                      |
| FD     | SBC (Absolute, X)                      | Subtract with Carry from Absolute, X                 |
| FE     | INC (Absolute, X)                      | Increment Memory (Absolute, X)                       |
| FF     | SBC (Absolute Long, X)                 | Subtract with Carry from Absolute Long, X            |
