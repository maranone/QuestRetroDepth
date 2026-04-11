; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x02FC7B
;   - Length: 6 DWORDs (24 bytes)
;
php
sep #$20
lda #$28
sta $0396
lda #$05
sta $15AD
plp
stz $68
stz $69
jmp $02FC7E
brk
brk
