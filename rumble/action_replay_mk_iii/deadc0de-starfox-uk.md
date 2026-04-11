# STARFOX/STARWING DEADCODES (UK VERSION)

> (ACTION REPLAY II ONLY)

[Dead Code Overview](preservaction-deadc0de.md)


## Downloads

- [Hexdump](deadc0de/hexdumps/starfox_uk.hex)
- [Binary](deadc0de/binaries/starfox_uk.bin)
- [Assambler (Plain)](deadc0de/assembler/plain/starfox_uk.asm)
- [Assambler (Annotated)](deadc0de/assembler/analyzed/starfox_uk.asm)

## Infinite/Unlimited Energy/Bombs

> This is for infinite energy and unlimited Nova bombs

### Input

```hex
DEADCODE
02FC7A06
08E220A9
288D9603
A9058DAD
15286468
64695C7E
FC020000
```

### Analysis

```assembler
02FC    	cop $FC          ; Coprocessor Enable (cop) - Triggers a coprocessor interrupt with vector offset $FC, used for specialized tasks or operations.
7A      	ply              ; Pull Y Register (ply) - Pulls (retrieves) a 16-bit value from the stack into the Y register.
0608    	asl $08          ; Arithmetic Shift Left (asl) - Shifts the bits of the byte at memory location $08 to the left by one bit position.
E220    	sep #$20         ; Set Processor Status Bits (sep) - Sets the specified processor status bits, changing the accumulator to 8-bit mode by setting bit 5.
A928    	lda #$28         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $28.
8D9603  	sta $0396        ; Store Accumulator (sta) - Stores the value of the accumulator at the memory location $0396.
A905    	lda #$05         ; Load Accumulator with Immediate (lda) - Loads the accumulator with the immediate value $05.
8DAD15  	sta $15AD        ; Store Accumulator (sta) - Stores the value of the accumulator at the memory location $15AD.
28      	plp              ; Pull Processor Status (plp) - Restores the processor status register from the stack.
6468    	stz $68          ; Store Zero (stz) - Stores zero into the memory location at $68.
6469    	stz $69          ; Store Zero (stz) - Stores zero into the memory location at $69.
5C7EFC02	jmp $02FC7E      ; Jump Long (jmp) - Unconditionally jump to the long address $02FC7E.
00      	brk              ; Break (brk) - Initiates a software interrupt, similar to a breakpoint for debugging.
00      	brk              ; Break (brk) - Another breakpoint, possibly for redundancy or alignment.
```
