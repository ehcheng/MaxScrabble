#!/usr/bin/env python3
"""scrabble-video: animate a Scrabble game turn by turn, as an MP4 and/or an animated GIF.

The board is drawn in classic colors on the left; on the right a scoreboard lists each turn's
word, tiles played, points and running total. New tiles drop in one at a time, the turn's points
pop up next to the word, and the total counts up.

Input is a game file with one move per line, in the format written by scrabmax:

    A  7  6 CENTENA         placed=7 score=70  plan
    D  0  1 AGRAFFEE        placed=7 score=96
    D  1  0 ay              placed=2 score=3

  A|D        across or down
  row col    first square of the word (0-14, top-left is 0 0)
  WORD       the whole word, including letters already on the board; lowercase = blank
  score=N    points for the move (anything else on the line is ignored)

Other lines (e.g. "BEST 4032 ..." or a printed board) are ignored.

usage: scrabble-video GAME [-o OUT] [--gif] [--mp4] [options]      (see --help)
Needs Pillow. ffmpeg is used for MP4 (and a better-looking GIF) if it is installed.
"""
import argparse, os, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

__version__ = '1.0'

# ============================== board and rules ==============================
N = 15
VAL = dict(zip('ABCDEFGHIJKLMNOPQRSTUVWXYZ', [1,3,3,2,1,4,2,4,1,8,5,1,3,1,1,3,10,1,1,1,1,4,4,8,4,10]))
# '=' triple word, '-' double word, ';' triple letter, ':' double letter, '*' center (double word)
LAYOUT = ["=..:...=...:..=", ".-...;...;...-.", "..-...:.:...-..", ":..-...:...-..:",
          "....-.....-....", ".;...;...;...;.", "..:...:.:...:..", "=..:...*...:..=",
          "..:...:.:...:..", ".;...;...;...;.", "....-.....-....", ":..-...:...-..:",
          "..-...:.:...-..", ".-...;...;...-.", "=..:...=...:..="]
WMULT = {'-': 2, '*': 2, '=': 3}

# ============================== colors ==============================
BG, FRAME, GRID = (24, 58, 46), (122, 31, 31), (236, 229, 205)
SQUARE = {'.': (205, 197, 163), ':': (166, 214, 236), ';': (40, 124, 191),
          '-': (243, 181, 179), '*': (243, 181, 179), '=': (214, 64, 52)}
PREM_TEXT = {':': ('DOUBLE', 'LETTER', 'SCORE'), ';': ('TRIPLE', 'LETTER', 'SCORE'),
             '-': ('DOUBLE', 'WORD', 'SCORE'), '=': ('TRIPLE', 'WORD', 'SCORE')}
PREM_INK = {':': (25, 70, 100), ';': (255, 255, 255), '-': (130, 40, 40), '=': (255, 255, 255), '*': (130, 40, 40)}
TILE, TILE_EDGE, TILE_SHADOW, TILE_SHINE = (243, 226, 186), (190, 160, 110), (120, 95, 60), (252, 242, 220)
INK, BLANK_INK, NEW_GLOW = (34, 30, 26), (165, 128, 80), (255, 200, 40)
PANEL, PANEL_INK, DIM, HILITE, RULE = (250, 246, 236), (40, 36, 30), (140, 132, 118), (255, 233, 160), (220, 212, 195)
BIG_TAG, POPUP = (180, 60, 40), (214, 64, 52)

# ============================== fonts ==============================
# Inter if installed, else a common system sans-serif, else Pillow's built-in font.
FONT_CANDIDATES = {
    'regular': ['Inter-Medium.otf', 'Inter-Regular.otf', 'Inter-Medium.ttf', 'Inter-Regular.ttf',
                'Helvetica.ttc', 'Arial.ttf', 'arial.ttf', 'DejaVuSans.ttf', 'LiberationSans-Regular.ttf'],
    'bold':    ['Inter-Bold.otf', 'Inter-Bold.ttf', 'Arial Bold.ttf', 'arialbd.ttf',
                'DejaVuSans-Bold.ttf', 'LiberationSans-Bold.ttf', 'Helvetica.ttc'],
    'black':   ['Inter-Black.otf', 'Inter-ExtraBold.otf', 'Inter-Black.ttf', 'Arial Black.ttf', 'ariblk.ttf',
                'Arial Bold.ttf', 'arialbd.ttf', 'DejaVuSans-Bold.ttf', 'LiberationSans-Bold.ttf', 'Helvetica.ttc'],
}
FONT_DIRS = ['/usr/share/fonts', '/usr/local/share/fonts', os.path.expanduser('~/.fonts'),
             os.path.expanduser('~/.local/share/fonts'), os.path.expanduser('~/Library/Fonts'),
             '/Library/Fonts', '/System/Library/Fonts', '/System/Library/Fonts/Supplemental',
             os.path.join(os.environ.get('WINDIR', 'C:\\Windows'), 'Fonts')]

def _index_fonts():
    found = {}
    for d in FONT_DIRS:
        if not os.path.isdir(d): continue
        for root, _, files in os.walk(d):
            for f in files: found.setdefault(f, os.path.join(root, f))
    return found

class Fonts:
    def __init__(self, regular=None, bold=None, black=None):
        index = None
        self.paths = {}
        for weight, override in (('regular', regular), ('bold', bold), ('black', black)):
            if override:
                self.paths[weight] = override; continue
            index = index if index is not None else _index_fonts()
            self.paths[weight] = next((index[n] for n in FONT_CANDIDATES[weight] if n in index), None)
        self.cache = {}
    def __call__(self, weight, size):
        key = (weight, size)
        if key not in self.cache:
            path = self.paths.get(weight)
            try:
                self.cache[key] = ImageFont.truetype(path, size) if path else ImageFont.load_default(size)
            except (OSError, TypeError):
                self.cache[key] = ImageFont.load_default()
        return self.cache[key]

# ============================== reading a game ==============================
def read_game(path):
    """-> list of moves: dict(word, pts, new=[(r, c, letter)], mult), and the BEST total if given."""
    moves, board, best = [], {}, None
    for n, line in enumerate(open(path), 1):
        p = line.split()
        if p and p[0] == 'BEST' and len(p) > 1 and p[1].isdigit(): best = int(p[1])
        if len(p) < 4 or p[0] not in ('A', 'D') or not p[3].isalpha() or 'score=' not in line: continue
        d, r, c, word = p[0], int(p[1]), int(p[2]), p[3]
        pts = int(line.split('score=')[1].split()[0])
        squares = [(r, c + i) if d == 'A' else (r + i, c) for i in range(len(word))]
        if any(not (0 <= rr < N and 0 <= cc < N) for rr, cc in squares):
            sys.exit(f'{path}:{n}: move runs off the board: {line.strip()}')
        new = []
        for (rr, cc), L in zip(squares, word):
            if (rr, cc) in board:
                if board[(rr, cc)].upper() != L.upper():
                    sys.exit(f'{path}:{n}: {word} conflicts with {board[(rr, cc)]} at row {rr} col {cc}')
            else:
                new.append((rr, cc, L))
        if not new: sys.exit(f'{path}:{n}: move places no tiles: {line.strip()}')
        mult = 1
        for rr, cc, L in new: mult *= WMULT.get(LAYOUT[rr][cc], 1)
        for rr, cc, L in new: board[(rr, cc)] = L
        moves.append(dict(word=word, pts=pts, new=new, mult=mult))
    if not moves: sys.exit(f'{path}: no moves found (expected lines like "A  7  6 WORD ... score=70")')
    return moves, best

# ============================== drawing ==============================
class Renderer:
    W, H = 1600, 900
    SQ, GAP = 52, 2
    BX, BY = 46, 44                        # board's top-left corner, inside the frame
    BOARD = N * SQ + (N + 1) * GAP
    ROWH = 24

    def __init__(self, moves, title, subtitle, fonts, big=300):
        self.moves, self.title, self.subtitle, self.big = moves, title, subtitle, big
        F = fonts
        self.f_tile, self.f_val, self.f_prem = F('bold', 30), F('bold', 11), F('bold', 8)
        self.f_title, self.f_sub = F('black', 30), F('regular', 17)
        self.f_head, self.f_row, self.f_rowb = F('bold', 14), F('regular', 17), F('bold', 17)
        self.f_totl, self.f_tot, self.f_pop = F('bold', 20), F('black', 64), F('black', 46)
        self.PX = self.BX + self.BOARD + 55
        self.PY, self.PW = 30, self.W - self.PX - 30
        self.base = self._board_base()

    def sq_xy(self, r, c):
        return self.BX + self.GAP + c * (self.SQ + self.GAP), self.BY + self.GAP + r * (self.SQ + self.GAP)

    def _star(self, d, cx, cy, R, fill):
        import math
        pts = [(cx + (R if i % 2 == 0 else R * 0.42) * math.sin(i * math.pi / 5),
                cy - (R if i % 2 == 0 else R * 0.42) * math.cos(i * math.pi / 5)) for i in range(10)]
        d.polygon(pts, fill=fill)

    def _board_base(self):
        img = Image.new('RGB', (self.W, self.H), BG)
        d = ImageDraw.Draw(img)
        B, X, Y = self.BOARD, self.BX, self.BY
        d.rounded_rectangle([X - 18, Y - 18, X + B + 18, Y + B + 18], 14, fill=FRAME)
        d.rectangle([X, Y, X + B, Y + B], fill=GRID)
        for r in range(N):
            for c in range(N):
                x, y = self.sq_xy(r, c); k = LAYOUT[r][c]; S = self.SQ
                d.rectangle([x, y, x + S - 1, y + S - 1], fill=SQUARE[k])
                if k == '*':
                    self._star(d, x + S / 2, y + S / 2 + 1, S * 0.34, PREM_INK[k])
                elif k in PREM_TEXT:
                    for i, t in enumerate(PREM_TEXT[k]):
                        d.text((x + S / 2, y + 15 + i * 12), t, font=self.f_prem, fill=PREM_INK[k], anchor='mm')
        return img

    def tile(self, img, r, c, L, glow=False, scale=1.0):
        x, y = self.sq_xy(r, c)
        s = self.SQ * scale; ox, oy = x + (self.SQ - s) / 2, y + (self.SQ - s) / 2
        d = ImageDraw.Draw(img)
        if glow: d.rounded_rectangle([ox - 3, oy - 3, ox + s + 2, oy + s + 2], 9, fill=NEW_GLOW)
        d.rounded_rectangle([ox + 1, oy + 2, ox + s, oy + s + 1], 7, fill=TILE_SHADOW)
        d.rounded_rectangle([ox, oy, ox + s - 2, oy + s - 2], 7, fill=TILE, outline=TILE_EDGE, width=1)
        d.line([ox + 5, oy + 2, ox + s - 7, oy + 2], fill=TILE_SHINE, width=2)
        if scale < 0.6: return
        blank = L.islower()                       # a blank: its letter in brown, no point value
        d.text((ox + s / 2 - 3, oy + s / 2 + 1), L.upper(), font=self.f_tile, fill=BLANK_INK if blank else INK, anchor='mm')
        if not blank:
            d.text((ox + s - 6, oy + s - 6), str(VAL[L]), font=self.f_val, fill=INK, anchor='rs')

    def panel(self, img, turn, shown_total):
        d = ImageDraw.Draw(img)
        PX, PY, PW, H = self.PX, self.PY, self.PW, self.H
        d.rounded_rectangle([PX, PY, PX + PW, H - 30], 16, fill=PANEL)
        d.text((PX + 24, PY + 20), self.title, font=self.f_title, fill=PANEL_INK)
        if self.subtitle: d.text((PX + 24, PY + 60), self.subtitle, font=self.f_sub, fill=DIM)
        cols = [PX + 24, PX + 64, PX + 380, PX + 500, PX + PW - 24]
        y = PY + 100
        for x, t, a in zip(cols, ['#', 'WORD', 'TILES', 'POINTS', 'TOTAL'], ['la', 'la', 'ma', 'ra', 'ra']):
            d.text((x + (20 if a == 'ma' else 0), y), t, font=self.f_head, fill=DIM, anchor=a)
        d.line([PX + 20, y + 24, PX + PW - 20, y + 24], fill=RULE, width=2)
        by = H - 30 - 112                          # top of the total box
        rows = max(1, (by - 8 - (y + 32)) // self.ROWH)
        first = max(0, min(turn, len(self.moves) - 1) - rows + 1)   # scroll so the current turn shows
        run = sum(m['pts'] for m in self.moves[:first])
        for i in range(first, min(len(self.moves), first + rows)):
            m = self.moves[i]
            ry = y + 32 + (i - first) * self.ROWH
            if i <= turn: run += m['pts']
            if i == turn: d.rounded_rectangle([PX + 14, ry - 3, PX + PW - 14, ry + self.ROWH - 3], 6, fill=HILITE)
            if i > turn: continue
            f = self.f_rowb if i == turn or m['pts'] >= self.big else self.f_row
            d.text((cols[0], ry), str(i + 1), font=self.f_row, fill=DIM)
            x = cols[1]
            for ch in m['word']:
                d.text((x, ry), ch.upper(), font=f, fill=BLANK_INK if ch.islower() else PANEL_INK)
                x += d.textlength(ch.upper(), font=f) + 1
            tag = ('\u00d727 ' if m['mult'] == 27 else '\u00d79 ' if m['mult'] == 9 else '') + ('bingo' if len(m['new']) == 7 else '')
            if tag: d.text((x + 10, ry + 2), tag, font=self.f_head, fill=BIG_TAG if m['mult'] >= 9 else DIM)
            d.text((cols[2] + 20, ry), str(len(m['new'])), font=self.f_row, fill=DIM, anchor='ma')
            d.text((cols[3], ry), f"{m['pts']:,}", font=f, fill=PANEL_INK, anchor='ra')
            d.text((cols[4], ry), f"{run:,}", font=self.f_row, fill=PANEL_INK, anchor='ra')
        d.rounded_rectangle([PX + 14, by, PX + PW - 14, H - 44], 12, fill=FRAME)
        label = f'TURN {min(turn, len(self.moves) - 1) + 1} OF {len(self.moves)}' if turn >= 0 else 'START'
        d.text((PX + 34, by + 22), label, font=self.f_totl, fill=(250, 225, 200))
        d.text((PX + 34, by + 54), 'TOTAL', font=self.f_totl, fill=(250, 225, 200))
        d.text((PX + PW - 34, by + 66), f'{shown_total:,}', font=self.f_tot, fill=(255, 255, 255), anchor='rm')

    def frame(self, placed, turn, new_shown, shown_total, popup=None):
        img = self.base.copy()
        for (r, c), L in placed.items(): self.tile(img, r, c, L)
        for (r, c, L), sc in new_shown: self.tile(img, r, c, L, glow=True, scale=sc)
        self.panel(img, turn, shown_total)
        if popup:
            text, (r, c) = popup
            x, y = self.sq_xy(r, c)
            d = ImageDraw.Draw(img)
            x = min(max(x + self.SQ / 2, 90), self.BX + self.BOARD - 90)
            y = y - 34 if r > 1 else y + self.SQ + 34
            tw = d.textlength(text, font=self.f_pop)
            d.rounded_rectangle([x - tw / 2 - 16, y - 32, x + tw / 2 + 16, y + 32], 14, fill=POPUP, outline=(255, 255, 255), width=3)
            d.text((x, y), text, font=self.f_pop, fill=(255, 255, 255), anchor='mm')
        return img

def animate(moves, R, fps=10, hold=1.0, big_hold=2.4, end_hold=4.0, start_hold=1.2):
    """Yield (image, number_of_frames) pairs for the whole game."""
    f = lambda seconds: max(1, round(seconds * fps))
    placed, run = {}, 0
    yield R.frame(placed, -1, [], 0), f(start_hold)
    for t, m in enumerate(moves):
        new = m['new']
        for k in range(len(new)):                  # tiles drop in one at a time
            for sc in (0.6, 1.0):
                yield R.frame(placed, t - 1, [(x, 1.0) for x in new[:k]] + [(new[k], sc)], run), 1
        big = m['pts'] >= R.big
        mid = new[len(new) // 2][:2]
        steps = 6 if big else 3                    # count the total up
        popup = (f"+{m['pts']:,}", mid)
        for s in range(1, steps + 1):
            yield R.frame(placed, t, [(x, 1.0) for x in new], run + round(m['pts'] * s / steps), popup), 1
        run += m['pts']
        yield R.frame(placed, t, [(x, 1.0) for x in new], run, popup), f(big_hold if big else hold)
        for r, c, L in new: placed[(r, c)] = L
    yield R.frame(placed, len(moves) - 1, [], run), f(end_hold)

# ============================== writing files ==============================
def write_outputs(frames, fps, out, want_mp4, want_gif, gif_width, use_ffmpeg, crf=20):
    ffmpeg = shutil.which('ffmpeg') if use_ffmpeg else None
    written = []
    if want_mp4 and not ffmpeg:
        print('note: ffmpeg not found, so no MP4 (install ffmpeg for video); writing the GIF only', file=sys.stderr)
        want_mp4 = False; want_gif = True
    if ffmpeg:
        with tempfile.TemporaryDirectory() as tmp:
            n = 0
            for img, k in frames:
                for _ in range(k):
                    img.save(os.path.join(tmp, f'{n:05d}.png'), compress_level=1); n += 1
            src = ['-framerate', str(fps), '-i', os.path.join(tmp, '%05d.png')]
            if want_mp4:
                subprocess.run([ffmpeg, '-y', '-loglevel', 'error', *src, '-c:v', 'libx264', '-preset', 'veryfast',
                                '-crf', str(crf), '-pix_fmt', 'yuv420p', '-movflags', '+faststart', out + '.mp4'], check=True)
                written.append(out + '.mp4')
            if want_gif:
                vf = (f'scale={gif_width}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];'
                      f'[b][p]paletteuse=dither=bayer:bayer_scale=4')
                subprocess.run([ffmpeg, '-y', '-loglevel', 'error', *src, '-vf', vf, '-loop', '0', out + '.gif'], check=True)
                written.append(out + '.gif')
    elif want_gif:                                  # Pillow-only GIF: one frame per distinct image, with durations
        h = round(Renderer.H * gif_width / Renderer.W)
        small = [img.resize((gif_width, h), Image.LANCZOS) for img, _ in frames]
        pal = small[-1].quantize(colors=128, method=Image.Quantize.MEDIANCUT)   # one palette (from the full board)
        imgs = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in small]  # so frames store only changes
        durs = [round(1000 * k / fps) for _, k in frames]
        imgs[0].save(out + '.gif', save_all=True, append_images=imgs[1:], duration=durs, loop=0, optimize=True)
        written.append(out + '.gif')
    return written

# ============================== command line ==============================
def main(argv=None):
    ap = argparse.ArgumentParser(prog='scrabble-video', description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog='Game file format: see the top of scrabble_video.py or README.md.')
    ap.add_argument('game', help='game file (move lines as written by scrabmax)')
    ap.add_argument('-o', '--out', help='output path without extension (default: the game file name)')
    ap.add_argument('--mp4', action='store_true', help='write an MP4 (needs ffmpeg)')
    ap.add_argument('--gif', action='store_true', help='write an animated GIF')
    ap.add_argument('--png', action='store_true', help='also write the final board as a PNG')
    ap.add_argument('--title', default='Highest-scoring Scrabble game', help='scoreboard title')
    ap.add_argument('--subtitle', default='One player \u00b7 100-tile bag', help='line under the title ("" for none)')
    ap.add_argument('--fps', type=int, default=10, help='frames per second (default 10)')
    ap.add_argument('--hold', type=float, default=1.0, help='seconds to pause after each turn (default 1.0)')
    ap.add_argument('--big', type=int, default=300, help='turns worth at least this many points get a longer pause (default 300)')
    ap.add_argument('--gif-width', type=int, default=1000, help='GIF width in pixels (default 1000; the MP4 is 1600x900)')
    ap.add_argument('--no-ffmpeg', action='store_true', help="don't use ffmpeg even if installed (GIF via Pillow)")
    ap.add_argument('--font', help='font file for regular text (default: Inter if installed, else a system sans-serif)')
    ap.add_argument('--font-bold', help='font file for bold text')
    ap.add_argument('--font-black', help='font file for the heaviest text (title, total, popups)')
    ap.add_argument('--version', action='version', version='%(prog)s ' + __version__)
    a = ap.parse_args(argv)
    if not (a.mp4 or a.gif or a.png): a.mp4 = a.gif = True
    out = a.out or os.path.splitext(a.game)[0]

    moves, best = read_game(a.game)
    total = sum(m['pts'] for m in moves)
    if best is not None and best != total:
        print(f'warning: the moves add up to {total}, but the file says BEST {best}', file=sys.stderr)
    fonts = Fonts(a.font, a.font_bold, a.font_black)
    if not all(fonts.paths.values()):
        print("note: some fonts weren't found; using Pillow's built-in font (try --font/--font-bold)", file=sys.stderr)
    R = Renderer(moves, a.title, a.subtitle, fonts, big=a.big)

    written = []
    if a.mp4 or a.gif:
        frames = list(animate(moves, R, fps=a.fps, hold=a.hold))
        secs = sum(k for _, k in frames) / a.fps
        written += write_outputs(frames, a.fps, out, a.mp4, a.gif, a.gif_width, not a.no_ffmpeg)
    else:
        secs = 0
    if a.png:
        placed = {(r, c): L for m in moves for r, c, L in m['new']}
        R.frame(placed, len(moves) - 1, [], total).save(out + '.png'); written.append(out + '.png')
    print(f'{len(moves)} turns, total {total:,}' + (f', {secs:.0f} s of animation' if secs else '') + ':', ', '.join(written))

if __name__ == '__main__':
    main()
