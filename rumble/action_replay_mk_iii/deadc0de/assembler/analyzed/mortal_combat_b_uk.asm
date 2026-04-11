; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x80E6D0
;   - Length: 8 DWORDs (32 bytes)
;
80/E6D0:    08          php             ; Push processor status onto stack.
80/E6D1:    C230        rep #$30        ; Clear processor status bits present in 0b00110000.
80/E6D3:    A20000      ldx #$0000      ; Load constant '0' into X register.
80/E6D6:    A91600      lda #$0016      ; Load constant '22' into accumulator.
80/E6D9:    9F6A1480    sta $80146A,X   ; Store accumulator at 80/146A.
80/E6DD:    E8          inx             ; Increment X register.
80/E6DE:    E8          inx             ; Increment X register.
80/E6DF:    E00800      cpx #$0008      ; Compare X register to constant '8'.
80/E6E2:    D0F2        bne $E6D6       ; Branch to 80/E6D6 if not equal.
80/E6E4:    28          plp             ; Pull processor status from stack.
80/E6E5:    08          php             ; Push processor status onto stack.
80/E6E6:    E220        sep #$20        ; Set processor status bits present in 0b00100000.
80/E6E8:    9C2121      stz $2121       ; Store zero at CGADD - CGRAM Address.
80/E6EB:    5C9C2121    jmp $21219C     ; Jump to 21/219C.
80/E6EF:	5C
