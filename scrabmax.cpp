// scrabmax: find a high-scoring single-player Scrabble game where you choose your rack each
// turn from the official 100-tile bag (standard board, 50-point bingo, blanks score 0).
//
// Two stages, sharing one board representation and one move scorer:
//
// 1. PLANNER. Searches for a "skeleton": 15-letter words across rows 0, 7 and 14, joined by
//    8-letter vertical connectors, that can be built as a sequence of legal plays:
//      opening word through the center -> 8-letter middle segment -> middle-row play covering
//      both of its triple-word squares (x9) -> connectors, each a 7-tile bingo hooked onto the
//      middle row -> short words around the connector ends in rows 0 and 14 -> the two edge
//      plays covering three triple-word squares each (x27).
//    The skeleton uses no blanks. Each candidate plan is scored exactly, move by move.
//
// 2. BEAM SEARCH. Plays the game from an empty board. At each state the children are the next
//    plan move, plus the best "free" moves using leftover tiles. Free moves may come before any
//    plan move, but may not use a square the rest of the plan needs, nor tiles it needs. A free
//    move next to a plan square is valued by exactly re-scoring the rest of the plan after it
//    (e.g. a hook under a triple-word square that the x27 play will multiply), and is rejected
//    if it would make a later plan move illegal. States are ranked by
//        points so far + exact value of the rest of the plan from this board.
//    With no plan, this is a plain beam search on points.
//
// usage: scrabmax DICT [-t threads] [-p plans] [-c candidates] [-w width] [-m moves]
//                      [-f planfile] [-P]   > game.txt
//   -t  threads (default: all cores; results don't depend on it)
//   -p  number of best plans to play out (default 20; 0 = no plan)
//   -c  planner candidates for the edge rows (default 1000; the middle row gets 5x; more = slower)
//   -w  beam width per tile count (default 20)
//   -m  free moves kept per state, per number of tiles placed (default 8)
//   -v  estimated points per unplayed leftover tile, used in ranking states (default 10)
//   -H  consider a hook connector only if it's expected to add more than this (default 0)
//   -D  print the hook options found for each skeleton
//   -f  play this plan (move lines as written by -P or in a game file) instead of planning
//   -P  just print the plans found, as move lines, and stop
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
using namespace std;

// ============================== board, letters, dictionary ==============================
const int N = 15;
int VAL[26] = {1,3,3,2,1,4,2,4,1,8,5,1,3,1,1,3,10,1,1,1,1,4,4,8,4,10};
//              A B C D  E F G H I J K L M N O P Q  R S T U V W X Y Z  blank
int BAG0[27] = {9,2,2,4,12,2,3,2,9,1,1,4,2,6,8,2,1, 6,4,6,4,2,2,1,2,1, 2};
const char* LAYOUT[N] = {
 "=..:...=...:..=", ".-...;...;...-.", "..-...:.:...-..", ":..-...:...-..:",
 "....-.....-....", ".;...;...;...;.", "..:...:.:...:..", "=..:...*...:..=",
 "..:...:.:...:..", ".;...;...;...;.", "....-.....-....", ":..-...:...-..:",
 "..-...:.:...-..", ".-...;...;...-.", "=..:...=...:..="};
int LM[N][N], WM[N][N];
static inline int UP(char c){ return c>='a'? c-'a' : c-'A'; }      // letter index of a tile
static inline int TV(char c){ return c>='a'? 0 : VAL[c-'A']; }     // tile value (blanks are lowercase)

struct TNode { int ch[26]; bool term; };
vector<TNode> T;
int newnode(){ T.push_back(TNode()); memset(T.back().ch,-1,sizeof T.back().ch); T.back().term=false; return T.size()-1; }
void addword(const string& w){ int n=0; for(char c:w){ int L=c-'A'; if(T[n].ch[L]<0){ int m=newnode(); T[n].ch[L]=m; } n=T[n].ch[L]; } T[n].term=true; }
int walk(int n,const char* s,int len){ for(int i=0;i<len&&n>=0;i++) n=T[n].ch[UP(s[i])]; return n; }
bool isword(const char* s,int len){ int n=walk(0,s,len); return n>=0 && T[n].term; }
bool isword(const string& s){ return isword(s.data(),s.size()); }
vector<string> BYLEN[N+1];

struct Board { char sq[N][N]; };
struct Bag { signed char c[27]; int total() const { int t=0; for(int i=0;i<27;i++) t+=c[i]; return t; } };
struct Tile { int r,c; char L; };
struct Move { signed char dir,line,start,len,placed; int score; int future; float key; char letters[16]; };

string movestr(const Move& m){
  char buf[80]; int r=m.dir==0? m.line:m.start, c=m.dir==0? m.start:m.line;
  snprintf(buf,80,"%c %2d %2d %-15s placed=%d score=%d",m.dir==0?'A':'D',r,c,m.letters,m.placed,m.score); return buf; }
void print_board(FILE* f,const Board& b){ for(int r=0;r<N;r++){ for(int c=0;c<N;c++) fputc(b.sq[r][c]?b.sq[r][c]:'.',f); fputc('\n',f); } }
bool board_empty(const Board& b){ for(int r=0;r<N;r++) for(int c=0;c<N;c++) if(b.sq[r][c]) return false; return true; }

// ============================== the shared move scorer ==============================
// Score placing `tiles` (all in one row or column) on b. Returns -1 if the play is illegal
// (squares taken, gaps, not connected, or any word formed is not in the dictionary).
// If `apply`, the tiles are left on the board. Also reports the main word as a Move.
int score_tiles(Board& b,const vector<Tile>& tiles,bool apply,Move* out=nullptr){
  if(tiles.empty() || tiles.size()>7) return -1;
  bool first=board_empty(b);
  for(auto& t:tiles) if(b.sq[t.r][t.c]) return -1;
  int dr=0,dc=1;                                          // main direction
  bool sameRow=true, sameCol=true;
  for(auto& t:tiles){ if(t.r!=tiles[0].r) sameRow=false; if(t.c!=tiles[0].c) sameCol=false; }
  if(tiles.size()>1){ if(sameRow) {dr=0;dc=1;} else if(sameCol) {dr=1;dc=0;} else return -1; }
  else { int r=tiles[0].r,c=tiles[0].c; bool h=(c>0&&b.sq[r][c-1])||(c<N-1&&b.sq[r][c+1]); if(!h){dr=1;dc=0;} }
  for(auto& t:tiles) b.sq[t.r][t.c]=t.L;
  auto undo=[&](){ for(auto& t:tiles) b.sq[t.r][t.c]=0; };
  bool isnew[N][N]={}; for(auto& t:tiles) isnew[t.r][t.c]=true;
  bool touches=false, center=false;
  auto word=[&](int r,int c,int dr,int dc,int& len)->int{    // score of the word through (r,c), or -1 if not a word
    while(r-dr>=0&&c-dc>=0&&b.sq[r-dr][c-dc]){ r-=dr; c-=dc; }
    char w[N]; int s=0,m=1; len=0;
    while(r<N&&c<N&&b.sq[r][c]){ char ch=b.sq[r][c]; w[len++]=ch;
      if(isnew[r][c]){ s+=TV(ch)*LM[r][c]; m*=WM[r][c]; if(r==7&&c==7) center=true; } else { s+=TV(ch); touches=true; }
      r+=dr; c+=dc; }
    if(len<2) return 0;
    return isword(w,len)? s*m : -1;
  };
  int len, total=word(tiles[0].r,tiles[0].c,dr,dc,len);
  int mainlen=len;
  // all tiles must lie inside the main word
  int lo=N,hi=-1; for(auto& t:tiles){ int p=dr?t.r:t.c; lo=min(lo,p); hi=max(hi,p); }
  for(int p=lo;p<=hi;p++){ int r=dr?p:tiles[0].r, c=dr?tiles[0].c:p; if(!b.sq[r][c]){ undo(); return -1; } }
  if(total<0 || (mainlen<2)){ undo(); return -1; }
  for(auto& t:tiles){ int l2; int x=word(t.r,t.c,dc,dr,l2); if(x<0){ undo(); return -1; } total+=x; }
  if(first? !center : !touches){ undo(); return -1; }
  if(tiles.size()==7) total+=50;
  if(out){ int r=tiles[0].r,c=tiles[0].c; while(r-dr>=0&&c-dc>=0&&b.sq[r-dr][c-dc]){ r-=dr; c-=dc; }
    out->dir= dr?1:0; out->line= dr?c:r; out->start= dr?r:c; out->len=mainlen; out->placed=tiles.size(); out->score=total;
    for(int i=0;i<mainlen;i++) out->letters[i]=b.sq[r+dr*i][c+dc*i]; out->letters[mainlen]=0; }
  if(!apply) undo();
  return total;
}

// ============================== free-move generator ==============================
struct LineInfo { char c[N]; unsigned mask[N]; int csum[N]; bool hasx[N]; int lm[N], wm[N]; };
// For one row (dir 0) or column (dir 1): letters, allowed letters per empty square (from cross
// words), cross-word letter sums. Squares in `resv` (needed by the plan) are blocked.
void line_info(const Board& b,const bool resv[N][N],int dir,int line,LineInfo& li){
  for(int i=0;i<N;i++){
    int r=dir==0?line:i, col=dir==0?i:line;
    li.c[i]=b.sq[r][col]; li.lm[i]=LM[r][col]; li.wm[i]=WM[r][col];
    li.mask[i]=0; li.csum[i]=0; li.hasx[i]=false;
    if(li.c[i]||resv[r][col]) continue;
    int dr=dir==0?1:0, dc=dir==0?0:1;
    char up[N],dn[N]; int nu=0,nd=0;
    int rr=r-dr,cc=col-dc; while(rr>=0&&cc>=0&&b.sq[rr][cc]){ up[nu++]=b.sq[rr][cc]; rr-=dr; cc-=dc; }
    reverse(up,up+nu);
    rr=r+dr; cc=col+dc; while(rr<N&&cc<N&&b.sq[rr][cc]){ dn[nd++]=b.sq[rr][cc]; rr+=dr; cc+=dc; }
    if(nu+nd==0){ li.mask[i]=(1u<<26)-1; continue; }
    li.hasx[i]=true;
    int s=0; for(int k=0;k<nu;k++) s+=TV(up[k]); for(int k=0;k<nd;k++) s+=TV(dn[k]); li.csum[i]=s;
    int n=walk(0,up,nu); if(n<0) continue;
    for(int L=0;L<26;L++){ int m=T[n].ch[L]; if(m<0) continue; m=walk(m,dn,nd); if(m>=0&&T[m].term) li.mask[i]|=1u<<L; }
  }
}
// Enumerate every legal move (respecting the bag; a blank is used only when no real tile is left)
// and pass each to `sink`.
struct Gen {
  const LineInfo* li; int dir,line,start; bool first; char w[16]; Bag bag;
  const function<void(const Move&)>* sink;
  void dfs(int pos,int node,int placed,bool conn,int msum,int mmul,int xtot,int firstpos){
    if(pos>=N) return;
    const LineInfo& L=*li;
    if(L.c[pos]){
      int l=UP(L.c[pos]); int m=T[node].ch[l]; if(m<0) return;
      w[pos-start]=L.c[pos];
      step(pos,m,placed,true,msum+TV(L.c[pos]),mmul,xtot,firstpos);
    } else {
      if(placed==7) return;
      unsigned mk=L.mask[pos];
      bool c2=conn||L.hasx[pos]||(first&&line==7&&pos==7);
      for(int l=0;l<26;l++){ if(!(mk>>l&1)) continue; int m=T[node].ch[l]; if(m<0) continue;
        bool real=bag.c[l]>0; if(!real&&bag.c[26]<=0) continue;
        int idx=real?l:26; bag.c[idx]--;
        int lv=real?VAL[l]*L.lm[pos]:0;
        int x=L.hasx[pos]?(L.csum[pos]+lv)*L.wm[pos]:0;
        w[pos-start]=real?'A'+l:'a'+l;
        step(pos,m,placed+1,c2,msum+lv,mmul*L.wm[pos],xtot+x,placed==0?pos:firstpos);
        bag.c[idx]++;
      }
    }
  }
  void step(int pos,int m,int placed,bool conn,int msum,int mmul,int xtot,int firstpos){
    const LineInfo& L=*li;
    bool endok=(pos+1==N)||!L.c[pos+1];
    int len=pos-start+1;
    if(endok&&T[m].term&&conn&&placed>=1&&len>=2){
      bool dup=(dir==1&&placed==1&&L.hasx[firstpos]);   // a lone tile is generated across already
      if(!dup){ Move mv; mv.dir=dir; mv.line=line; mv.start=start; mv.len=len; mv.placed=placed;
        mv.score=msum*mmul+xtot+(placed==7?50:0); mv.future=0; mv.key=mv.score;
        memcpy(mv.letters,w,len); mv.letters[len]=0; (*sink)(mv); }
    }
    dfs(pos+1,m,placed,conn,msum,mmul,xtot,firstpos);
  }
};
void gen_moves(const Board& b,const Bag& bag,const bool resv[N][N],const function<void(const Move&)>& sink){
  bool first=board_empty(b);
  Gen g; g.first=first; g.bag=bag; g.sink=&sink;
  LineInfo li;
  for(int dir=0;dir<2;dir++) for(int line=0;line<N;line++){
    if(first&&line!=7) continue;
    line_info(b,resv,dir,line,li);
    bool any=first; for(int i=0;i<N;i++) if(li.c[i]||li.hasx[i]) any=true; if(!any) continue;
    g.li=&li; g.dir=dir; g.line=line;
    for(int s=0;s<N;s++){
      if(s>0&&li.c[s-1]) continue;
      int k=0; bool ok=false;
      for(int i=s;i<N;i++){ if(li.c[i]){ ok=true; break; } if(li.hasx[i]||(first&&i==7)){ ok=true; break; } k++; if(k>=7) break; }
      if(!ok) continue;
      g.start=s; g.dfs(s,0,0,false,0,1,0,-1);
    }
  }
}
vector<Tile> move_tiles(const Board& b,const Move& m){
  vector<Tile> t; for(int i=0;i<m.len;i++){ int r=m.dir==0?m.line:m.start+i, c=m.dir==0?m.start+i:m.line; if(!b.sq[r][c]) t.push_back({r,c,m.letters[i]}); }
  return t; }
void apply_tiles(Board& b,Bag& bag,const vector<Tile>& t){ for(auto& x:t){ b.sq[x.r][x.c]=x.L; bag.c[x.L>='a'?26:x.L-'A']--; } }

// ============================== planner ==============================
struct Counts { int c[26]; };
Counts counts(const string& s){ Counts k{}; for(char ch:s) k.c[ch-'A']++; return k; }
Counts operator+(Counts a,const Counts& b){ for(int i=0;i<26;i++) a.c[i]+=b.c[i]; return a; }
bool fits(const Counts& k){ for(int i=0;i<26;i++) if(k.c[i]>BAG0[i]) return false; return true; }
int ntiles(const Counts& k){ int t=0; for(int i=0;i<26;i++) t+=k.c[i]; return t; }

int big_value(const string& w,int row,const bool isnew[N]){
  int s=0,m=1; for(int i=0;i<N;i++){ int v=VAL[w[i]-'A']; if(isnew[i]){ s+=v*LM[row][i]; m*=WM[row][i]; } else s+=v; }
  return s*m+50; }
struct EdgePat { int v; string w; vector<vector<int>> segs; };
// Middle row: 8 letters are pre-placed so a 7-tile play covers both end triple-word squares (x9).
// They form a center piece s0lo..s0hi through the star (opened by a sub-word, then extended),
// plus up to two other pieces ("extras"), each a word or a single letter, which are fed by a
// reverse connector coming down from the top row (or up from the bottom row).
struct MidPat { int v; string w; int s0lo, s0hi; vector<pair<int,int>> openings; vector<vector<int>> extras; };
// Ways to pre-place 8 letters of an edge-row word so a 7-tile play covers all three triple-word
// squares; each run of pre-placed letters must be a word (or a single letter). Best 8 per word.
vector<EdgePat> edge_patterns(const string& w){
  vector<EdgePat> out; int fr[12]={1,2,3,4,5,6,8,9,10,11,12,13};
  for(int a=0;a<12;a++) for(int b=a+1;b<12;b++) for(int c=b+1;c<12;c++) for(int d=c+1;d<12;d++){
    bool isnew[N]={}; isnew[0]=isnew[7]=isnew[14]=true; isnew[fr[a]]=isnew[fr[b]]=isnew[fr[c]]=isnew[fr[d]]=true;
    vector<vector<int>> segs; vector<int> cur; bool ok=true;
    for(int i=0;i<=N;i++){
      if(i<N&&!isnew[i]) { cur.push_back(i); continue; }
      if(!cur.empty()){ if(cur.size()>1&&!isword(w.substr(cur[0],cur.size()))) ok=false; segs.push_back(cur); cur.clear(); }
    }
    if(ok) out.push_back({big_value(w,0,isnew),w,segs});
  }
  stable_sort(out.begin(),out.end(),[](const EdgePat& x,const EdgePat& y){ return x.v>y.v; });
  if(out.size()>8) out.resize(8);
  return out;
}
vector<MidPat> middle_patterns(const string& w){
  vector<MidPat> out; int fr[12]={1,2,3,4,5,6,8,9,10,11,12,13};
  for(int a=0;a<12;a++) for(int b=a+1;b<12;b++) for(int c=b+1;c<12;c++) for(int d=c+1;d<12;d++) for(int e=d+1;e<12;e++){
    bool isnew[N]={}; isnew[0]=isnew[14]=true; for(int k:{a,b,c,d,e}) isnew[fr[k]]=true;
    vector<vector<int>> segs; vector<int> cur; bool ok=true;
    for(int i=0;i<=N&&ok;i++){
      if(i<N&&!isnew[i]){ cur.push_back(i); continue; }
      if(!cur.empty()){ if(cur.size()>1&&!isword(w.substr(cur[0],cur.size()))) ok=false; segs.push_back(cur); cur.clear(); }
    }
    if(!ok) continue;
    MidPat p; p.w=w; p.s0lo=-1;
    for(auto& sg:segs){ if(sg.front()<=7&&7<=sg.back()){ p.s0lo=sg.front(); p.s0hi=sg.back(); } else p.extras.push_back(sg); }
    if(p.s0lo<0||p.s0hi==p.s0lo||p.extras.size()>2) continue;
    for(int i=p.s0lo;i<=7;i++) for(int j=7;j<=p.s0hi;j++) if(j-i+1>=2&&j-i+1<=7&&isword(w.substr(i,j-i+1))) p.openings.push_back({i,j});
    if(p.openings.empty()) continue;
    p.v=big_value(w,7,isnew);
    out.push_back(p);
  }
  stable_sort(out.begin(),out.end(),[](const MidPat& x,const MidPat& y){ return x.v>y.v; });
  if(out.size()>10) out.resize(10);
  return out;
}
map<pair<char,char>,vector<string>> CONN;   // (top letter, bottom letter) -> 8-letter words
// Hook connectors: an 8-letter column word whose end letter is a NEW tile of an edge-row play.
// Top hook: rows 1-7 (w[1..7], a 7-letter word ending on the middle row) is played first;
// the top-row play's tile at row 0 completes w. Bottom hook: rows 7-13 (w[0..6]), completed by
// the bottom-row tile at row 14. The completed word is a cross word of the x27 play, so on a
// triple-word column (0, 7 or 14) it is tripled.
map<pair<char,char>,vector<string>> HUP, HDN;   // (edge-row letter or middle letter, ...) -> words
double TILEVAL=10;   // estimated points per tile not used by the plan (also used to rank states)
double HOOKMIN=-1;   // add a hook only if it's expected to add more than this (default 6*TILEVAL)
bool HOOKDEBUG=false;
// An 8-letter connector in column col spans rows 0-7 (up) or 7-14 (down). A forward connector is
// played from the middle row (new tiles rows 0-6 / 8-14); a reverse one from the edge row (new
// tiles rows 1-7 / 7-13), which puts a letter into the middle row before the x9 play.
int connector_value(const string& w,int col,bool up,bool rev=false){
  int r0 = up? (rev?1:0) : (rev?7:8);              // first new row
  int k0 = up? r0 : r0-7;                          // index in w of that row's letter
  int s=0,m=1;
  for(int k=0;k<7;k++){ int r=r0+k; s+=VAL[w[k0+k]-'A']*LM[r][col]; m*=WM[r][col]; }
  s+=VAL[w[k0==0?7:0]-'A'];                        // the letter already on the board
  return s*m+50; }
vector<Tile> connector_tiles(const string& w,int col,bool up,bool rev){
  int r0 = up? (rev?1:0) : (rev?7:8), k0 = up? r0 : r0-7; vector<Tile> t;
  for(int k=0;k<7;k++) t.push_back({r0+k,col,w[k0+k]});
  return t; }
struct Conn { int col; string w; bool up; bool rev; int extra; int gseg; };   // extra: which middle piece it serves (-1: none)
// Choose connectors: for each middle extra, a forward connector from the center piece up (or down)
// to an edge-row piece, and a reverse connector from that piece back to the extra; then one forward
// connector for every other edge-row piece. Columns distinct, letters fit the bag; maximize points.
bool choose_connectors(const EdgePat& Tp,const MidPat& Mp,const EdgePat& Bp,Counts used,int limit,vector<Conn>& best,int& bestpts,
                       const bool* forbidUp=nullptr,const bool* forbidDn=nullptr){
  const string& M=Mp.w;
  bestpts=-1; vector<Conn> chosen; bool colused[N]={};
  bool anchoredT[8]={}, anchoredB[8]={};
  auto words=[&](int c,bool up,bool rev,const Counts& u,vector<pair<int,const string*>>& out){
    auto it=CONN.find(up? make_pair(Tp.w[c],M[c]) : make_pair(M[c],Bp.w[c])); if(it==CONN.end()) return;
    for(auto& w:it->second) if(fits(u+counts(w.substr(1,6)))) out.push_back({connector_value(w,c,up,rev),&w});
    sort(out.begin(),out.end(),[](auto& x,auto& y){ return x.first>y.first; });
    if((int)out.size()>limit) out.resize(limit);
  };
  auto free_col=[&](int c,bool up){ const bool* fb=up?forbidUp:forbidDn; return !colused[c]&&!(fb&&fb[c]); };
  // normal tasks: edge pieces not anchored by an extra
  function<void(int,bool,Counts,int)> dfs_norm=[&](int k,bool up,Counts used,int pts){
    const EdgePat& P=up?Tp:Bp; bool* anch=up?anchoredT:anchoredB;
    if(k==(int)P.segs.size()){ if(up){ dfs_norm(0,false,used,pts); return; }
      if(pts>bestpts){ bestpts=pts; best=chosen; } return; }
    if(anch[k]){ dfs_norm(k+1,up,used,pts); return; }
    vector<tuple<int,int,const string*>> opts;
    for(int c:P.segs[k]){ if(!free_col(c,up)) continue; vector<pair<int,const string*>> ws; words(c,up,false,used,ws);
      for(auto& [v,w]:ws) opts.push_back({v,c,w}); }
    sort(opts.begin(),opts.end(),[](auto& x,auto& y){ return get<0>(x)>get<0>(y); });
    if((int)opts.size()>limit) opts.resize(limit);
    for(auto& [v,c,w]:opts){ colused[c]=true; chosen.push_back({c,*w,up,false,-1,k});
      dfs_norm(k+1,up,used+counts(w->substr(1,6)),pts+v); chosen.pop_back(); colused[c]=false; }
  };
  // extras first
  function<void(size_t,Counts,int)> dfs_extra=[&](size_t j,Counts used,int pts){
    if(j==Mp.extras.size()){ dfs_norm(0,true,used,pts); return; }
    const vector<int>& E=Mp.extras[j];
    struct Opt { int v; bool up; int g,a,b; const string *wa,*wb; };
    vector<Opt> opts;
    for(int up=1;up>=0;up--){
      const EdgePat& P=up?Tp:Bp; bool* anch=up?anchoredT:anchoredB;
      for(size_t g=0;g<P.segs.size();g++){ if(anch[g]) continue; auto& G=P.segs[g];
        for(int a:G){ if(a<Mp.s0lo||a>Mp.s0hi||!free_col(a,up)) continue;
          for(int b:G){ if(abs(a-b)<2||find(E.begin(),E.end(),b)==E.end()||!free_col(b,up)) continue;
            vector<pair<int,const string*>> wa,wb; words(a,up,false,used,wa); words(b,up,true,used,wb);
            for(auto& [va,pa]:wa) for(auto& [vb,pb]:wb) if(fits(used+counts(pa->substr(1,6))+counts(pb->substr(1,6))))
              opts.push_back({va+vb,(bool)up,(int)g,a,b,pa,pb}); } } } }
    sort(opts.begin(),opts.end(),[](auto& x,auto& y){ return x.v>y.v; });
    if((int)opts.size()>limit) opts.resize(limit);
    for(auto& o:opts){ bool* anch=o.up?anchoredT:anchoredB;
      anch[o.g]=true; colused[o.a]=colused[o.b]=true;
      chosen.push_back({o.a,*o.wa,o.up,false,(int)j,o.g}); chosen.push_back({o.b,*o.wb,o.up,true,(int)j,o.g});
      dfs_extra(j+1,used+counts(o.wa->substr(1,6))+counts(o.wb->substr(1,6)),pts+o.v);
      chosen.pop_back(); chosen.pop_back(); anch[o.g]=false; colused[o.a]=colused[o.b]=false; }
  };
  dfs_extra(0,used,0);
  return bestpts>=0;
}
struct Plan { int total; int big; int ntiles; int nhooks; vector<vector<Tile>> moves; vector<Move> shown; string desc;
  double value() const { return total + TILEVAL*(100-ntiles); } };   // plan points + leftover estimate
struct Hook { int col; string w; bool up; int est; };
// Points a hook is expected to add: its own 6-tile play plus its cross word in the edge play.
int hook_value(const string& w,int col,bool up){
  int s=0,m=1;
  for(int k=1;k<=6;k++){ int r=up?k:7+k; s+=VAL[w[k]-'A']*LM[r][col]; m*=WM[r][col]; }
  s+=VAL[(up?w[7]:w[0])-'A'];                       // the middle-row letter
  int play=s*m;
  int er=up?0:14; char e=up?w[0]:w[7]; int x=0;
  for(int k=0;k<8;k++) x+=VAL[w[k]-'A'];
  x+=VAL[e-'A']*(LM[er][col]-1);
  return play + x*WM[er][col];
}
// Turn a skeleton into sequences of legal plays, scored exactly (best opening word for each);
// one variant per number of hook connectors added greedily (0, 1, 2, ...).
vector<Plan> build_plan(const EdgePat& Tp,const MidPat& Mp,const EdgePat& Bp){
  vector<Plan> res;
  Counts base=counts(Tp.w)+counts(Mp.w)+counts(Bp.w);
  bool preT[N]={}, preB[N]={};
  for(auto& sg:Tp.segs) for(int c:sg) preT[c]=true;
  for(auto& sg:Bp.segs) for(int c:sg) preB[c]=true;
  // hook options (best word per column and half), on the edge plays' new squares
  vector<Hook> opts;
  for(int up=1;up>=0;up--) for(int c=0;c<N;c++){
    if(up? preT[c] : preB[c]) continue;
    auto key = up? make_pair(Tp.w[c],Mp.w[c]) : make_pair(Mp.w[c],Bp.w[c]);
    auto& mp = up? HUP : HDN; auto it=mp.find(key); if(it==mp.end()) continue;
    Hook best{c,"",(bool)up,-1};
    for(auto& w:it->second) if(fits(base+counts(w.substr(1,6)))){ int v=hook_value(w,c,up); if(v>best.est){ best.w=w; best.est=v; } }
    if(best.est>(HOOKMIN>=0?HOOKMIN:0)) opts.push_back(best);
  }
  sort(opts.begin(),opts.end(),[](auto& a,auto& b){ return make_tuple(-a.est,a.col,a.up)<make_tuple(-b.est,b.col,b.up); });
  if(opts.size()>8) opts.resize(8);
  if(HOOKDEBUG){ fprintf(stderr,"%s / %s / %s:",Tp.w.c_str(),Mp.w.c_str(),Bp.w.c_str());
    for(auto& h:opts) fprintf(stderr,"  %s@%d%s=%d",h.w.c_str(),h.col,h.up?"up":"dn",h.est); fprintf(stderr,"\n"); }
  // hook sets to try: none, each single hook, each compatible pair
  vector<vector<Hook>> sets{{}};
  auto compatible=[](const Hook& a,const Hook& b){ return a.col!=b.col && !(a.up==b.up && abs(a.col-b.col)<=1); };
  for(size_t i=0;i<opts.size();i++){ sets.push_back({opts[i]});
    for(size_t j=i+1;j<opts.size();j++) if(compatible(opts[i],opts[j])&&fits(base+counts(opts[i].w.substr(1,6))+counts(opts[j].w.substr(1,6)))) sets.push_back({opts[i],opts[j]}); }
  for(auto& hooks:sets){
    Counts used=base; bool fU[N]={}, fD[N]={};
    for(auto& h:hooks){ used=used+counts(h.w.substr(1,6));
      fU[h.col]=fD[h.col]=true; bool* f=h.up?fU:fD; if(h.col>0) f[h.col-1]=true; if(h.col<N-1) f[h.col+1]=true; }
    if(!fits(used)) continue;
    vector<Conn> conn; int cp;
    if(!choose_connectors(Tp,Mp,Bp,used,6,conn,cp,fU,fD)) continue;
    bool found=false; Plan bestp;
    for(auto [i,j]:Mp.openings){
      Board b; memset(b.sq,0,sizeof b.sq); Plan p; p.total=0; p.big=Tp.v+Mp.v+Bp.v; p.ntiles=0; p.nhooks=hooks.size(); bool ok=true;
      auto mv=[&](vector<Tile> t){ if(!ok||t.empty()) return; Move m; int s=score_tiles(b,t,true,&m);
        if(s<0){ ok=false; return; } p.total+=s; p.ntiles+=t.size(); p.moves.push_back(t); p.shown.push_back(m); };
      const string& M=Mp.w; vector<Tile> t;
      for(int c=i;c<=j;c++) t.push_back({7,c,M[c]}); mv(t); t.clear();                       // opening
      for(int c=Mp.s0lo;c<=Mp.s0hi;c++) if(c<i||c>j) t.push_back({7,c,M[c]}); mv(t); t.clear();  // extend center piece
      for(size_t e=0;e<Mp.extras.size();e++){                                                 // feed each extra piece
        for(auto& k:conn) if(k.extra==(int)e&&!k.rev){ mv(connector_tiles(k.w,k.col,k.up,false));
          int row=k.up?0:14; const EdgePat& P=k.up?Tp:Bp;
          for(int c:P.segs[k.gseg]) if(!b.sq[row][c]) t.push_back({row,c,P.w[c]}); mv(t); t.clear(); }
        for(auto& k:conn) if(k.extra==(int)e&&k.rev) mv(connector_tiles(k.w,k.col,k.up,true));
        for(int c:Mp.extras[e]) if(!b.sq[7][c]) t.push_back({7,c,M[c]}); mv(t); t.clear(); }
      for(int c=0;c<N;c++) if(!b.sq[7][c]) t.push_back({7,c,M[c]}); mv(t); t.clear();          // middle x9
      for(auto& k:conn) if(k.extra<0) mv(connector_tiles(k.w,k.col,k.up,false));
      for(auto& k:hooks){ for(int r=1;r<=6;r++){ int rr=k.up?r:7+r; t.push_back({rr,k.col,k.w[r]}); } mv(t); t.clear(); }
      for(auto [row,Pp]:{make_pair(0,&Tp),make_pair(14,&Bp)})
        for(auto& sg:Pp->segs){ for(int c:sg) if(!b.sq[row][c]) t.push_back({row,c,Pp->w[c]}); mv(t); t.clear(); }
      for(auto [row,W]:{make_pair(0,&Tp.w),make_pair(14,&Bp.w)}){ for(int c=0;c<N;c++) if(!b.sq[row][c]) t.push_back({row,c,(*W)[c]}); mv(t); t.clear(); }
      if(ok&&(!found||p.total>bestp.total)){
        p.desc=Tp.w+" / "+Mp.w+" / "+Bp.w;
        for(auto& h:hooks) p.desc+="  +"+h.w+(h.up?" (up, col ":" (down, col ")+to_string(h.col)+")";
        bestp=p; found=true; }
    }
    if(found) res.push_back(bestp);
  }
  return res;
}
vector<Plan> find_plans(int ncand,int nplans,int threads){
  vector<string> w15; for(auto& w:BYLEN[15]) if(fits(counts(w))) w15.push_back(w);
  vector<EdgePat> edge; vector<MidPat> mid;
  { mutex mu; vector<thread> pool;
    for(int t=0;t<threads;t++) pool.emplace_back([&,t]{ vector<EdgePat> e; vector<MidPat> m;
      for(size_t i=t;i<w15.size();i+=threads){ for(auto& p:edge_patterns(w15[i])) e.push_back(p); for(auto& p:middle_patterns(w15[i])) m.push_back(p); }
      lock_guard<mutex> g(mu); edge.insert(edge.end(),e.begin(),e.end()); mid.insert(mid.end(),m.begin(),m.end()); });
    for(auto& th:pool) th.join(); }
  sort(edge.begin(),edge.end(),[](auto& x,auto& y){ return make_tuple(-x.v,x.w,x.segs)<make_tuple(-y.v,y.w,y.segs); });
  sort(mid.begin(),mid.end(),[](auto& x,auto& y){ return make_tuple(-x.v,x.w,x.s0lo,x.s0hi,x.extras)<make_tuple(-y.v,y.w,y.s0lo,y.s0hi,y.extras); });
  if((int)edge.size()>ncand) edge.resize(ncand);
  if((int)mid.size()>5*ncand) mid.resize(5*ncand);        // the middle row has many more layouts now
  fprintf(stderr,"planner: %zu 15-letter words fit the bag; %zu edge and %zu middle candidates\n",w15.size(),edge.size(),mid.size());
  if(edge.empty()||mid.empty()) return {};
  // best skeletons by big-play value: one middle word per (top, bottom) pair
  const int KEEP=max(150,5*nplans);
  struct Res { int v; int ti,mi,bi; };
  vector<Res> res; mutex mu; atomic<int> bar(0);
  int midbest=mid[0].v;
  vector<thread> pool; atomic<int> next(0);
  for(int t=0;t<threads;t++) pool.emplace_back([&]{
    for(int ti; (ti=next++)<(int)edge.size(); ){
      auto& Tp=edge[ti]; if(2*Tp.v+midbest<=bar) break;
      Counts cT=counts(Tp.w);
      for(size_t bi=0;bi<edge.size();bi++){ auto& Bp=edge[bi];
        if(Tp.v+Bp.v+midbest<=bar) break;
        Counts cTB=cT+counts(Bp.w); if(!fits(cTB)) continue;
        for(size_t mi=0;mi<mid.size();mi++){ auto& Mp=mid[mi];
          if(Tp.v+Bp.v+Mp.v<=bar) break;
          Counts used=cTB+counts(Mp.w);
          if(!fits(used)||ntiles(used)+6*(int)(Tp.segs.size()+Bp.segs.size()+Mp.extras.size())>100) continue;
          vector<Conn> conn; int cp;
          if(!choose_connectors(Tp,Mp,Bp,used,2,conn,cp)) continue;
          lock_guard<mutex> g(mu);
          res.push_back({Tp.v+Bp.v+Mp.v,ti,(int)mi,(int)bi});
          sort(res.begin(),res.end(),[](auto& x,auto& y){ return x.v>y.v || (x.v==y.v && make_tuple(x.ti,x.bi,x.mi)<make_tuple(y.ti,y.bi,y.mi)); });
          if((int)res.size()>KEEP) res.resize(KEEP);
          if((int)res.size()==KEEP) bar=res.back().v;
          break;
        }
      }
    } });
  for(auto& th:pool) th.join();
  // build and exactly score each skeleton
  vector<vector<Plan>> plans(res.size());
  { vector<thread> pool; atomic<int> nx(0);
    for(int t=0;t<threads;t++) pool.emplace_back([&]{ for(int i;(i=nx++)<(int)res.size();) plans[i]=build_plan(edge[res[i].ti],mid[res[i].mi],edge[res[i].bi]); });
    for(auto& th:pool) th.join(); }
  vector<Plan> out; for(auto& v:plans) for(auto& p:v) out.push_back(p);
  stable_sort(out.begin(),out.end(),[](auto& x,auto& y){ return x.value()>y.value(); });
  if((int)out.size()>nplans) out.resize(nplans);
  return out;
}
// Read a plan (or a whole game) written as move lines "A  7  6 UNCTION ..." into tile lists.
Plan read_plan(const char* file){
  Plan p; p.total=0; p.big=0; p.ntiles=0; p.nhooks=0; p.desc=file; Board b; memset(b.sq,0,sizeof b.sq);
  ifstream f(file); string line;
  while(getline(f,line)){
    char d; int r,c; char w[32];
    if(sscanf(line.c_str()," %c %d %d %31s",&d,&r,&c,w)!=4||(d!='A'&&d!='D')) continue;
    vector<Tile> t; int len=strlen(w);
    for(int i=0;i<len;i++){ int rr=d=='A'?r:r+i, cc=d=='A'?c+i:c; if(!b.sq[rr][cc]) t.push_back({rr,cc,w[i]}); }
    Move m; int s=score_tiles(b,t,true,&m);
    if(s<0){ fprintf(stderr,"plan move is illegal: %s\n",line.c_str()); exit(1); }
    p.total+=s; p.ntiles+=t.size(); p.moves.push_back(t); p.shown.push_back(m);
  }
  return p;
}

// ============================== beam search ==============================
uint64_t Z[N][N][52], ZPI[128];
uint64_t bhash(const Board& b,int pi){ uint64_t h=ZPI[pi&127]; for(int r=0;r<N;r++) for(int c=0;c<N;c++) if(b.sq[r][c]){ char ch=b.sq[r][c]; h^=Z[r][c][ch>='a'?26+ch-'a':ch-'A']; } return h; }
// Plan squares switched to blanks during the search (the bag has only 2 blanks).
struct BlankSet { int n=0; signed char r[2], c[2];
  bool has(int rr,int cc) const { for(int i=0;i<n;i++) if(r[i]==rr&&c[i]==cc) return true; return false; }
  void add(int rr,int cc){ r[n]=rr; c[n]=cc; n++; } };
struct State { Board b; Bag bag; int score; int future; int pi; int tiles; int id; int freeleft; BlankSet bl;
  float rank() const { return score+future+TILEVAL*freeleft; } };
struct Arena { int parent; Move mv; bool plan; };
struct Game { int score; vector<pair<Move,bool>> moves; Board board; };

// plan move k's tiles, with any switched-to-blank squares in lowercase
vector<Tile> plan_tiles(const Plan& p,int k,const BlankSet& bl){
  vector<Tile> t=p.moves[k]; for(auto& x:t) if(bl.has(x.r,x.c)) x.L=tolower(x.L); return t; }
// exact points for playing plan moves pi.. from board b (-1 if one becomes illegal)
int plan_future(Board b,const Plan& p,int pi,const BlankSet& bl){
  int tot=0; for(size_t k=pi;k<p.moves.size();k++){ int s=score_tiles(b,plan_tiles(p,k,bl),true); if(s<0) return -1; tot+=s; } return tot; }

Game beam(const Plan& plan,int W,int M,int threads){
  int P=plan.moves.size();
  // which plan move fills each square, and with what letter
  int pidx[N][N]; char plet[N][N]; memset(pidx,-1,sizeof pidx);
  for(int k=0;k<P;k++) for(auto& t:plan.moves[k]){ pidx[t.r][t.c]=k; plet[t.r][t.c]=t.L; }
  // tiles still needed by the plan from state s: real letters, and blanks for switched squares
  auto needs=[&](const State& s,int nd[27]){
    memset(nd,0,27*sizeof(int));
    for(int k=s.pi;k<P;k++) for(auto& t:plan.moves[k]) nd[s.bl.has(t.r,t.c)?26:UP(t.L)]++; };
  auto freeleft=[&](const State& s){ int nd[27]; needs(s,nd); int t=0; for(int l=0;l<27;l++) t+=max(0,s.bag.c[l]-nd[l]); return t; };
  vector<Arena> arena;
  vector<vector<State>> bucket(101);
  State s0; memset(s0.b.sq,0,sizeof s0.b.sq); for(int i=0;i<27;i++) s0.bag.c[i]=BAG0[i];
  s0.score=0; s0.pi=0; s0.tiles=0; s0.id=-1; s0.future=P? plan_future(s0.b,plan,0,s0.bl):0;
  s0.freeleft=freeleft(s0);
  bucket[0].push_back(s0);
  int best=-1, bestid=-1; Board bestb=s0.b;
  struct Child { State s; Move mv; bool isplan; };
  struct Cand { Move m; BlankSet bl; float opt; };     // a free move, plus plan squares it switches to blanks
  auto expand=[&](const State& s,vector<Child>& out){
    // 1. the next plan move
    if(s.pi<P){
      Child c{s,{},true}; auto t=plan_tiles(plan,s.pi,s.bl); int sc=score_tiles(c.s.b,t,true,&c.mv);
      if(sc>=0){ for(auto& x:t) c.s.bag.c[x.L>='a'?26:UP(x.L)]--;
        c.s.score+=sc; c.s.future-=sc; c.s.pi++; c.s.tiles+=t.size(); c.s.freeleft=freeleft(c.s); out.push_back(c); }
    }
    if(board_empty(s.b)&&P>0) return;           // the plan opens the game
    // 2. free moves with the tiles the plan doesn't need, off the plan's squares
    bool resv[N][N]={}, nearp[N][N]={};
    for(int k=s.pi;k<P;k++) for(auto& t:plan.moves[k]) resv[t.r][t.c]=true;
    // A free tile can change a later plan move only if it sits just before or after a run of
    // tiles (on the board as it will be with the rest of the plan) that contains a plan square.
    for(int dir=0;dir<2;dir++) for(int line=0;line<N;line++){
      auto occ=[&](int i){ int r=dir?i:line, c=dir?line:i; return s.b.sq[r][c]||resv[r][c]; };
      auto isr=[&](int i){ int r=dir?i:line, c=dir?line:i; return resv[r][c]; };
      auto mark=[&](int i){ if(i<0||i>=N) return; int r=dir?i:line, c=dir?line:i; if(!s.b.sq[r][c]&&!resv[r][c]) nearp[r][c]=true; };
      for(int i=0;i<N;){ if(!occ(i)){ i++; continue; } int j=i; bool hasr=false; while(j<N&&occ(j)){ hasr|=isr(j); j++; }
        if(hasr){ mark(i-1); mark(j); } i=j; } }
    int nd[27]; needs(s,nd);
    Bag fb=s.bag; for(int l=0;l<27;l++){ fb.c[l]-=nd[l]; if(fb.c[l]<0) fb.c[l]=0; }
    if(fb.total()==0) return;
    // Per tile count keep the best M moves by exact key, and separately the best M moves that use
    // a blank by an optimistic key (the blank might be swapped for a real plan tile). Then
    // evaluate swaps exactly and keep the best M overall.
    vector<vector<Cand>> keep(8), keepb(8);
    auto cmpk=[](const Cand& a,const Cand& b){ return a.m.key>b.m.key; };
    auto cmp=[](const Cand& a,const Cand& b){ return a.opt>b.opt; };
    function<void(const Move&)> sink=[&](const Move& m0){
      Cand cd{m0,s.bl,0}; Move& m=cd.m; m.future=s.future;
      if(s.pi<P){
        bool near=false;
        for(int i=0;i<m.len&&!near;i++){ int r=m.dir==0?m.line:m.start+i, c=m.dir==0?m.start+i:m.line; if(!s.b.sq[r][c]&&nearp[r][c]) near=true; }
        if(near){ Board nb=s.b; for(auto& t:move_tiles(s.b,m)) nb.sq[t.r][t.c]=t.L;
          m.future=plan_future(nb,plan,s.pi,s.bl); if(m.future<0) return; }
      }
      m.key=m.score+(m.future-s.future); cd.opt=m.key; bool hasb=false;
      for(int i=0;i<m.len;i++) if(m.letters[i]>='a'){ cd.opt+=6*VAL[m.letters[i]-'a']; hasb=true; }
      auto& v=keep[m.placed];
      if((int)v.size()<M){ v.push_back(cd); push_heap(v.begin(),v.end(),cmpk); }
      else if(cd.m.key>v.front().m.key){ pop_heap(v.begin(),v.end(),cmpk); v.back()=cd; push_heap(v.begin(),v.end(),cmpk); }
      if(!hasb||s.bl.n>=2) return;
      auto& u=keepb[m.placed];
      if((int)u.size()<M){ u.push_back(cd); push_heap(u.begin(),u.end(),cmp); }
      else if(cd.opt>u.front().opt){ pop_heap(u.begin(),u.end(),cmp); u.back()=cd; push_heap(u.begin(),u.end(),cmp); }
    };
    gen_moves(s.b,fb,resv,sink);
    for(int k=1;k<=7;k++){
      auto& v=keep[k];
      for(auto& cd:keepb[k]){ bool dup=false;
        for(auto& x:v) if(x.m.dir==cd.m.dir&&x.m.line==cd.m.line&&x.m.start==cd.m.start&&!strcmp(x.m.letters,cd.m.letters)) dup=true;
        if(!dup) v.push_back(cd); }
      for(auto& cd:v){
        // For each blank in the move: use the real tile instead, and put the blank on a plan
        // square with that letter (not yet played), if that is worth more. Evaluated exactly.
        for(int i=0;i<cd.m.len;i++){
          int r=cd.m.dir==0?cd.m.line:cd.m.start+i, c=cd.m.dir==0?cd.m.start+i:cd.m.line;
          char L=cd.m.letters[i]; if(L<'a'||s.b.sq[r][c]||cd.bl.n>=2) continue;
          char U=toupper(L);
          Move bestm=cd.m; BlankSet bestbl=cd.bl; float bestkey=cd.m.key;
          for(int rr=0;rr<N;rr++) for(int cc=0;cc<N;cc++){
            if(pidx[rr][cc]<s.pi||plet[rr][cc]!=U||cd.bl.has(rr,cc)) continue;
            Move m2=cd.m; m2.letters[i]=U; BlankSet b2=cd.bl; b2.add(rr,cc);
            Board nb=s.b; auto t=move_tiles(s.b,m2); int sc=score_tiles(nb,t,true); if(sc<0) continue;
            int fut=s.pi<P? plan_future(nb,plan,s.pi,b2) : 0; if(fut<0) continue;
            m2.score=sc; m2.future=fut; m2.key=sc+(fut-s.future);
            if(m2.key>bestkey){ bestkey=m2.key; bestm=m2; bestbl=b2; }
          }
          cd.m=bestm; cd.bl=bestbl;
        }
      }
      sort(v.begin(),v.end(),[](const Cand& a,const Cand& b){ return a.m.key>b.m.key; });
      if((int)v.size()>M) v.resize(M);
      for(auto& cd:v){
        Child c{s,cd.m,false}; c.s.bl=cd.bl; auto t=move_tiles(s.b,cd.m); apply_tiles(c.s.b,c.s.bag,t);
        c.s.score+=cd.m.score; c.s.future=cd.m.future; c.s.tiles+=t.size(); c.s.freeleft=freeleft(c.s); out.push_back(c); }
    }
  };
  auto byrank=[](const State& a,const State& b){ return a.rank()>b.rank(); };
  auto shash=[&](const State& s){ uint64_t h=bhash(s.b,s.pi); for(int i=0;i<s.bl.n;i++) h^=Z[s.bl.r[i]][s.bl.c[i]][26]*0x9e3779b97f4a7c15ULL; return h; };
  for(int n=0;n<=100;n++){
    auto& B=bucket[n]; if(B.empty()) continue;
    stable_sort(B.begin(),B.end(),byrank);
    vector<State> keep; unordered_set<uint64_t> seen;
    for(auto& s:B){ if((int)keep.size()>=W) break; if(seen.insert(shash(s)).second) keep.push_back(s); }
    vector<State>().swap(B);
    for(auto& s:keep) if(s.score>best){ best=s.score; bestid=s.id; bestb=s.b; }
    vector<vector<Child>> kids(keep.size());
    int nt=min(threads,(int)keep.size()); vector<thread> pool;
    for(int t=0;t<nt;t++) pool.emplace_back([&,t]{ for(size_t i=t;i<keep.size();i+=nt) expand(keep[i],kids[i]); });
    for(auto& th:pool) th.join();
    for(size_t i=0;i<keep.size();i++) for(auto& ch:kids[i]){
      arena.push_back({keep[i].id,ch.mv,ch.isplan}); ch.s.id=arena.size()-1;
      if(ch.s.tiles<=100) bucket[ch.s.tiles].push_back(ch.s); }
    for(int k=n+1;k<=min(100,n+7);k++){ auto& V=bucket[k]; if((int)V.size()>40*W){ nth_element(V.begin(),V.begin()+20*W,V.end(),byrank); V.resize(20*W); } }
  }
  Game g; g.score=best; g.board=bestb;
  for(int id=bestid;id>=0;id=arena[id].parent) g.moves.push_back({arena[id].mv,arena[id].plan});
  reverse(g.moves.begin(),g.moves.end());
  return g;
}

// ============================== main ==============================
int main(int argc,char** argv){
  if(argc<2){ fprintf(stderr,"usage: scrabmax DICT [-t threads] [-p plans] [-c candidates] [-w width] [-m moves] [-f planfile] [-P]\n"); return 1; }
  const char* dict=argv[1];
  int threads=thread::hardware_concurrency(), nplans=20, ncand=1000, W=20, M=8; const char* pfile=nullptr; bool printonly=false;
  for(int i=2;i<argc;i++){ string a=argv[i];
    auto val=[&]{ if(i+1>=argc){ fprintf(stderr,"missing value for %s\n",a.c_str()); exit(1);} return argv[++i]; };
    if(a=="-t") threads=atoi(val()); else if(a=="-p") nplans=atoi(val()); else if(a=="-c") ncand=atoi(val());
    else if(a=="-v") TILEVAL=atof(val());
    else if(a=="-H") HOOKMIN=atof(val());
    else if(a=="-D") HOOKDEBUG=true;
    else if(a=="-w") W=atoi(val()); else if(a=="-m") M=atoi(val()); else if(a=="-f") pfile=val(); else if(a=="-P") printonly=true;
    else { fprintf(stderr,"unknown option %s\n",a.c_str()); return 1; } }
  if(threads<1) threads=1;
  for(int r=0;r<N;r++) for(int c=0;c<N;c++){ char p=LAYOUT[r][c]; LM[r][c]=p==':'?2:p==';'?3:1; WM[r][c]=(p=='-'||p=='*')?2:p=='='?3:1; }
  newnode(); ifstream f(dict); string w; int nw=0;
  while(f>>w){ for(auto& ch:w) ch=toupper(ch); bool ok=w.size()>=2&&w.size()<=15; for(char ch:w) if(ch<'A'||ch>'Z') ok=false;
    if(ok){ addword(w); BYLEN[w.size()].push_back(w); if(w.size()==8) CONN[{w[0],w[7]}].push_back(w); nw++; } }
  for(auto& w:BYLEN[8]){ if(isword(w.substr(1,7))) HUP[{w[0],w[7]}].push_back(w); if(isword(w.substr(0,7))) HDN[{w[0],w[7]}].push_back(w); }
  for(auto& [k,v]:CONN) sort(v.begin(),v.end());
  for(auto& v:BYLEN) sort(v.begin(),v.end());
  mt19937_64 rng(12345); for(auto& a:Z) for(auto& b:a) for(auto& c:b) c=rng(); for(auto& z:ZPI) z=rng();
  fprintf(stderr,"%d words; threads=%d plans=%d candidates=%d width=%d moves=%d\n",nw,threads,nplans,ncand,W,M);

  vector<Plan> plans;
  if(pfile) plans.push_back(read_plan(pfile));
  else if(nplans>0) plans=find_plans(ncand,nplans,threads);
  if(plans.empty()){ Plan p; p.total=0; p.big=0; p.ntiles=0; p.nhooks=0; p.desc="(no plan)"; plans.push_back(p); }
  if(printonly){
    for(size_t k=0;k<plans.size();k++){ printf("PLAN %zu %d %s\n",k,plans[k].total,plans[k].desc.c_str());
      for(auto& m:plans[k].shown) printf("%s\n",movestr(m).c_str()); }
    return 0;
  }
  Game bestg; bestg.score=-1; string bestdesc;
  for(size_t k=0;k<plans.size();k++){
    Game g=beam(plans[k],W,M,threads);
    fprintf(stderr,"plan %2zu: plan alone %5d (%2d tiles) -> game %5d  %s\n",k,plans[k].total,plans[k].ntiles,g.score,plans[k].desc.c_str());
    if(g.score>bestg.score){ bestg=g; bestdesc=plans[k].desc; }
  }
  printf("BEST %d  (%s)\n",bestg.score,bestdesc.c_str());
  for(auto& [m,isplan]:bestg.moves) printf("%s  %s\n",movestr(m).c_str(),isplan?"plan":"extra");
  print_board(stdout,bestg.board);
}
