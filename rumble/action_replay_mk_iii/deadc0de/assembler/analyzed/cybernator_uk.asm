; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x808279
;   - Length: Dynamic (until EOF)
;
80/8279:    8F004200    sta $004200     ; Store accumulator at 00/4200.
80/827D:    A9038F      lda #$8F03      ; Load constant '36611' into accumulator.
80/8280:    6600        ror $00         ; Rotate right on 0x00.
80/8282:    7EEAEA      ror $EAEA,X     ; Rotate right on 80/EAEA.
80/8285:    A9068F      lda #$8F06      ; Load constant '36614' into accumulator.
80/8288:    5C007EEA    jmp $EA7E00     ; Jump to EA/7E00.
80/828C:    EA          nop             ; No operation.
80/828D:    A9038F      lda #$8F03      ; Load constant '36611' into accumulator.
80/8290:    6A          ror A           ; Rotate right on A.
80/8291:    00          brk             ; Force break.
80/8292:    7EEAEA      ror $EAEA,X     ; Rotate right on 80/EAEA.
80/8295:    A9198F      lda #$8F19      ; Load constant '36633' into accumulator.
80/8298:    60          rts             ; Return from subroutine.
80/8299:    00          brk             ; Force break.
80/829A:    7EEAEA      ror $EAEA,X     ; Rotate right on 80/EAEA.
80/829D:    A9038F      lda #$8F03      ; Load constant '36611' into accumulator.
80/82A0:    68          pla             ; Pull accumulator from stack.
80/82A1:    00          brk             ; Force break.
80/82A2:    7EEAEA      ror $EAEA,X     ; Rotate right on 80/EAEA.
80/82A5:    A9078F      lda #$8F07      ; Load constant '36615' into accumulator.
80/82A8:    5E007E      lsr $7E00,X     ; Logical shift right on 80/7E00.
80/82AB:    EA          nop             ; No operation.
80/82AC:    EA          nop             ; No operation.
80/82AD:    A9038F      lda #$8F03      ; Load constant '36611' into accumulator.
80/82B0:    6C007E      jmp ($7E00)     ; Jump to 80/7E00.
80/82B3:    EA          nop             ; No operation.
80/82B4:    EA          nop             ; No operation.
80/82B5:    A9B08F      lda #$8FB0      ; Load constant '36784' into accumulator.
80/82B8:    CA          dex             ; Decrement X register.
80/82B9:    147E        trb $7E         ; Test and reset bits at 0x7E.
80/82BB:    EA          nop             ; No operation.
80/82BC:    EA          nop             ; No operation.
80/82BD:    5C7C8280    jmp $80827C     ; Jump to 80/827C.
