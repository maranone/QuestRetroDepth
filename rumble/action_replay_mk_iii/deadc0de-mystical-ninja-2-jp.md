# MYSTICAL NINJA 2 DEADCODES (JP VERSION)

> (ACTION REPLAY II ONLY)

[Dead Code Overview](preservaction-deadc0de.md)


## Downloads

- [Hexdump](deadc0de/hexdumps/mystical_ninja_2_jp.hex)
- [Binary](deadc0de/binaries/mystical_ninja_2_jp.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/mystical_ninja_2_jp.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/mystical_ninja_2_jp.asm)

## Maximum/Unlimited Equipment, Level Skip

### Input

```hex
DEADCODE
8087B400
E230EAEA
XXXXXXXX
XXXXXXXX
XXXXXXXX
XXXXXXXX
C2105CB8
87800000
```

> XXXXXXXX can be replaced by one or more of the following codes

**Always have full magic batton:**
```hex
A9028F60
047EEAEA
```

**Unlimited energy:**
```hex
A9068F46
047EEAEA
```

**Unlimited coins:**
```hex
A9098F75
047EEAEA
```

**Infinite lives:**
```hex
A9098F98
047EEAEA
```

**Unlimited time:**
```hex
A9998FAE
007EEAEA
```

**Level select:**

> Warning: Use a blank save game as this will keep adding to the level!

> Hint: Replace XX with desired level

```hex
A9XX8F92
007EEAEA
```

### Analysis

```assembler
8087    	bra $7F89        ; Branch Always (bra) - Unconditional jump to the address $7F89. This causes an immediate forward jump in the code execution.
B400    	ldy $00,X        ; Load Y Register from Memory (ldy) - Loads the Y register with the value from the memory address $00 plus the X register's offset.
E230    	sep #$30         ; Set Processor Status Bits (sep) - Sets the specified processor status bits, changing accumulator and index registers to 8-bit mode by setting bits 4 and 5.
EA      	nop              ; No Operation (nop) - Does nothing, often used for timing or alignment purposes.
EA      	nop              ; No Operation (nop) - Another NOP for timing or alignment.
A902    	lda #$02         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $02.
8F60047E	sta $7E0460      ; Store Accumulator (sta) - Stores the value of the accumulator at the long address $7E0460.
EA      	nop              ; No Operation (nop) - No operation, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP.
A906    	lda #$06         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $06.
8F46047E	sta $7E0446      ; Store Accumulator (sta) - Stores the value of the accumulator at the long address $7E0446.
EA      	nop              ; No Operation (nop) - No operation, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP.
A909    	lda #$09         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $09.
8F75047E	sta $7E0475      ; Store Accumulator (sta) - Stores the value of the accumulator at the long address $7E0475.
EA      	nop              ; No Operation (nop) - No operation, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP.
A909    	lda #$09         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $09 again.
8F98047E	sta $7E0498      ; Store Accumulator (sta) - Stores the value of the accumulator at the long address $7E0498.
EA      	nop              ; No Operation (nop) - No operation, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP.
A999    	lda #$99         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $99.
8FAE007E	sta $7E00AE      ; Store Accumulator (sta) - Stores the value of the accumulator at the long address $7E00AE.
EA      	nop              ; No Operation (nop) - No operation, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Another NOP.
A900    	lda #$00         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $00.
7EEAEA  	ror $EAEA,X      ; Rotate Right (ror) - Rotates the bits of the byte at memory location $EAEA plus X register one bit to the right.
C210    	rep #$10         ; Reset Processor Status Bits (rep) - Clears specified status bits (bit 4) to switch accumulator to 16-bit mode.
5CB88780	jmp $8087B8      ; Jump Long (jmp) - Unconditionally jump to the long address $8087B8.
00      	brk              ; Break (brk) - Forces a software interrupt, often used as a breakpoint for debugging.
00      	brk              ; Break (brk) - Another break, possibly redundant or for alignment.
```
