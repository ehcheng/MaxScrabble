<div style="text-align: right" align="right"><i>Peter Norvig<br>Nov 2026</i></div>

# A 4,068-point Scrabble Solitaire Game

In Scrabble Solitaire, the idea is to make the highest possible total score, using a standard board, the standard tiles from the 100-tile bag, but allowing any set of up to 7 tiles to be chosen from the bag on each play. 


I vibe-coded a program that gets to a **4,068 point** game. The word list I used  merges the standard ENABLE Scrabble list with the obsolete Webster's Second. Several words, such as JACKPUDDINGHOOD, would  not be legal in tournament play today.
My goal was to beat a [famous game](https://digitalcommons.butler.edu/cgi/viewcontent.cgi?article=2692&context=wordways) that scored **4,001 points** using a similar word list. I achieved my goal.

<img src="scrabble_4068.png" width=800>



| file | what it is |
|---|---|
| `scrabmax.cpp` | the solver in C++ |
| `game_4068.txt` | the best game found so far|
| `enable_web2.txt` | the dictionary |
| `scrabble_video.py` | makes the MP4/GIF animation of a game |
| `scrabble_4068.gif`, `.mp4`, `.png` | the animation and the final frame |



# The solver

## Build and Run

    clang++ -O3 -std=c++17 -pthread -o scrabmax scrabmax.cpp      # or g++

    ./scrabmax enable_web2.txt -p 60 -w 80 -m 12 -v 14 > game.txt # the settings that found 4,068

It takes a minute or two to run.

| option | meaning | default |
|---|---|---|
| `-t N` | threads to run in parallel | all cores |
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

## Strategy

1. **Find and rank 15-letter words**: It seems clear that the triple word score squares are vital, and that the best way to use them is to place tiles that fill three triple word scpres at once along the top or bottom rows, giving a 27-times multiplier for the word (and in the middle row, a 9-times multiplier). So the first step is to try every 15-letter word in the dictionary, and count how many points you could get by placing those 15 letters (over several turns, because you can only place 7 at a time).
2. **Form a skeleton of three 15-letter words with connectors**: From the list of 15-letter words in sorted order, most points first, choose triples of top/middle/bottom row, and search for a way to join them with connecting words. Keep the *c* best-scoring triples. Build the skeleton as follows:
   - The opening word must be through the center star, and we build up (in several plays) to 8 letters in the center row.
   - Add 7 letters to get a 50 point bingo bonus and a triple-triple word score for the 15-letter word.
   - Add 8-letter connector column words (building on the one letter already in the middle row).
   - In general, there are two good ways to add words: 7 letters at once, to get the 50 point bingo bonus, or 1 (or  a few) letters at a time, to score multiple times on multiple words. For example, AX → MAX → MAXI → MAXIM → MAXIMA → MAXIMAL → MAXIMALS. My search found that using the bingo is the better way to go.
   - Optionally there can be other words off of the connector column words.
   - Build up short words in the top and bottom row until they both have 8 letters.
   - Add 7 letters to get a bingo and a triple-triple-triple word score for the 15-letter word.
3. **Rank the skeletons**: Score each skeleton by the number of points it gets, plus an estimate of how many points it might get from the unused letters. Go through them in ranked order.
4. **Fill in the skeletons**: Use a beam search to try a large number of promising possibilities for filling in the skeleton with the remaining letters. Beam search means that we try making the first move, keep the best-scoring *p* moves, and from each of them try making second moves, again keeping a total of *p* of the best-scoring moves.

# Animation maker

![Animation](scrabble_4068.gif)

A python program to make .gif and .mp4 files showing an animation of a game's progress, like [this](https://github.com/norvig/MaxScrabble/blob/main/scrabble_4068.gif). Requires Python 3.8+, and Pillow 10.1+, ffmpeg.

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

It reads the solver's game files and checks that each move fits on the
board and agrees with the letters already there. It does not check that words are legal or that
scores are correct.
