; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x00FEE5
;   - Length: 14 DWORDs (56 bytes)
;
php
rep #$30
pha
phy
phx
sep #$30
lda $004219
and #$20
beq $FF12
lda $7E0042
cmp #$0B
beq $FF12
lda #$FD
sta $7E0471
lda #$FE
sta $7E0472
lda $7E0042
inc A
sta $7E0042
rep #$30
plx
ply
pla
plp
jmp $80FEEA
brk
