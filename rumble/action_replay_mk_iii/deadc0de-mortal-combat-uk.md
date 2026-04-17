# MORTAL COMBAT DEADCODES (UK VERSION)

> (ACTION REPLAY II ONLY)

[Dead Code Overview](preservaction-deadc0de.md)


## Downloads

**Version A***

- [Hexdump](deadc0de/hexdumps/mortal_combat_a_uk.hex)
- [Binary](deadc0de/binaries/mortal_combat_a_uk.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/mortal_combat_a_uk.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/mortal_combat_a_uk.asm)

**Version B***

- [Hexdump](deadc0de/hexdumps/mortal_combat_b_uk.hex)
- [Binary](deadc0de/binaries/mortal_combat_b_uk.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/mortal_combat_b_uk.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/mortal_combat_b_uk.asm)

**Skip Level***

- [Hexdump](deadc0de/hexdumps/mortal_combat_skip_uk.hex)
- [Binary](deadc0de/binaries/mortal_combat_skip_uk.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/mortal_combat_skip_uk.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/mortal_combat_skip_uk.asm)

## Full Blood During Combat (A)

### Input

```hex
DEADCODE
80E69B08
08C230A2
0000A916
009F3014
80E8E8E0
0800D0F2
2808E220
9C21215C
A1E68000
```

### Analysis

```assembler
80E6    	bra $7FE8        ; Branch Always (bra) - Unconditional jump to the address $7FE8. This will skip a portion of the code if executed.
9B      	txy              ; Transfer X to Y (txy) - Copies the value in the X register to the Y register.
08      	php              ; Push Processor Status (php) - Pushes the processor status register onto the stack.
08      	php              ; Push Processor Status (php) - Again pushes the processor status register onto the stack (possibly redundant).
C230    	rep #$30         ; Reset Processor Status Bits (rep) - Clears specified bits (in this case, bit 4 and 5) in the status register, setting accumulator and index registers to 16-bit mode.
A20000  	ldx #$0000       ; Load X Register with Immediate (ldx) - Loads the X register with the immediate value $0000.
A91600  	lda #$0016       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $0016.
9F301480	sta $801430,X    ; Store Accumulator (sta) - Stores the value of the accumulator into the address calculated by $801430 plus the X register. This uses the long indexed addressing mode.
E8      	inx              ; Increment X Register (inx) - Increases the value of the X register by one.
E8      	inx              ; Increment X Register (inx) - Performs another increment, total increment is by 2 at this point.
E00800  	cpx #$0008       ; Compare X Register with Immediate (cpx) - Compares the X register with the immediate value $0008.
D0F2    	bne $800A        ; Branch if Not Equal (bne) - Jumps back to address $800A if the result of the previous comparison is not zero (X is not 8), creating a loop.
28      	plp              ; Pull Processor Status (plp) - Pulls (retrieves) the processor status register from the stack, restoring it.
08      	php              ; Push Processor Status (php) - Pushes the processor status register onto the stack again.
E220    	sep #$20         ; Set Processor Status Bits (sep) - Sets specified bits (in this case, bit 5) in the status register, making the accumulator 8-bit mode.
9C2121  	stz $2121        ; Store Zero (stz) - Stores zero into the memory location $2121.
5CA1E680	jmp $80E6A1      ; Jump Long (jmp) - Unconditionally jumps to the long address $80E6A1.
00      	brk              ; Break (brk) - Forces a software interrupt, similar to a program breakpoint.
```

## Full Blood During Combat (B)

> Use this code if the previous code did not work

### Input

```hex
80E6CF08
08C230A2
0000A916
009F6A14
80E8E8E0
0800D0F2
2808E220
9C21215C
9C21215C
```

### Analysis

```assembler
80E6    	bra $7FE8        ; Branch Always (bra) - Unconditional jump to the address $7FE8. This causes an immediate jump in the program flow.
CF0808C2	cmp $C20808      ; Compare Accumulator with Memory (cmp) - Compares the accumulator with the contents at the long address $C20808.
30A2    	bmi $7FAA        ; Branch if Minus (bmi) - Branches to the address $7FAA if the result of the previous comparison sets the negative flag.
00      	brk              ; Break (brk) - Initiates a software interrupt, functioning similarly to a breakpoint in debugging.
00      	brk              ; Break (brk) - Another breakpoint, possibly for redundancy or alignment purposes.
A91600  	lda #$0016       ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $0016.
9F6A1480	sta $80146A,X    ; Store Accumulator (sta) - Stores the value of the accumulator at the memory address calculated as $80146A plus the X register.
E8      	inx              ; Increment X Register (inx) - Increments the value in the X register by one.
E8      	inx              ; Increment X Register (inx) - Another increment, total increment by 2 at this point.
E00800  	cpx #$0008       ; Compare X Register with Immediate (cpx) - Compares the value of the X register with the immediate value $0008.
D0F2    	bne $800A        ; Branch if Not Equal (bne) - Jumps back to $800A if the X register is not equal to $0008, forming a loop.
28      	plp              ; Pull Processor Status (plp) - Restores the processor status register from the stack.
08      	php              ; Push Processor Status (php) - Pushes the current processor status register onto the stack.
E220    	sep #$20         ; Set Processor Status Bits (sep) - Sets the specified processor status bits, changing accumulator to 8-bit mode.
9C2121  	stz $2121        ; Store Zero (stz) - Stores zero into memory location $2121.
5C9C2121	jmp $21219C      ; Jump Long (jmp) - Unconditional jump to the long address $21219C.
00/8023:	5C              ; Data at memory location $00/8023. The value 5C typically corresponds to a jump instruction, needing further context.
```

## Level Skip

> Level skip - when select is pressed your player moves to the next stage.

### Input

```hex
DEADCODE
OOFEE40E
08C23048
5ADAE230
AF194200
2920F01D
AF42007E
C90BF015
A9FD8F71
047EA9FE
8F72047E
AF42007E
1A8F4200
7EC230FA
7A68285C
EAFE8000
```

### Analysis

```assembler
FEE40E  	inc $0EE4,X    ; Increment the value at memory location ($0EE4 + X). This adds 1 to the byte stored in that location.
08      	php             ; Push the Processor Status Register onto the stack. This saves the current processor flags for later restoration.
C230    	rep #$30        ; Reset processor status bits 4 and 5 in the Status Register. This sets the accumulator and index registers to 16-bit mode.
48      	pha             ; Push the accumulator onto the stack. This saves the current value of the accumulator for later use.
5A      	phy             ; Push the Y register onto the stack. This saves the current value of the Y register.
DA      	phx             ; Push the X register onto the stack. This saves the current value of the X register.
E230    	sep #$30        ; Set processor status bits 4 and 5. This sets the accumulator and index registers back to 8-bit mode.
AF194200	lda $004219    ; Load the accumulator with the value from memory location $004219. This fetches a specific byte from memory.
2920    	and #$20        ; Perform a bitwise AND between the accumulator and hexadecimal 20. This isolates a specific bit of interest.
F01D    	beq $8030       ; Branch to memory address $8030 if the zero flag is set, meaning the result of the AND was zero.
AF42007E	lda $7E0042    ; Load the accumulator with the value from memory location $7E0042. This accesses another specific byte from memory.
C90B    	cmp #$0B        ; Compare the accumulator with hexadecimal 0B. Sets flags based on the comparison.
F015    	beq $8030       ; Branch to memory address $8030 if the zero flag is set, meaning the value was 0B.
A9FD    	lda #$FD        ; Load the accumulator with the immediate value FD. Puts this exact byte into the accumulator.
8F71047E	sta $7E0471    ; Store the accumulator's value into memory location $7E0471. This saves the FD byte to this location.
A9FE    	lda #$FE        ; Load the accumulator with the immediate value FE. Updates the accumulator with this specific value.
8F72047E	sta $7E0472    ; Store the accumulator's value into memory location $7E0472. Writes FE byte into this specific memory location.
AF42007E	lda $7E0042    ; Load the accumulator with the value from memory location $7E0042. Fetches the current byte from this location.
1A      	inc A           ; Increment the accumulator. Adds one to the current value in the accumulator.
8F42007E	sta $7E0042    ; Store the accumulator's value into memory location $7E0042. Saves the updated accumulator value back to this location.
C230    	rep #$30        ; Reset processor status bits 4 and 5. Switches the processor back to 16-bit mode for the operands.
FA      	plx             ; Pull the X register from the stack. Restores the previous value of the X register.
7A      	ply             ; Pull the Y register from the stack. Restores the previous value of the Y register.
68      	pla             ; Pull the accumulator from the stack. Restores the previously saved accumulator value.
28      	plp             ; Pull the Processor Status Register from the stack. Restores the processor's flags to their saved state.
5CEAFE80	jmp $80FEEA    ; Jump to address $80FEEA. Transfers program control to the specified location.
00      	brk             ; Force a break. Typically used to trigger a software interrupt for debugging or as a breakpoint.
```
