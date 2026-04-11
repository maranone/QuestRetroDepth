# Project Preservaction

[Table Of Contents](preservaction-toc.md)

## `DEADC0DE` Reverse Engineering

Different from the regular ones you find `DEADC0DE` cheatcodes for the Action Replay beginning from MK2. They look very unusual, and a quick google research shows? Right, nothing. The only thing you find about it in the Action Replay manuals, besides the deadcodes itself, is the following text:

> It is important to note that the codes should be in the correct order and no spaces between them. Deadcodes are the only situation where this is important. It is important that you enter the first line 'DEADC0DE' as well as the rest. You can enter only one deadcode per game.

Additionally, as [Fullsnes Nocash SNES Specs](./docs/Fullsnes-Nocash_SNES_Specs.pdf) states:

> One can use only one DEADC0DE at a time, and, when using it, there are some more
restrictions: On PAR2a/b one cannot use ANY other hardware/software patches. On PAR3 one
can keep using ONE hardware patch (and any number of software patches).

Another hint gives us the packaging of MK3 (and MK2 as well):

> NEW - 'DEAD CODE' GENERATOR: "Dead Codes" are super cheats which effictively enables you to re-write whole sections of the game giving total control. No other product features "Dead Codes".

Sounds good, right? But where is the generator? How to write dead codes? Nobody knows :smile:

## Disassambler for SNES

In case you want to tinker yourself a little bit, I recommend using [Dispel DEADC0DE Edition](https://github.com/timboettiger/dispel-snesdc-edition)! It's a modern version of Dispel with build-in support for deadc0des and the ability to decribe what the code does.

That repository is a sister project to this repository and is a convenient way to study and understand Super Nintendo Machine Language (which is used by deadc0des).

## Insights

1. [The Story](deadc0de-story.md)
2. [Learnings](deadc0de-learnings.md)
3. Technical Code Structure
   - [Cheatformat](preservaction-cheatformat.md)
   - [Prologue/Epilogue Variants](preservaction-prologue-epilogue.md)
4. [Generator](deadc0de-generator.md)
5. [Opcodes](deadc0de-opcodes.md)
6. Deadcode Analysis
   - [Cybernator (UK Version)](deadc0de-cybernator-uk.md)
   - [Mortal Combat (UK Version)](deadc0de-mortal-combat-uk.md)
   - [Mystical Ninja II (JAP Version)](deadc0de-mystical-ninja-2-jp.md)
   - [Starfox/Starwing (UK Version)](deadc0de-starfox-uk.md)
   - [Super Streetfighter II (US Version)](deadc0de-super-streetfighter-2-us.md)
   - [Super Taz-Mania (US Version)](deadc0de-super-taz-mania-us.md)
   - [Wolfenstein 3D](deadc0de-wolfenstein-3d.md)
