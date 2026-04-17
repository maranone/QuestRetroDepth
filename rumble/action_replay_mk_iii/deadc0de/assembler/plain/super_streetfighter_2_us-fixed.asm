; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0xC0025D
;   - Length: 7 DWORDs (28 bytes)
;
lda $00A8
cmp #$2000
bne $026F
sep #$30
lda #$FF
sta $7E0771
rep #$30
lda $421A
tax
jmp $C00260
brk
brk
