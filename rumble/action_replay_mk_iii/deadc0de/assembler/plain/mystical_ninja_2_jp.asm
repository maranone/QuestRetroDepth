; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x8087B5
;   - Length: Dynamic (until EOF)
;
sep #$30
nop
nop
lda #$02
sta $7E0460
nop
nop
lda #$06
sta $7E0446
nop
nop
lda #$09
sta $7E0475
nop
nop
lda #$09
sta $7E0498
nop
nop
lda #$99
sta $7E00AE
nop
nop
lda #$00
ror $EAEA,X
rep #$10
jmp $8087B8
brk
brk
