# CYBERNATOR DEADCODES (UK VERSION)

> (ACTION REPLAY II ONLY)

[Dead Code Overview](preservaction-deadc0de.md)

## Downloads

- [Hexdump](deadc0de/hexdumps/cybernator_uk.hex)
- [Binary](deadc0de/binaries/cybernator_uk.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/cybernator_uk.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/cybernator_uk.asm)

## Maximum/Unlimited Equipment

### Input

```hex
DEADCODE
80827800
8F004200
XXXXXXXX
XXXXXXXX
XXXXXXXX
XXXXXXXX
5C7C8280
```

> XXXXXXXX can be replaced by one or more of the following codes

**Maximum Vulcans:**
```hex
A9038F66
007EEAEA
```

**Unlimited Vulcans:**
```hex
A9068F5C
007EEAEA
```

**Maximum Missiles:**
```hex
A9038F6A
007EEAEA
```

**Unlimited Missiles:**
```hex
A919BF60
007EEAEA
```

**Maximum Laser:**
```hex
A9038F68
007EEAEA
```

**Unlimited Laser:**
```hex
A9078F5E
007EEAEA
```

**Maximum punch:**
```hex
A9038F6C
007EEAEA
```

**Unlimited energy:**
```hex
A9B08FCA
147EEAEA
```

### Analysis

```assembler
8082    	bra $7F84        ; Branch Always (bra) - Unconditional jump to the address $7F84. Effectively, this is a forward jump in the program.
78      	sei              ; Set Interrupt Disable (sei) - Disables all interrupts by setting the Interrupt Disable flag in the status register.
00      	brk              ; Break (brk) - Forces an interrupt by generating a software interrupt request, similar to a breakpoint.
8F004200	sta $004200      ; Store Accumulator (sta) - Stores the value in the accumulator to the memory location $004200.
A9038F  	lda #$8F03       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $8F03.
6600    	ror $00          ; Rotate Right (ror) - Rotates the bits of the byte at memory location $00 to the right by one bit.
7EEAEA  	ror $EAEA,X      ; Rotate Right (ror) - Rotates the bits of the byte at memory location $EAEA plus the value of X register to the right.
A9068F  	lda #$8F06       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $8F06.
5C007EEA	jmp $EA7E00      ; Jump (jmp) - Unconditionally jump to the long address $EA7E00.
EA      	nop              ; No Operation (nop) - Does nothing, used for timing or alignment.
A9038F  	lda #$8F03       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $8F03 again.
6A      	ror A            ; Rotate Right Accumulator (ror) - Rotates the bits of the accumulator to the right by one bit.
00      	brk              ; Break (brk) - Forces an interrupt by generating a software interrupt request.
7EEAEA  	ror $EAEA,X      ; Rotate Right (ror) - Rotates the bits of the byte at memory location $EAEA plus the value of X register to the right.
A919BF  	lda #$BF19       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $BF19.
60      	rts              ; Return from Subroutine (rts) - Returns from subroutine to the address stored on the stack.
00      	brk              ; Break (brk) - Forces an interrupt by generating a software interrupt request.
7EEAEA  	ror $EAEA,X      ; Rotate Right (ror) - Rotates the bits of the byte at memory location $EAEA plus the value of X register to the right.
A9038F  	lda #$8F03       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $8F03.
68      	pla              ; Pull Accumulator (pla) - Pulls (retrieves) a byte from the stack into the accumulator.
00      	brk              ; Break (brk) - Forces an interrupt by generating a software interrupt request.
7EEAEA  	ror $EAEA,X      ; Rotate Right (ror) - Rotates the bits of the byte at memory location $EAEA plus the value of X register to the right.
A9078F  	lda #$8F07       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $8F07.
5E007E  	lsr $7E00,X      ; Logical Shift Right (lsr) - Shifts the bits of the byte at memory location $7E00 plus X register to the right.
EA      	nop              ; No Operation (nop) - Does nothing, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Does nothing, used for timing or alignment.
A9038F  	lda #$8F03       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $8F03.
6C007E  	jmp ($7E00)      ; Jump Indirect (jmp) - Jumps to the address contained in memory location $7E00.
EA      	nop              ; No Operation (nop) - Does nothing, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Does nothing, used for timing or alignment.
A9B08F  	lda #$8FB0       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $8FB0.
CA      	dex              ; Decrement X Register (dex) - Decreases the value of the X register by one.
147E    	trb $7E          ; Test and Reset Bit (trb) - Tests and resets the bits in the memory location $7E that are set in the accumulator.
EA      	nop              ; No Operation (nop) - Does nothing, used for timing or alignment.
EA      	nop              ; No Operation (nop) - Does nothing, used for timing or alignment.
5C7C8280	jmp $80827C      ; Jump Long (jmp) - Jumps unconditionally to the long address $80827C.
```
