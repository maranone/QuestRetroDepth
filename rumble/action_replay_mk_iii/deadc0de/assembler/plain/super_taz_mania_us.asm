; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x9A8988
;   - Length: Dynamic (until EOF)
;
php
sep #$20
nop
lda #$10
sta $7E0096
nop
nop
lda #$01
sta $7E702E
nop
nop
lda #$30
sta $7E0098
nop
nop
lda #$05
sta $7E7020
nop
nop
lda #$06
sta $7E008A
nop
nop
lda #$02
sta $7E7022
nop
nop
lda #$00
sta $7E7024
nop
nop
lda #$02
sta $7E702A
nop
nop
plp
lda $B2
and $AA
jmp $9A898B
brk
brk
brk
