# A 4,068-point Scrabble Solitaire Game

In Scrabble Solitaire, the idea is to make the highest possible total score, using a standard board, the standard tiles from the 100-tile bag, but allowing any 7 tiles to be chosen for each play. I vibe-coded a program that gets to a **4,068 point** game. The word list I used  merges the standard ENABLE Scrabble list with the obsolete Webster's Second. Several words, such as JACKPUDDINGHOOD, would  not be legal in tournament play today.
A [famous game](https://digitalcommons.butler.edu/cgi/viewcontent.cgi?article=2692&context=wordways) scored **4,001 points** using a similar word list.


| file | what it is |
|---|---|
| `scrabmax.cpp` | the solver in C++ |
| `game_4068.txt` | the best game found so far|
| `enable_web2.txt` | thee dictionary, merging the standard ENABLE word list and  Webster's Second Dictionary |
| `scrabble_video.py` | makes the MP4/GIF animation of a game |
| `scrabble_4068.gif`, `.mp4` | the animation |



## The solver

### Build

    clang++ -O3 -std=c++17 -pthread -o scrabmax scrabmax.cpp      # or g++

### Run

    ./scrabmax enable_web2.txt -p 60 -w 80 -m 12 -v 14 > game.txt     # the settings that found 4,068

It takes a minute or two to run

| option | meaning | default |
|---|---|---|
| `-t N` | threads (results don't depend on it) | all cores |
| `-p K` | how many of the best plans to play out; `0` = no plan, plain beam search | 20 |
| `-c N` | planner candidates for the edge rows (the middle row gets 5×) | 1000 |
| `-w W` | beam width (states kept per number of tiles on the board) | 20 |
| `-m M` | free moves kept per state, per number of tiles placed | 8 |
| `-v V` | estimated points per tile the plan doesn't use (ranks plans and states) | 10 |
| `-H X` | consider a hook connector only if it's expected to add more than X points | 0 |
| `-D` | print the hook-connector options found for each plan | |
| `-f FILE` | play this plan (move lines, e.g. from `-P` or a game file) instead of planning | |
| `-P` | print the plans found and stop | |

Progress goes to stderr. The output is `BEST <score>`, then one line per move, then the final board
(blanks in lowercase). Each move looks like this:

    A  7  6 CENTENA         placed=7 score=70  plan

`A`/`D` means across/down, then the zero-based row and column of the word's first square,
the whole word, number of tiles placed, points, and whether the move is part of the plan or an extra.

### How it works

1. **Planner**: Finds a *skeleton*: 15-letter words across rows 0, 7 and 14, joined by 8-letter
   column words ("connectors"), that can be built as a sequence of legal plays:
   opening word through the center → the rest of the middle row's 8 pre-placed letters → the
   middle-row play covering both of its triple-word squares (×9) → connectors, each a 7-tile bingo
   hooked onto the middle row → short words around the connectors' ends in rows 0 and 14 → the two
   edge plays covering three triple-word squares each (×27). Those cover all 8 triple-word squares on
   the board. Skeletons use no blanks, and each candidate plan is scored exactly, move by move.
   The middle row's pre-placed letters may be split into pieces, each fed by a *reverse connector*
   coming back from an edge row; plans may add *hook connectors*, column words completed by a new
   tile of a ×27 play (so they are tripled on columns 0, 7 and 14).
2. **Beam search**: Plays the game from an empty board. A state's children are the next plan move,
   plus the best moves with leftover tiles, on squares and tiles the plan doesn't need. Those can come
   at any point in the plan; one next to a plan square is valued by exactly re-scoring the rest of the
   plan after it, and is rejected if it would make a later plan move illegal. A move that needs a
   letter the plan has reserved can take the real tile and put a blank on that plan square instead, if
   that scores more. States are ranked by points so far + exact value of the rest of the plan +
   `V` × unplayed leftover tiles.

## The animation maker

Needs Python 3.8+ and Pillow 10.1+; ffmpeg is needed for MP4 (and makes a smaller GIF).

    pip install Pillow                  # and: brew install ffmpeg
    python3 scrabble_video.py game_4068.txt                    # writes game_4068.mp4 and game_4068.gif
    python3 scrabble_video.py game.txt -o mygame --gif         # GIF only, as mygame.gif
    python3 scrabble_video.py game.txt --png                   # just the final board
    python3 scrabble_video.py game.txt --title "My game" --subtitle "ENABLE word list"

| option | meaning | default |
|---|---|---|
| `-o PATH` | output path without extension | the game file's name |
| `--mp4`, `--gif`, `--png` | which files to write (PNG = final board) | MP4 + GIF |
| `--title`, `--subtitle` | scoreboard heading | "Highest-scoring Scrabble game" / "One player · 100-tile bag" |
| `--fps N` | frames per second | 10 |
| `--hold S` | seconds to pause after each turn | 1.0 |
| `--big N` | turns worth at least N points get a longer pause and a bold row | 300 |
| `--gif-width N` | GIF width in pixels (the MP4 is 1600×900) | 1000 |
| `--no-ffmpeg` | make the GIF with Pillow only | |
| `--font`, `--font-bold`, `--font-black` | font files to use | Inter if installed, else Helvetica/Arial/DejaVu |

It reads the solver's game files (other lines are ignored) and checks that each move fits on the
board and agrees with the letters already there. It does not check that words are legal or that
scores are right.
