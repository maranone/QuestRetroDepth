# WOLFENSTEIN 3D DEADCODES

> (ACTION REPLAY II ONLY)

[Dead Code Overview](preservaction-deadc0de.md)


## Downloads

- [Hexdump](deadc0de/hexdumps/wolfenstein_3d.hex)
- [Binary](deadc0de/binaries/wolfenstein_3d.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/wolfenstein_3d.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/wolfenstein_3d.asm)

## Unlimited Shots, Attempts, Keys

### Input

```hex
DEADCODE
00FD1500
4808E230
XXXXXXXX
XXXXXXXX
XXXXXXXX
XXXXXXXX
2868AF11
42005C15
FDC00000
```

> XXXXXXXX can be replaced by one or more of the following codes

**Unlimited shots:**
```hex
A90D8F57
897EEAEA
```

**Unlimited energy:**
```hex
A9638F55
897EEAEA
```

**Infinite lives:**
```hex
A9048F53
897EEAEA
```

**Always have both keys:**
```hex
A9038F5F
897EEAEA
```

### Analysis

```assembler
00      	brk               ; Break (brk) - Initiates a software interrupt, often used as a breakpoint for debugging.
FD1500  	sbc $0015,X       ; Subtract with Borrow from Memory (sbc) - Subtracts the value from memory location $0015 plus the X register from the accumulator, considering the carry flag for borrow.
48      	pha               ; Push Accumulator (pha) - Pushes the current value of the accumulator onto the stack.
08      	php               ; Push Processor Status (php) - Pushes the processor status register onto the stack.
E230    	sep #$30          ; Set Processor Status Bits (sep) - Sets the specified processor status bits, changing accumulator and index registers to 8-bit mode by setting bits 4 and 5.
A90D    	lda #$0D          ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $0D.
8F57897E	sta $7E8957       ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E8957.
EA      	nop               ; No Operation (nop) - Does nothing, often used for timing or alignment purposes.
EA      	nop               ; No Operation (nop) - Another NOP for timing or alignment.
A963    	lda #$63          ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $63.
8F55897E	sta $7E8955       ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E8955.
EA      	nop               ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop               ; No Operation (nop) - Another NOP for timing or alignment.
A904    	lda #$04          ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $04.
8F53897E	sta $7E8953       ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E8953.
EA      	nop               ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop               ; No Operation (nop) - Another NOP for timing or alignment.
A903    	lda #$03          ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $03.
8F5F897E	sta $7E895F       ; Store Accumulator (sta) - Stores the value of the accumulator at the long memory address $7E895F.
EA      	nop               ; No Operation (nop) - Another NOP for timing or alignment.
EA      	nop               ; No Operation (nop) - Another NOP for timing or alignment.
28      	plp               ; Pull Processor Status (plp) - Restores the processor status register from the stack.
68      	pla               ; Pull Accumulator (pla) - Pulls (retrieves) the last pushed accumulator value from the stack.
AF114200	lda $004211       ; Load Accumulator from Long Address (lda) - Loads the accumulator with the value from the long address $004211.
5C15FDC0	jmp $C0FD15       ; Jump Long (jmp) - Unconditionally jumps to the long address $C0FD15.
00      	brk               ; Break (brk) - Another software interrupt, often used for debugging.
00      	brk               ; Break (brk) - Another breakpoint, possibly for redundancy or alignment.
```
