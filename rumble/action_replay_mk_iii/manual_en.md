# SNES Action Replay MK3 V1.1 - Manual/Guide (English)

Translation V2.1

Copyright (C) 2024/2025 by Tim Böttiger

## WELCOME!

Welcome to the world of the "Action Replay MK3". The "Action Replay MK3" offers you numerous new possibilities, such as the completely revamped trainer, parameter groups, and the improved German menu system.

> **COPYRIGHT:** `SUPER NES` and `NINTENDO` are registered trademarks of Nintendo America INC.

> **DISCLAIMER:** Nothing from this manual may be reproduced, copied, or distributed in any other way without prior written permission from systems B.V. Ede, Netherlands.

### <a name="schalter"></a> SWITCH ON THE RIGHT SIDE OF THE ACTION REPLAY

Switch options:

1. **Top Position:**
    - Parameters active,
    - Slow-motion function active,
    - Trainer active
2. **Middle Position:**
    - Action Replay off
3. **Bottom Position:**
    - Parameter input,
    - Activation of the Action Replay menu

### <a name="leds"></a> LED'S ON THE FRONT OF THE ACTION REPLAY

- **Left LED (`LED 1`):** Indicates whether and which parameter groups are active. (See chapter "[Parameter Groups](#param_gruppen)")
- **Right LED (`LED 2`):** Indicates the status of the trainer. (See chapter "[The Trainer](#trainer)".)

### <a name="allgemein"></a> GENERAL NOTICE

The Action Replay's menus are navigated using a cursor in the shape of an arrow. This cursor can be controlled with both the **joypad** and the **Super Nintendo mouse**.

> Please note that you must only change the joypad or mouse when the console is turned off.

## <a name="benutzung"></a> USING THE ACTION REPLAY

1. Turn off the console.
2. Insert the Action Replay (the labeling of the module must face forward).
3. Insert the game cartridge into the Action Replay (the labeling of the game cartridge must face forward).
4. Set the Action Replay's switch to the middle position.
5. Turn on the console.
6. The Action Replay's main menu appears.

> **CAUTION:** SWITCHING THE GAME CARTRIDGE MUST ONLY BE DONE WITH THE CONSOLE TURNED OFF!

### <a name="root"></a> THE MAIN MENU

![Main Menu](./ui/clean/clean-main-menu.png)

The main menu consists of the following menu items:

- **SPIEL STARTEN** Selecting this menu item will start the game.
- **CODES EINGEBEN:** Selecting this menu item ("Enter Codes") brings up the Parameter Menu. Further explanations can be found in the chapter "[The Parameter Menu](#param)".
- **EUROP:** This menu item activates the adapter function for the use of imported game cartridges. Further explanations can be found in the chapter "[The Adapter Function](#adapter)".
- **GESCHWINDIGKEIT:** Using this menu item, the speed of the slow-motion function can be adjusted. Further explanations can be found in the chapter "[The Slow-Motion Function](#zeitlupe)".
- **OPTIONEN:** Selecting this menu item brings up the Options Menu, where various settings, such as the type or form of the menu items, etc., can be made. Further explanations can be found in the chapter "[Options](#optionen)".
- **MOEGLICHKEITEN:** This menu item ("capabilities") displays any found parameters. Further explanations can be found in the chapter "[The Trainer](#trainer)".

### <a name="optionen"></a> OPTIONS

![Options](./ui/clean/clean-options.png)

In the **OPTIONEN** Menu, you can change various basic settings (options):

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu. If you have already used the "Action Replay MK II", you can also press the `RESET` button after setting the Action Replay's switch to the bottom position.
2. Use the cursor to select the menu item `OPTIONEN` and press the `B` button. The Options Menu appears.
3. After making the desired changes to the menu items described below, use the cursor to select the menu item `HAUPTMENUE` and press the `B` button.

#### <a name="hauptmenue"></a> HAUPTMENUE

Selecting this menu item brings up the Main Menu.

#### <a name="menuefeld"></a> MENUEFELD 01

Using this menu item, the type or form of all menu items can be changed. You have seven different types or forms to choose from.

#### <a name="hintergrund"></a> HINTERGRUND AUS

Using this menu item, the screen background can be set into motion (`AN` = `ON`). Selecting it again turns this function `AUS` (`OFF`).

#### <a name="trainer_select"></a> TRAINER-SELECT

The trainer function is controlled via several key combinations (in combination with the `SELECT` button). Since the `SELECT` button is already used in some games, selecting this menu item can change this default setting so that the `START` button can be used for the key combinations.

- Selecting this menu item brings up `TRAINER-START`.
- Selecting it again assigns the `SELECT` button again.

#### <a name="joypad"></a> JOYPAD

![Joypad](./ui/clean/clean-joypad.png)

Selecting this menu item will display the joypad's configuration.

### <a name="param"></a> THE PARAMETER MENU

![Parameter](./ui/clean/clean-codes.png)

Explanations for the Parameter Menu:

- At the top of the screen is the name of the inserted game cartridge (e.g., "SUPER MARIOWORLD"). For game cartridges with Japanese characters, the note `NICHT DARSTELLBAR` ("NOT DISPLAYABLE") may appear instead of the name.
- The two menu items under the label `AKTUELLER PARAMETER` refer to the selected parameter. The left menu item is referred to in the following text as `Input Line`, and the right as `Group Field`. Further explanations can be found in the chapter "[Selecting Parameter Groups](#param_gruppen_auswahl)".
- Under the label `ZEICHENFELD` are the digits required to input a parameter.
- To the right of the `ZEICHENFELD` is the so-called Parameter List, where up to 100 parameters can be entered.
- To the right of the Parameter List are four directional arrows:
    - **Top Directional Arrow:** Selecting this arrow will scroll the Parameter List back by one page.
    - **Bottom Directional Arrow:** Selecting this arrow will scroll the Parameter List forward by one page.
    - **Middle Directional Arrows:** Selecting these arrows will scroll the Parameter List up or down by one parameter each.

### <a name="param_menue"></a> Menu Items of the Parameter Menu

- **O.K.:** Selecting this menu item will confirm the entered or modified parameter and transfer it to the Parameter List.
- **RST:** Using this menu item will delete the parameter marked `red` in the Parameter List or set it to `7E000000` and deactivate it (`AUS`).
- **ALL:** This menu item deletes all parameters entered in the Parameter List or sets all parameters to `7E000000` and turns them off (`AUS`). After selecting this menu item, the following safety prompt appears:
    - `JA, RESET` (YES, RESET) Deletes all parameters in the Parameter List.
    - `NEIN, KEIN RESET` (NO, NO RESET) Cancels the selection of the menu item.
- **KONV:** This menu item converts "Game Genie TM" codes into ACTION-REPLAY parameters. To convert, the "Game Genie TM" code must be entered into the Input Line. Then, select the `KONV` menu item with the cursor and press the `B` button. After conversion, the parameter can be transferred to the Parameter List.

> Please note that up to `five` Game Genie codes can be converted.

- **VOREING:** Selecting this menu item brings up the menu of built-in parameters. Further explanations can be found in the chapter "[Using Built-In Parameters](#param_nutzen)"
- **MENUE:** Selecting this menu item brings up the Main Menu.

### <a name="param_input"></a> PARAMETER INPUT

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. In the main menu, use the cursor to select the menu item `CODES EINGEBEN` and press the `B` button. The Parameter Menu appears. There are `100` input lines in the Parameter List.
    - Since almost every parameter starts with `7E`, these digits are already predefined in each line (but can be changed if necessary).
    - After that, there are six zeros in each line. To enter a parameter, proceed as follows:
3. Use the cursor to select an input line in the Parameter List and press the `B` button. The parameter entry will automatically appear in the input field at the top of the screen. The white-marked `0` represents the input position.
4. Now take the parameter(s) for the inserted game cartridge, which you can find in the included codebook or have discovered using the trainer.
5. Enter the parameter using the digits in the Character Field. To do this, select the respective digit with the cursor and press the `B` button. The letter or number will now appear in the input field.
    - The next `0` or digit is now white-marked. This white marking always indicates the current position in the input field.
    - If you want to correct an already entered digit, select it with the cursor and press the `B` button. Then select the correct digit in the Character Field with the cursor and press the `B` button.
6. After entering the parameter, use the cursor to select the menu item `O.K.` and press the `B` button. The entered parameter is transferred to the selected input line (indicated by the red marking) and activated (set to `AN`).
    - As described above, you can now enter `99` additional parameters for the currently used game. However, the parameters are only stored until they are replaced or the console is turned off.
    - If you want to delete a stored parameter, select the menu item `RST` with the cursor and press the `B` button.
7. After entering the parameter(s), select the menu item `MENUE` with the cursor and press the `B` button.
8. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
9. The game starts.
10. When the game logo appears, set the Action Replay's switch to the top position. `LED 1` will begin to light up.
11. *The gameplay is now altered by the entered parameter(s).*
12. If you want to continue playing the game without changes, set the Action Replay's switch to the middle position.

### <a name="param_nutzen"></a> USING BUILT-IN PARAMETERS

![Built-In Parameters](./ui/clean/clean-games.png)

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. In the main menu, use the cursor to select the menu item `CODES EINGEBEN` and press the `B` button.
3. In the Parameter Menu, use the cursor to select the menu item `VOREING` and press the `B` button. A new menu with a list of game names appears.
4. Next to the selection field are four directional arrows:
    - **Top Directional Arrow:** Selecting this arrow will scroll the list of game names back by one page.
    - **Bottom Directional Arrow:** Selecting this arrow will scroll the list of game names forward by one page.
    - **Middle Directional Arrows:** Selecting these arrows will scroll the list of game names up or down by one game title each.
5. Use the cursor to select the game title for the game you are currently using and press the `B` button. A list of available parameters appears.
6. If you now want to transfer a parameter to the Parameter List, select the desired parameter with the cursor and press the `B` button. Selecting it again will remove the parameter from the Parameter List.
    - If you want to transfer all available parameters for the game to the Parameter List, select the menu item `ALLE KOPIEREN` with the cursor and press the `B` button.
    - After selecting the menu item `VOREING` with the cursor and pressing the `B` button, the list of game names appears again.
    - After selecting the menu item `ALLE LOESCHEN`, all parameters are removed from the Parameter List.
7. Use the cursor to select the menu item `CODE-MENUE` and press the `B` button.
8. In the Parameter Menu, use the cursor to select the menu item `MENUE` and press the `B` button.
9. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
10. The game starts.
11. When the game logo appears, set the Action Replay's switch to the top position. `LED 1` will begin to light up.
12. *The gameplay is now altered by the entered parameter(s).*
13. If you want to continue playing the game without changes, set the Action Replay's switch to the middle position.

### <a name="param_gruppen"></a> PARAMETER GROUPS

In addition to the parameters that are always activated (`AN`) by entering them in the Parameter List, the "Action Replay MK3" also has two parameter groups to which multiple parameters can be assigned.

These `A` and `B` groups can be toggled on or off at certain points in the game using a key combination.

- **Activation/Deactivation of the `A` Group:** `SELECT` button + up directional button. When the `A` group is activated, `LED 1` blinks rapidly.
- **Activation/Deactivation of the `B` Group:** `SELECT` button + down directional button. When the `B` group is activated, `LED 1` blinks slowly.

> If both groups are activated, `LED 1` remains lit steadily.

> Please note that all parameters are only active if the Action Replay's switch is in the top position.

### <a name="param_gruppen_auswahl"></a> SELECTING PARAMETER GROUPS

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. In the main menu, use the cursor to select the menu item `CODES EINGEBEN` and press the `B` button. The Parameter Menu appears.
3. Next to the Input Line is the `GRUPPENFELD`. By selecting this group field with the cursor and pressing the `B` button, you can choose which group a parameter should be assigned to. Parameter markings:
    - Parameters that are `always active` are marked `red`.
    - Parameters assigned to the `A Group` are marked `dark blue`.
    - Parameters assigned to the `B Group` are marked `light blue`.
4. After entering and assigning the parameter(s), use the cursor to select the menu item `MENUE` and press the `B` button.
5. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
6. The game starts.
7. When the game logo appears, set the Action Replay's switch to the top position. `LED 1` will begin to light up.
8. *The gameplay is now altered by the entered parameter(s).*

> If you want to continue playing the game without changes, set the Action Replay's switch to the middle position.

## <a name="trainer"></a> THE TRAINER

The purpose of the trainer function is to find the memory address in a game where values such as the number of lives or energy are stored. The result is, for example, the parameter for infinite lives.

> **IMPORTANT:** The trainer is not compatible with its very limited number of games (`LED 2` does not light up when pressing the `SELECT` or `START` button). In this case, you can inquire about the corresponding parameters via the ACTION-REPLAY HOTLINE or obtain them from various specialized magazines.

Please note that the trainer can only be activated during gameplay. Ensure that the switch remains in the top position during the trainer process.
The trainer is controlled during the game using the following key combinations.

Since the `SELECT` button (default setting) is already used in some games, you can change this default setting in the Options Menu so that the `START` button can be used for the key combinations. Further explanations can be found in the chapter "[OPTIONS](#optionen)".

| FUNCTION                                   | KEY COMBINATION                    |
|:-------------------------------------------|:-----------------------------------|
| Delete Trainer                             | `SELECT` + `START`                 |
| Start Trainer / Same Value as Before       | `SELECT` + `X` or `START` + `X`    |
| Value Greater Than Before                  | `SELECT` + `A` or `START` + `A`    |
| Value Less Than Before                     | `SELECT` + `Y` or `START` + `Y`    |
| Completely Different Value                 | `SELECT` + `B` or `START` + `B`    |
| Activate Found Parameter                   | `SELECT` + `R` or `START` + `R`    |

To activate the above functions (except `TRAINER LÖSCHEN`), you must first hold down the `SELECT` or `START` button and then press the second specified button. To activate the function `TRAINER LÖSCHEN`, you must press the `SELECT` and `START` buttons simultaneously.

### <a name="param_leben"></a> PARAMETER SEARCH FOR INFINITE LIVES

In this example, we refer to the game "SUPER MARIOWORLD TM," where you start the game with 5 lives.

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. Set the Action Replay's switch to the top position.
3. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
4. Start the game until you can move the character.
5. Delete the trainer's memory by pressing the `START` and `SELECT` buttons simultaneously.
6. Start the trainer by pressing the `SELECT` and `X` buttons. The Action Replay now saves all values between 0 and 255.
7. You must now lose a life so that only 4 remain.
8. Since there are now only 4 lives available (you started with 5), the current value is less than before. Accordingly, press the key combination `SELECT` and `Y`.
9. Repeat points 7 & 8 until the message `GAME OVER` appears. You must lose a life with each repetition and then use the aforementioned key combination.
10. Start the game again (you now have 5 lives available). Since there was only 1 life available in the previous run and now there are 5 (current value is greater than before), you must press the key combination `SELECT` and `A`.
11. Continue the game to about halfway through the level without losing a life.
12. Since there are still 5 lives available (current value is equal to before), you must press the key combination `SELECT` and `X`. The trainer sorts out irrelevant parameters.
13. Repeat points 7 - 12 until `LED 2` blinks rapidly (indicating that only one possible parameter remains). If it blinks slowly, the trainer has found multiple possible parameters, and you must continue repeating.
14. When `LED 2` blinks rapidly, you can activate the found parameter using the key combination `SELECT` + `R`.

The parameter we are searching for (infinite lives) for the game "SUPER MARIOWORLD TM" is: `7E0D BE05`.

> IF `LED 2` DOES NOT BLINK AT ALL, YOU MUST FOLLOW THE INSTRUCTIONS IN THE CHAPTER "[LED 2 DOES NOT BLINK !?](#blinkt_nicht)".

### <a name="param_geld"></a> PARAMETER SEARCH FOR INFINITE TIME OR INFINITE MONEY

The storage of time or money varies between games. On the one hand, there are games where three values for time or money are present, divided into hundreds, tens, and units places (e.g., "[SUPER MARIOWORLD TM](#beispiel_smw)").
Accordingly, these are stored in three memory addresses. On the other hand, there are games where time or money is stored in a value between `0` and `255` (e.g., "[STREET FIGHTER II TM](#beispiel_sfii)").

#### <a name="beispiel_smw"></a> Example SUPER MARIOWORLD TM

In this example, we refer to the game "SUPER MARIOWORLD TM," where the time counts down from `400` to `0` seconds. In our example, we will search for the tens value of the time.

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. Set the Action Replay's switch to the top position.
3. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
4. Start the game until you can move the character.
5. Delete the trainer's memory by pressing the `START` and `SELECT` buttons simultaneously.
6. Wait until the tens place of the time displays a `9` (e.g., `393`).
7. Start the trainer by pressing the `SELECT` and `X` buttons.
8. Wait until the tens place of the time displays an `8` (e.g., `386`).
9. Since the tens place of the time is now less than before (previously `9`, now `8`), press the `SELECT` and `Y` buttons.
10. Now ensure that the tens place does not decrease. Wait until, for example, four seconds have elapsed.
11. Since the tens place of the time has not changed (is still `8`), press the `SELECT` and `X` buttons. The trainer sorts out irrelevant parameters.
12. Repeat points 8 and 9 until the tens place of the time displays a `9` again (e.g., `294`). The tens place must decrease by one with each pass (`7`, `6`, `5` ... `0`).
13. Since the tens place of the time is now greater than before (previously `0`, now `9`), press the `SELECT` and `A` buttons.
14. Repeat points 8 to 13 until `LED 2` blinks rapidly.
15. Press the `SELECT` and `R` buttons to activate the found parameter.
16. *If you continue the game now, the tens place of the time will no longer change (infinite time).*

The parameter we are searching for (infinite time) for the game "SUPER MARIOWORLD TM" is: `7EOF 3205`.

> IF `LED 2` DOES NOT BLINK AT ALL, YOU MUST FOLLOW THE INSTRUCTIONS IN THE CHAPTER "[LED 2 DOES NOT BLINK !?](#blinkt_nicht)".

#### <a name="beispiel_sfii"></a> Example STREET FIGHTER II TM

In this example, we refer to the game "STREET FIGHTER II TM," where the time value is stored between `0` and `255`.

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. Set the Action Replay's switch to the top position.
3. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
4. Start the game until you can move the character.
5. Delete the trainer's memory by pressing the `START` and `SELECT` buttons simultaneously.
6. When the time displays 99 seconds, press the `SELECT` and `X` buttons to start the trainer.
7. Wait until the time displays `90` seconds.
8. Since the current time value is less than before (previously `99`, now `90`), press the `SELECT` and `Y` buttons.
9. Repeat points 7 and 8 with increasingly smaller values (e.g., `85`, `78`, `64`, etc.) until the time runs out or the character is defeated.
10. Start the game again.
11. Wait until the time displays `99` seconds.
12. Press the `SELECT` and `A` buttons since the current value is greater than the previous one (now `99`, previously `0`).
13. Repeat points 7 to 12 until `LED 2` blinks rapidly.
14. Press the `SELECT` and `R` buttons to activate the found parameter.
15. *If you continue the game now, the time will no longer change (infinite time).*

The parameter we are searching for (infinite time) for the game "STREET FIGHTER II TM" is: `7E1A C820`

> IF `LED 2` DOES NOT BLINK AT ALL, YOU MUST FOLLOW THE INSTRUCTIONS IN THE CHAPTER "[LED 2 DOES NOT BLINK !?](#blinkt_nicht)".

### <a name="param_energie"></a> PARAMETER SEARCH FOR INFINITE ENERGY

In this example, we refer to the game "STREET FIGHTER II."

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. Set the Action Replay's switch to the top position.
3. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
4. Start the game until you can move the character.
5. Delete the trainer's memory by pressing the `START` and `SELECT` buttons simultaneously.
6. Ensure that the character does not lose energy.
7. Press the `SELECT` and `X` buttons to start the trainer.
8. You must now lose some energy.
9. Since the energy bar is now less than before, press the `SELECT` and `Y` buttons.
10. Continue playing; however, ensure that the character does not lose energy.
11. Since the energy bar is still the same as before, press the `SELECT` and `X` buttons. The trainer sorts out irrelevant parameters.
12. Repeat points 8 and 9 until the character has no energy left.
13. Start the game again. Once the energy is fully available again, press the `SELECT` and `A` buttons since the energy bar is now greater than before (previously no energy, now full energy).
14. Repeat points 8 to 13 until `LED 2` blinks rapidly.
15. Press the `SELECT` and `R` buttons to activate the found parameter.

*If you continue the game now, the energy bar will no longer change (infinite energy).*

The parameter we are searching for (infinite energy) for the game "STREET FIGHTER II TM" is: `7E0C 2B20`.

> IF `LED 2` DOES NOT BLINK AT ALL, YOU MUST FOLLOW THE INSTRUCTIONS IN THE CHAPTER "[LED 2 DOES NOT BLINK !?](#blinkt_nicht)".

### <a name="param_level"></a> PARAMETER SEARCH FOR LEVEL SELECTION

In this example, we refer to the game "SUPER BOMBERMAN 1 TM."

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. Set the Action Replay's switch to the top position.
3. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
4. Start the game until you can move the character.
5. Delete the trainer's memory by pressing the `START` and `SELECT` buttons simultaneously.
6. Press the `SELECT` and `X` buttons to start the trainer.
7. You must now complete the first level.

> IF THE CHARACTER HAS LOST ALL LIVES, YOU MUST RESTART THE GAME AND CONTINUE THE TRAINER BY PRESSING THE `SELECT` AND `X` BUTTONS BECAUSE THE CHARACTER IS STILL IN THE FIRST LEVEL.

8. In the second level, press the `SELECT` and `A` buttons since the character was previously in the first level and is now in the second level.

> IF THE CHARACTER HAS LOST ALL LIVES, YOU MUST RESTART THE GAME AND CONTINUE THE TRAINER BY PRESSING THE `SELECT` AND `Y` BUTTONS BECAUSE THE CHARACTER IS AGAIN IN THE FIRST LEVEL AND WAS PREVIOUSLY IN THE SECOND LEVEL.

9. Repeat points 7 and 8 until `LED 2` blinks rapidly. You must be in the next level with each pass (first in the third, then in the fourth, etc.).
10. Press the `SELECT` and `R` buttons to activate the found parameter.

*If you continue the game, you will find that you remain in the level where you currently are.*

The parameter we are searching for for the game "SUPER BOMBERMAN 1 TM" is: `7E00 A4XX`. For the `XX`, you must insert the level number in which you want to play (`01` for `Level 1`, `02` for `Level 2`, etc.).

> IF `LED 2` DOES NOT BLINK AT ALL, YOU MUST FOLLOW THE INSTRUCTIONS IN THE CHAPTER "[LED 2 DOES NOT BLINK !?](#blinkt_nicht)".

If you want to start in a different level, proceed as follows:

1. Set the Action Replay's switch to the bottom position.
2. Press the `RESET` button.
3. In the main menu, use the cursor to select the menu item `MOEGLICHKEITEN` and press the `B` button.
4. A new menu with a list of all found parameters appears.
5. Use the cursor to select the topmost parameter and press the `B` button.
6. The parameter has now been transferred to the Parameter List (also indicated by the white marking).
7. Use the cursor to select the menu item `CODEMENUE` and press the `B` button.
8. In the Parameter List of the Parameter Menu, you will now find the parameter for level selection.
9. Select the parameter with the cursor and press the `B` button.
10. Now you must change the last two digits of the parameter according to the level you want to play.
11. Use the cursor to select the menu item `MENUE` and press the `B` button.
12. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
13. When the game logo appears, set the Action Replay's switch to the top position.
14. *The game now starts in the level you desired.*

### <a name="param_waffen"></a> PARAMETER SEARCH FOR INFINITE EXTRA WEAPONS ETC.

In this example, we refer to the game "SUPER MARIOWORLD TM." We want the character to always have the "Super Mario" ability.

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. Set the Action Replay's switch to the top position.
3. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
4. Start the game until you can move the character.
5. Delete the trainer's memory by pressing the `START` and `SELECT` buttons simultaneously.
6. Collect the red mushroom (for "Super Mario") with the character.
7. Press the `SELECT` and `X` buttons to start the trainer.
8. You must now continue playing until the character loses the "Super Mario" ability.
9. Press the `SELECT` and `B` buttons since the value we are searching for has not increased or decreased but is always different.
10. Collect the red mushroom (for "Super Mario") with the character.
11. Press the `SELECT` and `B` buttons since the value is different from the previous one (previously the normal character, now "Super Mario").
12. Continue the game to about halfway through the level. Ensure that the character retains the "Super Mario" ability.
13. Press the `SELECT` and `X` buttons since the character still has the "Super Mario" ability. The trainer sorts out irrelevant parameters.
14. Repeat points 8 to 13 until `LED 2` blinks rapidly.
15. Press the `SELECT` and `R` buttons to activate the found parameter.
16. *If you continue the game, you will find that the character always retains the "Super Mario" ability.*

The parameter we are searching for for the game "SUPER MARIOWORLD TM" is: `7E00 1901`.

> IF `LED 2` DOES NOT BLINK AT ALL, YOU MUST FOLLOW THE INSTRUCTIONS IN THE CHAPTER "[LED 2 DOES NOT BLINK !?](#blinkt_nicht)".

If you want to search for a parameter for the extra weapon in a game, you must also follow the above instructions.

## <a name="blinkt_nicht"></a> `LED 2` DOES NOT BLINK!?

In this case, even after the above trainer procedures, too many parameters remain, and you must proceed as follows:

1. Set the Action Replay's switch to the bottom position.
2. Press the `RESET` button.
3. In the main menu, use the cursor to select the menu item `MOEGLICHKEITEN` and press the `B` button.

> IF THE MESSAGE `KEINE PARAMETER VORHANDEN. BITTE STARTEN SIE DEN TRAINER WAEHREND DES SPIELES ERNEUT` APPEARS ("NO PARAMETERS AVAILABLE. PLEASE RESTART THE TRAINER DURING THE GAME"), YOU HAVE MADE A MISTAKE AND MUST START OVER AGAIN!!!

4. A new menu with a list of all found parameters appears. To the right of the selection field or the list are four directional arrows:
    - **Top Directional Arrow:** Selecting this arrow will scroll the list of parameters back by one page.
    - **Bottom Directional Arrow:** Selecting this arrow will scroll the list of parameters forward by one page.
    - **Middle Directional Arrows:** Selecting these arrows will scroll the list of parameters up or down by one parameter each.
5. Use the cursor to select the topmost parameter and press the `B` button. The Action Replay has been programmed so that the valid parameter is always at the top of the list.
6. The parameter has now been transferred to the Parameter List (also indicated by the white marking).
7. Select the menu item `HAUPTMENUE` with the cursor and press the `B` button.
8. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
9. Start the game until you can move the character.
10. Set the Action Replay's switch to the top position.

> Make sure to observe whether the gameplay is affected by the corresponding parameter (e.g., infinite lives, infinite energy, etc.).

If the parameter you used does not affect the gameplay, you must proceed as follows:

- **Infinite Lives:** Chapter "[PARAMETER SEARCH FOR INFINITE LIVES](#param_leben)" from point 7 to point 14.
- **Infinite Time/Money:** Chapter "[PARAMETER SEARCH FOR INFINITE TIME AND INFINITE MONEY](#param_geld)" from point 8 to point 15 or from point 7 to point 14.
- **Infinite Energy:** Chapter "[PARAMETER SEARCH FOR INFINITE ENERGY](#param_energie)" from point 8 to point 15.
- **Level Selection:** Chapter "[PARAMETER SEARCH FOR LEVEL SELECTION](#param_level)" from point 6 (but press `SELECT` and `Y` buttons) to point 10.
- **Infinite Extra Weapons:** Chapter "[PARAMETER SEARCH FOR INFINITE WEAPONS ETC.](#param_waffen)" from point 8 to point 15.

## <a name="zeitlupe"></a> THE SLOW-MOTION FUNCTION

With the slow-motion function, you have the possibility to slow down the gameplay.

1. Follow the steps described in the chapter "[USING THE ACTION REPLAY](#benutzung)" until you reach the main menu.
2. In the main menu, use the cursor to select the menu item `GESCHWINDIGK. I` repeatedly and press the `B` button until your desired setting appears:
    - `GESCHWINDIGK. I`: This option slows down the gameplay to half the normal speed.
    - `GESCHWINDIGK. II`: This option slows down the gameplay to a quarter of the normal speed.
    - `GESCHWINDIGK. III`: This option is the slowest and best suited for the trainer function.
    - `ZEITLUPE AUS`: This option turns off the slow-motion function.
3. Use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
4. Start the game until you can move the character.
5. Set the Action Replay's switch to the top position.
6. Now press the `SELECT` and `L` buttons simultaneously. The slow-motion function is now activated.
7. If you want to turn off the slow-motion function again, you must press the key combination `SELECT` + `L` once more.

## <a name="adapter"></a> THE ADAPTER FUNCTION

### <a name="adapter_pal"></a> Using NTSC Modules on a European Console

1. Turn off the console.
2. Insert the Action Replay (the labeling of the module must face forward).
3. Insert the NTSC game cartridge (American or Japanese) into the Action Replay (the labeling of the game cartridge must face forward).
4. On the back of the Action Replay, you must insert a European game cartridge (the labeling of the game cartridge must face upward).
5. Set the Action Replay's switch to the middle position.
6. Turn on the console.
7. In the main menu, use the cursor to select the menu item `EUROP.` and press the `B` button. The label `USA/JAP` now appears.
8. Use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
9. Start the game.

### <a name="adapter_ntsc"></a> Using European Modules on an NTSC Console

1. Turn off the console.
2. Insert the Action Replay (the labeling of the module must face forward).
3. Insert the European game cartridge into the Action Replay (the labeling of the game cartridge must face forward).
4. On the back of the Action Replay, you must insert a game cartridge that matches the nationality of your console (the labeling of the game cartridge must face upward).
5. Set the Action Replay's switch to the middle position.
6. Turn on the console.
7. In the main menu, use the cursor to select the menu item `SPIEL STARTEN` and press the `B` button.
8. Start the game.

> MAKE SURE TO REGULARLY CLEAN THE ACTION REPLAY'S CONTACTS. ALSO CHECK THE CONTACTS OF YOUR GAME CARTRIDGES. DIRTY CONTACTS CAN LEAD TO MALFUNCTIONS.
