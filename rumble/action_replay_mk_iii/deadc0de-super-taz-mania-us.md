# TAZ-MANIA DEADCODES (US VERSION)

> (ACTION REPLAY II ONLY)

[Dead Code Overview](preservaction-deadc0de.md)


## Downloads

- [Hexdump](deadc0de/hexdumps/super_taz_mania_us.hex)
- [Binary](deadc0de/binaries/super_taz_mania_us.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/super_taz_mania_us.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/super_taz_mania_us.asm)

## Unlimited Time, Attempts, Energy

### Input

```hex
DEADCODE
9A898700
08E220EA
XXXXXXXX
XXXXXXXX
XXXXXXXX
XXXXXXXX
28A5B225
AA5C8B89
9A000000
```

> XXXXXXXX can be replaced by one or more of the following codes

**Unlimited energy:**
```hex
A9108F96
007EEAEA
```

**You only need to collect 1 Kiwi:**
```hex
A9018F2E
707EEAEA
```

**Unlimited time:**
```hex
A9308F98
007EEAEA
```

**Unlimited continues:**
```hex
A9058F20
707EEAEA
```

**Slow motion when running forward:**
```hex
A9068F8A
007EEAEA
```

**Extra Continues every time you gain more points:**
```hex
A9028F22
707EEAEA
```

**The timer runs down at about double the speed:**
```hex
A9008F24
707EEAEA
```

**Timer runs backwards:**
```hex
A9028F2A
707EEAEA
```

### Analysis

```assembler
9A      	txs              ; Transfer X to Stack Pointer (txs) - Copies the value in the X register to the stack pointer.
898700  	bit #$0087       ; Test Bits in Memory with Accumulator (bit) - Performs a logical AND between the accumulator and the immediate value $0087, updating the processor status flags without changing the accumulator.
08      	php              ; Push Processor Status (php) - Pushes the processor status register onto the stack.
E220    	sep #$20         ; Set Processor Status Bits (sep) - Sets the specified processor status bits, changing the accumulator to 8-bit mode by setting bit 5.
EA      	nop              ; No Operation (nop) - Does nothing, often used for timing or alignment purposes.
A910    	lda #$10         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $10.
8F96007E	sta $7E0096      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E0096.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A901    	lda #$01         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $01.
8F2E707E	sta $7E702E      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E702E.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A930    	lda #$30         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $30.
8F98007E	sta $7E0098      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E0098.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A905    	lda #$05         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $05.
8F20707E	sta $7E7020      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E7020.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A906    	lda #$06         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $06.
8F8A007E	sta $7E008A      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E008A.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A902    	lda #$02         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $02.
8F22707E	sta $7E7022      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E7022.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A900    	lda #$00         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $00.
8F24707E	sta $7E7024      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E7024.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A902    	lda #$02         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $02 again.
8F2A707E	sta $7E702A      ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E702A.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
28      	plp              ; Pull Processor Status (plp) - Restores the processor status register from the stack.
A5B2    	lda $B2          ; Load Accumulator from Memory (lda) - Loads the accumulator with the value from the memory location $B2.
25AA    	and $AA          ; Logical AND (and) - Performs a bitwise AND between the accumulator and the value at memory location $AA.
5C8B899A	jmp $9A898B      ; Jump Long (jmp) - Unconditionally jumps to the long address $9A898B.
00      	brk              ; Break (brk) - Forces a software interrupt, often used as a breakpoint for debugging.
00      	brk              ; Break (brk) - Another breakpoint, possibly for redundancy or alignment.
00      	brk              ; Break (brk) - Another breakpoint, possibly for redundancy or alignment.
```
