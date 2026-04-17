; Action Replay DEADC0DE Cheat
;   - Address-Hook: 0x02FC7B
;   - Length: 6 DWORDs (24 bytes)
;
02/FC7B:    08          php             ; Push processor status onto stack.
02/FC7C:    E220        sep #$20        ; Set processor status bits present in 0b00100000.
02/FC7E:    A928        lda #$28        ; Load constant '40' into accumulator.
02/FC80:    8D9603      sta $0396       ; Store accumulator at 02/0396.
02/FC83:    A905        lda #$05        ; Load constant '5' into accumulator.
02/FC85:    8DAD15      sta $15AD       ; Store accumulator at 02/15AD.
02/FC88:    28          plp             ; Pull processor status from stack.
02/FC89:    6468        stz $68         ; Store zero at 0x68.
02/FC8B:    6469        stz $69         ; Store zero at 0x69.
02/FC8D:    5C7EFC02    jmp $02FC7E     ; Jump to 02/FC7E.
02/FC91:    00          brk             ; Force break.
02/FC92:    00          brk             ; Force break.
