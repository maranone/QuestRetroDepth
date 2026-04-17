; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x80E6D0
;   - Length: 8 DWORDs (32 bytes)
;
php
rep #$30
ldx #$0000
lda #$0016
sta $80146A,X
inx
inx
cpx #$0008
bne $E6D6
plp
php
sep #$20
stz $2121
jmp $21219C
80/E6EF:	5C
