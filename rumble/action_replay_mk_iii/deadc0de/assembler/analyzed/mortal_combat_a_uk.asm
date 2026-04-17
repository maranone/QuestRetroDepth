; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x80E69C
;   - Length: 8 DWORDs (32 bytes)
;
80/E69C:    08          php             ; Push processor status onto stack.
80/E69D:    C230        rep #$30        ; Clear processor status bits present in 0b00110000.
80/E69F:    A20000      ldx #$0000      ; Load constant '0' into X register.
80/E6A2:    A91600      lda #$0016      ; Load constant '22' into accumulator.
80/E6A5:    9F301480    sta $801430,X   ; Store accumulator at 80/1430.
80/E6A9:    E8          inx             ; Increment X register.
80/E6AA:    E8          inx             ; Increment X register.
80/E6AB:    E00800      cpx #$0008      ; Compare X register to constant '8'.
80/E6AE:    D0F2        bne $E6A2       ; Branch to 80/E6A2 if not equal.
80/E6B0:    28          plp             ; Pull processor status from stack.
80/E6B1:    08          php             ; Push processor status onto stack.
80/E6B2:    E220        sep #$20        ; Set processor status bits present in 0b00100000.
80/E6B4:    9C2121      stz $2121       ; Store zero at CGADD - CGRAM Address.
80/E6B7:    5CA1E680    jmp $80E6A1     ; Jump to 80/E6A1.
80/E6BB:    00          brk             ; Force break.
