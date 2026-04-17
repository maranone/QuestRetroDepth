# Project Preservaction

[Table Of Contents](preservaction-toc.md)

## ROM

The goal of this section is to uncover and understand the data structure of the ROM, focusing specifically on locating the built-in game list and the associated cheat codes stored within the file.

In addition, I want to know more about the interaction between ROM (SNES runtime) and the Datel IC (cheat runtime)

### Versions

The following ROM Versions are known:

| Version | Date    | Langugage |
|---------|---------|-----------|
| `V1.0G` | 19/7/95 | German    |
| `V1.0U` | 19/7/95 | English   |

What about the suffix? In my opinion 'G' stands for 'Germany', where 'U' stands for 'United Kingdom'.

At first sight, they seem identical, but there are differences concerning the games list as well as supported cheat codes (the `UK`-version supports more of it).

### Structure

**File**
- Name: `Pro Action Replay MK3 BIOS (Europe) (Unl).sfc`
- Size: `131,072 Bytes`

```
|  Offset   |  Length  |  Description             |  Content                        |
|-----------|----------|--------------------------|---------------------------------|
|  0x00000  |   18296  |  Logic                   |                                 |
|  0x04778  |    3464  |  Texts                   |  UI Menus, Labels, Hints, etc.  |
|  0x05500  |      16  |  Blank                   |  <0x00>                         |
|  0x05510  |   10912  |  Blank                   |  <0xFF>                         |
|  0x07FB0  |   29552  |  Logic Blocks            |  Handlers of UI Elements?       |
|  0x0F320  |    3296  |  Blank                   |  <0xFF>                         |
|  0x10000  |    9322  |  Games List and Codes    |                                 |
|  0x1246A  |     826  |  Code Description Masks  |                                 |
|  0x127A4  |      82  |  Blank                   |  Alternating <0x20><0x00>       |
|  0x127F6  |    2369  |  Code Description Masks  |                                 |
|  0x13137  |     417  |  Code Description Words  |                                 |
|  0x132D8  |   52520  |  Blank                   |  <0x30>                         |
```

### Extracting A Game

**Games List and Codes**

The serialized game database is a sequence of games. Per game you see the following structure:

```
Offset 0x10000:
19 00 41 43 52 4F 42 41 54 20 4D 49 53 53 49 4F 4E 00 01 7E 04 12 04 00 0A 6E
|           19  |  ?                         |  25                        |
|           00  |  Null Terminator           |                            |
|  41 <...> 4E  |  Game Name                 |  ACROBAT MISSION           |
|           00  |  Null Terminator           |                            |
|           01  |  Amount of following codes |  1                         |

Per Code:
|  7E 04 12 04  |  Raw Cheat Code            |  7E0412:04                 |
|           00  |  Null Terminator           |                            |
|           0A  |  Relative Desc Mask Offset |  10                        |

Suffix:
|           6E  |  ?                         |  110                       |
```

As you can see, cheat codes are serialized within the game listing. After the suffix from the game the next game starts right away in the same style.

The information you have is the name of the game and the cheat codes itself. To retrieve the cheatcode description you have to resolve the description mask.

**Resolve Relative Description Mask Offset**

You can think of the description mask as a template with placeholders: ASCII characters remain while Non-ASCII characters will be replaced by other texts.

```
Offset 0x1246A:
- Count Null Terminators 
- Start with counter = 0
- Until it is equal to relative offset (10)
- Then extract until next Null Terminator

0.     56 41 43 41 4E 54 20 43 4F 44 45
1.  00 4E 4F 20 44 45 53 43 52 49 50 54 49 4F 4E
2.  00 54 52 41 4E 53 46 45 52 45 44 20 54 52 41 49 4E 45 52
3.  00 54 52 41 4E 53 46 45 52 45 44 20 51 55 49 43 4B 20 4D 45 4E 55
4.  00 44 45 41 44 43 4F 44 45
5.  00 55 53 45 20 57 49 54 48 20 41 42 4F 56 45
6.  00 55 53 45 20 57 49 54 48 20 42 45 4C 4F 57
7.  00 50 4C 41 59 20 55 2E 53 20 56 45 52 53 49 4F 4E
8.  00 50 4C 41 59 20 45 55 52 20 56 45 52 53 49 4F 4E
9.  00 50 4C 41 59 20 4A 41 50 20 56 45 52 53 49 4F 4E
10. 00 80 31 20 48 41 53 20 81 82
11. 00 ...
```

Resolve Non-ASCII characters within description mask:

```
80 31 20 48 41 53 20 81 82
|                 80  |  Relative Word Mask Offset  |  128        |
|  31 20 48 41 53 20  |  Text                       |  ' 1 HAS '  |
|                 81  |  Relative Word Mask Offset  |  129        |
|                 82  |  Relative Word Mask Offset  |  130        |

```

**Resolve Relative Word Mask Offset**

```
Offset 0x13137:
- Count Null Terminators 
- Start with counter = 128
- Until it is equal to relative offset (128)
- Then extract until next Null Terminator

128.    50 4C 2E
129. 00 55 4E 4C 2E
130. 00 4C 49 56 45 53
140. 00 54 49 4D 45
141. 00 ...
```

Resolve words #128, #129 and #130 from above:

```
|        50 4C 2E  |  Text  |  'PL.'    |
|     55 4E 4C 2E  |  Text  |  'UNL.'   |
|  4C 49 56 45 53  |  Text  |  'LIVES'  |
```

**Result:**

```
|  Game Name       |  ACROBAT MISSION       |
|  Amout Of Codes  |  1                     |
|  Code 1          |  7E0412:04             |
|  Description 1   |  PL. 1 HAS UNL. LIVES  |
```
