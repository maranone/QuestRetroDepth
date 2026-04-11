; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x00FD16
;   - Length: Dynamic (until EOF)
;
pha
php
sep #$30
lda #$0D
sta $7E8957
nop
nop
lda #$63
sta $7E8955
nop
nop
lda #$04
sta $7E8953
nop
nop
lda #$03
sta $7E895F
nop
nop
plp
pla
lda $004211
jmp $C0FD15
brk
brk
