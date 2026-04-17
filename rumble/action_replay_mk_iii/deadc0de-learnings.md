# DEADC0DE Learnings

[Dead Code Overview](preservaction-deadc0de.md)

## Preparation

If you want to try disassembling yourself, you have the choice!

**BREAKING NEWS** its recommended to use the [Dispel DEADC0DE Edition](https://github.com/timboettiger/dispel-snesdc-edition)! It's a modern version of Dispel with build-in support for deadc0des and the ability to decribe what the code does.

1. Clone the [Dispel-DEADC0DE Github-Repository](https://github.com/timboettiger/dispel-snesdc-edition) to get a disassembler.
2. Compile Dispel using `make`.
3. Test if it runs correctly (it should display instructions) using `./dispel -h`
4. Disassemble the readymade binaries using `./dispel -f=annotated ../par-mk3/deadc0de/binaries/deadcode.bin` (where "par-mk3" is your clone of this repository and "deadcode.bin" is the filename of the cheat you want to analyze).

Or do you want to go the hard way? It's up to you:

1. Clone the [Dispel Github-Repository](https://github.com/pelrun/Dispel) to get a disassembler.
2. Check if you can apply the [Special Edition Patch](./bin/dispel-special-edition.patch) to the repository using `git apply --check dispel-special-edition.patch`.
3. If everything is fine, apply the [Special Edition Patch](./bin/dispel-special-edition.patch) to the repository using `git apply dispel-special-edition.patch`.
4. Compile Dispel using `make`.
5. Test if it runs correctly (it should display instructions) using `./dispel`
6. Convert hex dumps to binary files using `xxd -r -p deadcode.hex deadcode.bin` or the convenient shell script [hex2asm.sh](./deadc0de/hex2asm.sh) within the deadc0de folder.
7. You can now use Dispel to disassemble dead codes using `./dispel -A deadcode.bin > deadcode.asm`

## Dead Code Structure

Thanks to a helpful hint from [Pelrun](https://github.com/pelrun), I discovered that the team at [Kodewerx](https://doc.kodewerx.org/hacking_snes.html) had already documented the structure of two different [Action Replay cheat code types](preservaction-cheatformat.md). While a few minor details in their documentation might be slightly inaccurate, the overall idea of their format is indeed a good approach for the second cheat code format.

They refer to it as a "32-bit multi-write", although this title is somewhat misleading: it mainly refers to the 32-bit (4-byte, or `DWORD`) magic word "`DEADC0DE`". Their example actually demonstrates the 32-bit magic word followed by a 64-bit (8-byte) data structure:
```hex
DEADC0DE
aaaaaadd
xxxxxxxx
```

They describe it as follows:
- go to memory location `aaaaaa`
- write `dd` times the value `xxxxxxxx`

It might describe a valid cheat code type (one that I have not encountered yet), but it appears to follow a slightly different logic than the cheat codes currently documented and available. This is also documented within the [Fullsnes Nocash SNES Specs](./docs/Fullsnes-Nocash_SNES_Specs.pdf) Documentation.

### Cheatcode Header

We agree that the second cheat code type starts with the magic word `DEADC0DE` (yes, it is always 32-bit, 4-byte, or `DWORD`). However, what follows is not a single structure but rather multiple structures, each differentiated by a length byte (more on that later).

### Blockwise Processing

What is also certain is that the following code structure is organized in `DWORD`s (4-byte blocks). If the capacity of a `DWORD` is not fully used, it is padded with `EA` opcodes (which represent `NOP` or "no operation" in the 65C816 assembly language).

### Maximum Dead Code Length

- You can enter `100` cheats using the Action Replay UI, that's `400` bytes.
- Subtracting the magic word leaves `396` bytes.
- Subtracting target memory location and length leaves `392` bytes.
- The Action Replay's RAM is `32KB`, that should work out.

According to this, offical dead codes can have a maximum length of `98 DWORD`s or `392` bytes.

### Maximum Option Length

Optional cheats that can be placed within a dead code frame are, at least in known examples, always 2x8 bytes. However, I doubt that this limit is necessary, because multiple options can be used in succession. Including the dead code frame, the number of `DWORD`s is known as either even or odd.

### Injection Timing

The interesting question, of course, is when the dead codes are injected or written to the target memory address. I currently assume this happens at the same time the regular cheats would normally be applied. Just a guess, but I think (source 2 in mind) every standard cheat from Action Replay gets converted into a dead code first. That's probably why it doesn't make any difference.

Available sources ([^Spritedmind], [^Maxim]) suggest that the Action Replay taps into the `VINT` interrupt, executes cheats, and then resumes the ROM execution (for other ar versions of other consoles, though). `VINT` refers to the Vertical Interrupt or Vertical Blank Interrupt (`V-Blank`) on the SNES. This Non-Maskable Interrupt (`NMI`) is triggered by the SNES's Picture Processing Unit (`PPU`) at the end of each frame when the CRT monitor's beam returns from the bottom to the top. This allows changes to be made without causing visible graphical glitches, like screen flickering.

1. Activation of the Cheat Mode of Action Replay (switch is up)
2. Action Replay writes (or injects if you like) the code to the memory
3. At every Non-Maskable Interrupt (`NMI`) from the `PPU`, Action Replay sets the Program Counter (`PC`) to the cheats location
4. The cheat gets executed, as if it's regular game logic
5. At the cheats end, a `JMP` sends the `PC` to the game's regular interruppt handler

> When the assumption at 3 is correct, it would not be possible to overwrite game logic.

Credits/Source
([^Spritedmind]) [Pro Action Replay reverse-engineering notes](https://gendev.spritesmind.net/forum/viewtopic.php?t=813)
([^Maxim]) [How the ... Action Replay ... work](./docs/how_par_work-v3-2001.txt)
