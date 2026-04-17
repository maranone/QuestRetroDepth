; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x808279
;   - Length: Dynamic (until EOF)
;
sta $004200
lda #$8F03
ror $00
ror $EAEA,X
lda #$8F06
jmp $EA7E00
nop
lda #$8F03
ror A
brk
ror $EAEA,X
lda #$8F19
rts
brk
ror $EAEA,X
lda #$8F03
pla
brk
ror $EAEA,X
lda #$8F07
lsr $7E00,X
nop
nop
lda #$8F03
jmp ($7E00)
nop
nop
lda #$8FB0
dex
trb $7E
nop
nop
jmp $80827C
