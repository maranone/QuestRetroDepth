; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0xC0025D
;   - Length: 7 DWORDs (28 bytes)
;
C0/025D:    ADA800      lda $00A8       ; Load 0x00A8 into accumulator.
C0/0260:    C90020      cmp #$2000      ; Compare accumulator to constant '8192'.
C0/0263:    D00A        bne $026F       ; Branch to C0/026F if not equal.
C0/0265:    E230        sep #$30        ; Set processor status bits present in 0b00110000.
C0/0267:    A9FF        lda #$FF        ; Load constant '255' into accumulator.
C0/0269:    8F71077E    sta $7E0771     ; Store accumulator at Work RAM (WRAM) (at 0x7E0771).
C0/026D:    C230        rep #$30        ; Clear processor status bits present in 0b00110000.
C0/026F:    AD1A42      lda $421A       ; Load JOY2L - Joypad 2 Data (Lower) into accumulator.
C0/0272:    AA          tax             ; Transfer accumulator to X register.
C0/0273:    5C6002C0    jmp $C00260     ; Jump to C0/0260.
C0/0277:    00          brk             ; Force break.
C0/0278:    00          brk             ; Force break.
