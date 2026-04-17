; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x9A8988
;   - Length: Dynamic (until EOF)
;
9A/8988:    08          php             ; Push processor status onto stack.
9A/8989:    E220        sep #$20        ; Set processor status bits present in 0b00100000.
9A/898B:    EA          nop             ; No operation.
9A/898C:    A910        lda #$10        ; Load constant '16' into accumulator.
9A/898E:    8F96007E    sta $7E0096     ; Store accumulator at Work RAM (WRAM) (at 0x7E0096).
9A/8992:    EA          nop             ; No operation.
9A/8993:    EA          nop             ; No operation.
9A/8994:    A901        lda #$01        ; Load constant '1' into accumulator.
9A/8996:    8F2E707E    sta $7E702E     ; Store accumulator at Work RAM (WRAM) (at 0x7E702E).
9A/899A:    EA          nop             ; No operation.
9A/899B:    EA          nop             ; No operation.
9A/899C:    A930        lda #$30        ; Load constant '48' into accumulator.
9A/899E:    8F98007E    sta $7E0098     ; Store accumulator at Work RAM (WRAM) (at 0x7E0098).
9A/89A2:    EA          nop             ; No operation.
9A/89A3:    EA          nop             ; No operation.
9A/89A4:    A905        lda #$05        ; Load constant '5' into accumulator.
9A/89A6:    8F20707E    sta $7E7020     ; Store accumulator at Work RAM (WRAM) (at 0x7E7020).
9A/89AA:    EA          nop             ; No operation.
9A/89AB:    EA          nop             ; No operation.
9A/89AC:    A906        lda #$06        ; Load constant '6' into accumulator.
9A/89AE:    8F8A007E    sta $7E008A     ; Store accumulator at Work RAM (WRAM) (at 0x7E008A).
9A/89B2:    EA          nop             ; No operation.
9A/89B3:    EA          nop             ; No operation.
9A/89B4:    A902        lda #$02        ; Load constant '2' into accumulator.
9A/89B6:    8F22707E    sta $7E7022     ; Store accumulator at Work RAM (WRAM) (at 0x7E7022).
9A/89BA:    EA          nop             ; No operation.
9A/89BB:    EA          nop             ; No operation.
9A/89BC:    A900        lda #$00        ; Load constant '0' into accumulator.
9A/89BE:    8F24707E    sta $7E7024     ; Store accumulator at Work RAM (WRAM) (at 0x7E7024).
9A/89C2:    EA          nop             ; No operation.
9A/89C3:    EA          nop             ; No operation.
9A/89C4:    A902        lda #$02        ; Load constant '2' into accumulator.
9A/89C6:    8F2A707E    sta $7E702A     ; Store accumulator at Work RAM (WRAM) (at 0x7E702A).
9A/89CA:    EA          nop             ; No operation.
9A/89CB:    EA          nop             ; No operation.
9A/89CC:    28          plp             ; Pull processor status from stack.
9A/89CD:    A5B2        lda $B2         ; Load 0xB2 into accumulator.
9A/89CF:    25AA        and $AA         ; Bitwise AND accumulator with 0xAA.
9A/89D1:    5C8B899A    jmp $9A898B     ; Jump to 9A/898B.
9A/89D5:    00          brk             ; Force break.
9A/89D6:    00          brk             ; Force break.
9A/89D7:    00          brk             ; Force break.
