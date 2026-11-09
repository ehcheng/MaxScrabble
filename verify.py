"""Independent verifier for MaxScrabble game files. Replays moves in order,
checks legality (dict, connectivity, tile counts, <=7 tiles/play, board premiums),
and recomputes scores. Usage: python3 verify.py <gamefile> [dict]"""
import sys
from collections import Counter
VAL={c:v for c,v in zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ",[1,3,3,2,1,4,2,4,1,8,5,1,3,1,1,3,10,1,1,1,1,4,4,8,4,10])}
BAG=Counter({'A':9,'B':2,'C':2,'D':4,'E':12,'F':2,'G':3,'H':2,'I':9,'J':1,'K':1,'L':4,'M':2,'N':6,'O':8,'P':2,'Q':1,'R':6,'S':4,'T':6,'U':4,'V':2,'W':2,'X':1,'Y':2,'Z':1,'_':2})
LAYOUT=["=..:...=...:..=",".-...;...;...-.","..-...:.:...-..",":..-...:...-..:","....-.....-....",".;...;...;...;.","..:...:.:...:..","=..:...*...:..=","..:...:.:...:..",".;...;...;...;.","....-.....-....",":..-...:...-..:","..-...:.:...-..",".-...;...;...-.","=..:...=...:..="]
LM=[[2 if ch==':' else 3 if ch==';' else 1 for ch in row] for row in LAYOUT]
WM=[[2 if ch in '-*' else 3 if ch=='=' else 1 for ch in row] for row in LAYOUT]

def load_dict(path):
    d=set()
    for w in open(path):
        w=w.strip().upper()
        if 2<=len(w)<=15 and w.isalpha():
            d.add(w)
    return d

def verify(gamefile, dictpath='enable_web2.txt'):
    D=load_dict(dictpath)
    board=[[None]*15 for _ in range(15)]  # None or (letter, is_blank)
    bag=Counter(BAG)
    total=0
    moves=[]
    lines=open(gamefile).read().splitlines()
    claimed=None
    for ln in lines:
        if ln.startswith('BEST'):
            claimed=int(ln.split()[1])
            continue
        parts=ln.split()
        if len(parts)>=4 and parts[0] in 'AD' and parts[1].isdigit():
            d,r,c,w=parts[0],int(parts[1]),int(parts[2]),parts[3]
            # expected score if present
            exp=None
            for p in parts[4:]:
                if p.startswith('score='): exp=int(p.split('=')[1])
            moves.append((d,r,c,w,exp))
    def word_at(r,c,dr,dc):
        # returns (word_str, cells) going through (r,c)
        r0,c0=r,c
        while 0<=r0-dr<15 and 0<=c0-dc<15 and board[r0-dr][c0-dc] is not None:
            r0-=dr; c0-=dc
        cells=[]
        while 0<=r0<15 and 0<=c0<15 and board[r0][c0] is not None:
            cells.append((r0,c0)); r0+=dr; c0+=dc
        w=''.join(board[rr][cc][0] for rr,cc in cells)
        return w,cells
    for mi,(d,r,c,w,exp) in enumerate(moves):
        # collect new tiles
        new=[]
        for i,ch in enumerate(w):
            rr=r+i if d=='D' else r
            cc=c if d=='D' else c+i
            assert 0<=rr<15 and 0<=cc<15, f"move {mi} out of board"
            if board[rr][cc] is None:
                isblank=ch.islower()
                new.append((rr,cc,ch.upper(),isblank))
            else:
                assert board[rr][cc][0]==ch.upper(), f"move {mi} {w}: board {board[rr][cc]} vs {ch} at {(rr,cc)}"
        assert 1<=len(new)<=7, f"move {mi} {w}: placed {len(new)}"
        # bag check
        for rr,cc,ch,bl in new:
            key='_' if bl else ch
            assert bag[key]>0, f"move {mi}: bag out of {key}"
            bag[key]-=1
        # place tentatively
        for rr,cc,ch,bl in new:
            board[rr][cc]=(ch,bl)
        newset={(rr,cc) for rr,cc,_,_ in new}
        # main direction
        dr,dc=(0,1) if d=='A' else (1,0)
        # find main word
        mw,mcells=word_at(new[0][0],new[0][1],dr,dc)
        assert len(mcells)>=2, f"move {mi} {w}: main word len {len(mcells)}"
        assert mw in D, f"move {mi} {w}: main word {mw} not in dict"
        # all new tiles inside main word
        assert all((rr,cc) in mcells for rr,cc,_,_ in new), f"move {mi}: new tile outside main word"
        # connectivity: first move covers center, else touches existing
        if mi==0:
            assert (7,7) in mcells, f"move 0 must cover center"
        else:
            # must touch a pre-existing tile (not in newset) either in main word or cross
            touches=False
            for rr,cc in mcells:
                if (rr,cc) not in newset: touches=True
            for rr,cc,_,_ in new:
                for ddr,dcc in [(1,0),(-1,0),(0,1),(0,-1)]:
                    nr,nc=rr+ddr,cc+dcc
                    if 0<=nr<15 and 0<=nc<15 and board[nr][nc] is not None and (nr,nc) not in newset:
                        touches=True
            assert touches, f"move {mi} {w}: not connected"
        # score
        def score_word(cells):
            s=0; m=1
            for rr,cc in cells:
                ch,bl=board[rr][cc]
                v=0 if bl else VAL[ch]
                if (rr,cc) in newset:
                    s+=v*LM[rr][cc]; m*=WM[rr][cc]
                else:
                    s+=v
            return s*m
        tot=score_word(mcells)
        # cross words
        cdr,cdc=dc,dr
        for rr,cc,_,_ in new:
            cw,ccells=word_at(rr,cc,cdr,cdc)
            if len(ccells)>=2:
                assert cw in D, f"move {mi} {w}: cross word {cw} at {(rr,cc)} not in dict"
                tot+=score_word(ccells)
        if len(new)==7: tot+=50
        status="OK" if exp is None or exp==tot else f"MISMATCH claimed {exp}"
        print(f"move {mi+1:2d} {d} {r:2d} {c:2d} {w:15s} placed={len(new)} score={tot:5d} {status}  main={mw}")
        assert exp is None or exp==tot, f"move {mi} score mismatch"
        total+=tot
    print(f"\nTOTAL recomputed={total} claimed={claimed} tiles_used={100-sum(bag.values())} leftover={dict((k,v) for k,v in bag.items() if v)}")
    assert claimed is None or claimed==total, "TOTAL MISMATCH"
    assert sum(bag.values())==0, f"tiles left over: {bag}"
    print("VERIFIED: legal game, scores match, all 100 tiles used.")
    return total

if __name__=='__main__':
    verify(sys.argv[1], sys.argv[2] if len(sys.argv)>2 else 'enable_web2.txt')
