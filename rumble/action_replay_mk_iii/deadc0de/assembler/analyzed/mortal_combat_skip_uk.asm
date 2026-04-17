; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x00FEE5
;   - Length: 14 DWORDs (56 bytes)
;
00/FEE5:    08          php             ; Push processor status onto stack.
00/FEE6:    C230        rep #$30        ; Clear processor status bits present in 0b00110000.
00/FEE8:    48          pha             ; Push accumulator onto stack.
00/FEE9:    5A          phy             ; Push Y register onto stack.
00/FEEA:    DA          phx             ; Push X register onto stack.
00/FEEB:    E230        sep #$30        ; Set processor status bits present in 0b00110000.
00/FEED:    AF194200    lda $004219     ; Load 0x004219 into accumulator.
00/FEF1:    2920        and #$20        ; Bitwise AND accumulator with constant '32'.
00/FEF3:    F01D        beq $FF12       ; Branch to 00/FF12 if equal.
00/FEF5:    AF42007E    lda $7E0042     ; Load Work RAM (WRAM) (at 0x7E0042) into accumulator.
00/FEF9:    C90B        cmp #$0B        ; Compare accumulator to constant '11'.
00/FEFB:    F015        beq $FF12       ; Branch to 00/FF12 if equal.
00/FEFD:    A9FD        lda #$FD        ; Load constant '253' into accumulator.
00/FEFF:    8F71047E    sta $7E0471     ; Store accumulator at Work RAM (WRAM) (at 0x7E0471).
00/FF03:    A9FE        lda #$FE        ; Load constant '254' into accumulator.
00/FF05:    8F72047E    sta $7E0472     ; Store accumulator at Work RAM (WRAM) (at 0x7E0472).
00/FF09:    AF42007E    lda $7E0042     ; Load Work RAM (WRAM) (at 0x7E0042) into accumulator.
00/FF0D:    1A          inc A           ; Increment value at A.
00/FF0E:    8F42007E    sta $7E0042     ; Store accumulator at Work RAM (WRAM) (at 0x7E0042).
00/FF12:    C230        rep #$30        ; Clear processor status bits present in 0b00110000.
00/FF14:    FA          plx             ; Pull X register from stack.
00/FF15:    7A          ply             ; Pull Y register from stack.
00/FF16:    68          pla             ; Pull accumulator from stack.
00/FF17:    28          plp             ; Pull processor status from stack.
00/FF18:    5CEAFE80    jmp $80FEEA     ; Jump to 80/FEEA.
00/FF1C:    00          brk             ; Force break.
