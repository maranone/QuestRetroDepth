; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x00FD16
;   - Length: Dynamic (until EOF)
;
00/FD16:    48          pha             ; Push accumulator onto stack.
00/FD17:    08          php             ; Push processor status onto stack.
00/FD18:    E230        sep #$30        ; Set processor status bits present in 0b00110000.
00/FD1A:    A90D        lda #$0D        ; Load constant '13' into accumulator.
00/FD1C:    8F57897E    sta $7E8957     ; Store accumulator at Work RAM (WRAM) (at 0x7E8957).
00/FD20:    EA          nop             ; No operation.
00/FD21:    EA          nop             ; No operation.
00/FD22:    A963        lda #$63        ; Load constant '99' into accumulator.
00/FD24:    8F55897E    sta $7E8955     ; Store accumulator at Work RAM (WRAM) (at 0x7E8955).
00/FD28:    EA          nop             ; No operation.
00/FD29:    EA          nop             ; No operation.
00/FD2A:    A904        lda #$04        ; Load constant '4' into accumulator.
00/FD2C:    8F53897E    sta $7E8953     ; Store accumulator at Work RAM (WRAM) (at 0x7E8953).
00/FD30:    EA          nop             ; No operation.
00/FD31:    EA          nop             ; No operation.
00/FD32:    A903        lda #$03        ; Load constant '3' into accumulator.
00/FD34:    8F5F897E    sta $7E895F     ; Store accumulator at Work RAM (WRAM) (at 0x7E895F).
00/FD38:    EA          nop             ; No operation.
00/FD39:    EA          nop             ; No operation.
00/FD3A:    28          plp             ; Pull processor status from stack.
00/FD3B:    68          pla             ; Pull accumulator from stack.
00/FD3C:    AF114200    lda $004211     ; Load 0x004211 into accumulator.
00/FD40:    5C15FDC0    jmp $C0FD15     ; Jump to C0/FD15.
00/FD44:    00          brk             ; Force break.
00/FD45:    00          brk             ; Force break.
