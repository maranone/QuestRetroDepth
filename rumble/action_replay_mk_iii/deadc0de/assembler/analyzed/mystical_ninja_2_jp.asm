; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x8087B5
;   - Length: Dynamic (until EOF)
;
80/87B5:    E230        sep #$30        ; Set processor status bits present in 0b00110000.
80/87B7:    EA          nop             ; No operation.
80/87B8:    EA          nop             ; No operation.
80/87B9:    A902        lda #$02        ; Load constant '2' into accumulator.
80/87BB:    8F60047E    sta $7E0460     ; Store accumulator at Work RAM (WRAM) (at 0x7E0460).
80/87BF:    EA          nop             ; No operation.
80/87C0:    EA          nop             ; No operation.
80/87C1:    A906        lda #$06        ; Load constant '6' into accumulator.
80/87C3:    8F46047E    sta $7E0446     ; Store accumulator at Work RAM (WRAM) (at 0x7E0446).
80/87C7:    EA          nop             ; No operation.
80/87C8:    EA          nop             ; No operation.
80/87C9:    A909        lda #$09        ; Load constant '9' into accumulator.
80/87CB:    8F75047E    sta $7E0475     ; Store accumulator at Work RAM (WRAM) (at 0x7E0475).
80/87CF:    EA          nop             ; No operation.
80/87D0:    EA          nop             ; No operation.
80/87D1:    A909        lda #$09        ; Load constant '9' into accumulator.
80/87D3:    8F98047E    sta $7E0498     ; Store accumulator at Work RAM (WRAM) (at 0x7E0498).
80/87D7:    EA          nop             ; No operation.
80/87D8:    EA          nop             ; No operation.
80/87D9:    A999        lda #$99        ; Load constant '153' into accumulator.
80/87DB:    8FAE007E    sta $7E00AE     ; Store accumulator at Work RAM (WRAM) (at 0x7E00AE).
80/87DF:    EA          nop             ; No operation.
80/87E0:    EA          nop             ; No operation.
80/87E1:    A900        lda #$00        ; Load constant '0' into accumulator.
80/87E3:    7EEAEA      ror $EAEA,X     ; Rotate right on 80/EAEA.
80/87E6:    C210        rep #$10        ; Clear processor status bits present in 0b00010000.
80/87E8:    5CB88780    jmp $8087B8     ; Jump to 80/87B8.
80/87EC:    00          brk             ; Force break.
80/87ED:    00          brk             ; Force break.
