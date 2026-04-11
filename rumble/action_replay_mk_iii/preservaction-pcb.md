# Project Preservaction

[Table Of Contents](preservaction-toc.md)

## Printed Circuit Board (PCB)

![PCB](./assets/pcb-chips.png)

**Download:** [PCB-Image without descriptions](./assets/pcb-pure.png)

### Components

- **DATEL TURBO REPLAY (LZ95G72):** The 64 pin LZ95G72 is the heart of the module and is likely a specialized microcontroller or custom IC from DATEL, designed specifically for cheat and region-bypass functions. It retrieves user data stored by the ROM in SRAM, processes cheats, operates the trainer, injects dead codes and controls the module's overall functionality.
  - It looks much like a Zilog Z180 (an enhanced, compatible version of the Z80 CPU with additional instructions and capabilities) 68-Pin PLCC Chip; which can address up to 64kB RAM which maches the 32kB SRAM on the PCB. Additionally it has built-in serial I/O, timers, and a DMA controller.
  - Z80 Chips were reportly used for the Datel Action Replay for the Sega Mega System (3).
  - Before the chips were labeled as custom Datel chips, identical ones (1) were marked as `SHARP` on other pcbs.
- **EPROM (AT27C010-12JC):** The AT27C010 is a one-time programmable memory chip (EPROM) with a storage capacity of 1 Megabit. It is used to store the hard-coded programs and data for the module, such as the SNES ROM (with user interface and pre-installed cheat codes) and custom instructions for the Zilog CPU - the BIOS or firmware.
- **CMOS SRAM (HY62256A):** All PAR versions contain 32 KBytes of SRAM, divided into four 8 KByte segments, which are mapped to even bank numbers as follows: `00/02/04/06:6000h–7FFFh`. This SRAM is used by the system as internal workspace for stack operations, variables, code storage, the NMI handler, the `DEADC0DE` handler, and for maintaining lists used by the code finder (such as possible match results).
- **2 LEDs** (put through holes below the sticker-area and visibible on front of cartridge)
- Cartridge Slots:
  - **Top 62pin cartridge slot:** This slot is used to plugin the SNES game cartridge. It provides access to the game's ROM and any additional hardware features (e.g., enhancement chips) that the game might include.
  - **Rear 62pin cartridge slot:** This cartridge slot is used to grab CIC from other region cartridges to bypass region lock.
- Gates, Switches and Flip-Flops:
  - **CMOS Logic Quad Bilateral Switch (74HC4066):** The 74HC4066 is a logic IC containing four bidirectional switches. It's often used in digital circuits to selectively switch and route signals. In this module, it may be used to switch between different signal paths or data sources depending on the switch position or selected functions.
  - **Dual D Flip-Flop with Set and Reset (HC74A):** The HC74A is a Dual D Flip-Flop IC, used in digital circuits to store and stabilize states. The Flip-Flop can be controlled by set and reset signals, and could be used here to store cheat states (e.g., which cheats are active), keeping them consistent even through game inputs.
  - **Quadruple 2-Input Positive-OR Gates (HC32A):** This chip contains four 2-input OR gates and is a logic component used to combine signals based on OR logic in digital circuits. It might be used to combine logic signals required for activating cheats or controlling certain module settings.

> Credits/Source
    (1) [Jungsi (german)](https://www.jungsi.de/pro-action-replay-2-sega-megadrive/)
    (2) [Fullsnes Nocash SNES Specs](./docs/Fullsnes-Nocash_SNES_Specs.pdf)
    (3) [SMS Power! Development](./docs/PAR-SMS-Power.pdf)

### 3-Way Toggle Switch and Its Functions

The **3-way toggle switch** on the side of the Pro Action Replay MK3 module controls the different operating modes of the module and allows switching between the following positions:

1. **Top Position:**
   - This position activates **parameters**, the **slow-motion function**, and the **trainer**. In this mode, cheats can be applied, and the game can be slowed down using the slow-motion feature.
   - During gameplay, the trainer can be used to locate memory addresses for specific values (e.g., lives or energy) and adjust them.
   - Here, the custom Datel Chip is leading

2. **Middle Position:**
   - This position turns off the Action Replay, placing it in pass-through mode. The game runs unmodified without any cheat or modification functions.
   - This is the default mode when no modifications to the game are desired.
   - Both the custom Datel IC (cheat runtime) and the ROM (SNES runtime) are inactive
   - This position may be required for booting some games which may use WRAM for different purposes during intro & game phases.

3. **Bottom Position:**
   - This position enables the **parameter input mode** and displays the Action Replay's main menu. In this mode, users can enter cheat codes, activate the adapter function for games from other regions, and adjust other settings.
   - Through the menu, parameters can be selected or entered, which will later be applied in cheat mode.
   - All what is happening here is gathering user input and saving it to the CMOS SRAM to be available for the Datel IC in cheat mode.

### I/O Ports

[Fullsnes Nocash SNES Specs](./docs/Fullsnes-Nocash_SNES_Specs.pdf) documentation is describing several I/O ports used by Pro Action Replay MK3 to control its functions and settings. The following ports are known:

```
0x100000, 0x100001, 0x100002, 0x100003  ; Code 0 (DTA,LSB,MID,MSB) (code 0)
0x100004, 0x100005, 0x100006, 0x100007  ; Code 1 (DTA,LSB,MID,MSB) (code 1)
0x100008, 0x100009, 0x10000A, 0x10000B  ; Code 2 (DTA,LSB,MID,MSB) (code 2)
0x10000C, 0x10000D, 0x10000E, 0x10000F  ; Code 3 (DTA,LSB,MID,MSB) (code 3)
0x100010, 0x100011, 0x100012, 0x100013  ; Code 4 (DTA,LSB,MID,MSB) (code 4)
0x100014, 0x100015, 0x100016, 0x100017  ; Code 5 (DTA,LSB,MID,MSB) (always NMI.LSB)
0x100018, 0x100019, 0x10001A, 0x10001B  ; Code 6 (DTA,LSB,MID,MSB) (always NMI.MSB)

0x10001C            ; Control A (bits 4, 6 and 7)
                    ; Bits 0-3 Should be 0
                    ; Bit 4 ROM Mapping (0=Normal, 1=Temporarily disable BIOS & enable GAME ROM)
                    ; Bit 5 Should be 0
                    ; Bits 6-7 Select/force Video Type (0=Normal, 1=NTSC, 2=PAL, 3=Reserved)

0x10001D-0x10001F   ; Set to zero (maybe accidently, trying to init "code 7")

0x10003C            ; Control B (set to 01h upon game start)

086000h             ; Control LEDs (bits 0 and 1)
                    ; Bit 0 Control left or right LED? (0=on or off?, 1=off or on?)
                    ; Bit 1 Control other LED (vice versa of bit0)
                    ; Bits 2-7 Should be 0

206000h             ; Control C (bit 0)
                    ; Bit 0 (0=BIOS or PAR-NMI Execution, 1=GAME Execution)
                    ; Bits 1-7 Should be 0

0x008000            ; Control D (set to 00h upon PAR-NMI entry)
```

> **Codes 0-6:** Unused codes are initialized with the value `0x00000000`. Unlike the PAR1 and PAR MK2 formats, all codes use a linear 24-bit address space without modifying or removing bit 15. Of the seven available code slots, codes 5 and 6 are reserved for hooking the `NMI` (Non-Maskable Interrupt) handler, even when no `WRAM` software patches are active. Therefore, a maximum of five hardware ROM patches can be applied simultaneously.

### Hypothetical Analysis of Chip Interaction

Based on the known functions of the module and the analyzed hardware, the following explanation is purely **speculative** about how the chips might work together:

- **Processing and Control by the Main Chip:** The **LZ95G72** acts as the central control unit, coordinating the cheat and region-bypass functions. It reads its BIOS/firmware and ROM-Image data from the EPROM and makes use of the SRAM to store data between cartrige modes toggeled by the switch.

- **Memory and Logic Processing:** The **HY62256A (SRAM)** temporarily stores active cheat data, detected cheat memory addresses (by the cheat finder) and settings that the module applies during gameplay. Its a fact that many HiROM games use their own SRAM in the same address range (`6000h–7FFFh`). It is not clearly documented how the Pro Action Replay hardware handles such conflicts or whether it can disable its own SRAM for compatibility. While the original PAR 1 was apparently designed for LoROM games, later versions (PAR MK2 and PAR MK3) reportedly include HiROM support. In these cases, the hardware must dynamically enable or disable the internal SRAM depending on whether game code or Pro Action Replay routines (such as the NMI and `DEADC0DE` handlers) are being executed.

- **Storage and Program Access:** The **AT27C010 EPROM** holds the BIOS/firmware of the IC as well as the user interface (ROM-image) for the Action Replay.

However, the exact technical implementation remains unknown.
