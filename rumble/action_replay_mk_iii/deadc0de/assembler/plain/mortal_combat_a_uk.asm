; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x80E69C
;   - Length: 8 DWORDs (32 bytes)
;
php
rep #$30
ldx #$0000
lda #$0016
sta $801430,X
inx
inx
cpx #$0008
bne $E6A2
plp
php
sep #$20
stz $2121
jmp $80E6A1
brk
