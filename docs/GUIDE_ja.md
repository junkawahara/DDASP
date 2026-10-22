# DDASP 入門ガイド(ASP 初心者向け)

このガイドは、**ASP(Answer Set Programming、解集合プログラミング)を初めて使う人**が、
DDASP を使って「ASP で書いた問題のすべての解」を ZDD(ゼロサプレス型二分決定グラフ)
として得られるようになることを目標にしています。

ASP の文法は、このガイドで使う範囲を順に説明します。より詳しい文法は
[Potassco(clingo)のドキュメント](https://potassco.org/doc/) を参照してください。

## 目次

1. [DDASP とは何か](#1-ddasp-とは何か)
2. [インストール](#2-インストール)
3. [最初の一歩: 3 つの原子と制約](#3-最初の一歩-3-つの原子と制約)
4. [ASP の基本文法](#4-asp-の基本文法)
5. [例題: N クイーン](#5-例題-n-クイーン)
6. [例題: グラフ彩色](#6-例題-グラフ彩色)
7. [最適化(#minimize)](#7-最適化minimize)
8. [さらに例題: 再帰・数え上げ・頂点被覆・ナップサック](#8-さらに例題-再帰数え上げ頂点被覆ナップサック)
9. [コマンドラインオプション一覧](#9-コマンドラインオプション一覧)
10. [Python から使う](#10-python-から使う)
11. [ZDD を KyotoDD で活用する](#11-zdd-を-kyotodd-で活用する)
12. [DD を ASP から使う(2 段階のコンパイル)](#12-dd-を-asp-から使う2-段階のコンパイル)
13. [よくある質問とトラブルシューティング](#13-よくある質問とトラブルシューティング)

---

## 1. DDASP とは何か

### ASP とは

ASP は、「解きたい問題の条件」を論理的なルールとして書くと、ソルバがその条件を満たす
解(**解集合、answer set**)を求めてくれる宣言的プログラミングの枠組みです。
パズル、スケジューリング、組合せ最適化などに使われます。代表的な処理系は
[clingo](https://potassco.org/clingo/) です。

たとえば「a, b, c のうち好きなものを選ぶ。ただし 3 つ全部は選べない」は、
ASP では次の 2 行で書けます。

```prolog
{ a ; b ; c }.
:- a, b, c.
```

clingo はこのプログラムの解集合を **1 つずつ** 出力します(全部で 7 個)。

### DDASP がすること

DDASP は、同じ ASP プログラムを入力として、**すべての解集合をまとめて 1 つの ZDD
に圧縮**します。ZDD は「集合の集合」をコンパクトに表すデータ構造で、

- 解の **個数** を(解を 1 つずつ列挙せずに)厳密に数えられる
- 解を **列挙** したり、**一様ランダムに 1 つ取り出し** たりできる
- 「原子 X を含む解だけ」などの **絞り込み** や集合演算が、ZDD 同士の演算でできる
- ファイルに保存して、他のプログラム(KyotoDD / Python)から再利用できる

という特長があります。解が何万個・何億個あっても、ZDD のノード数が小さければ
高速に扱えます。

### 処理の流れ

```
 prog.lp ──(gringo でグラウンディング)──▶ aspif ──(DDASP)──▶ ZDD ──▶ 個数 / 列挙 / ファイル出力
```

1. **グラウンディング**: 変数を含むルール(`q(X,Y)` など)を、変数のない
   ルールの集まりに展開します。これは clingo 付属の **gringo** が行います。
2. **コンパイル**: DDASP がグラウンド済みプログラムを読み、解集合の族を表す
   ZDD を KyotoDD ライブラリで構築します。

---

## 2. インストール

Linux(または macOS)で、C++ コンパイラ、CMake(3.15 以上)、Python 3 が
必要です。

```sh
git clone --recursive https://github.com/junkawahara/DDASP.git
cd DDASP
```

すでにクローン済みで `third_party/KyotoDD` が空の場合は、サブモジュールを取得します。

```sh
git submodule update --init --recursive
```

C++ 本体(コマンド `ddasp`)をビルドします。

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build        # 動作確認(C++ のユニットテスト)
```

次に、グラウンダ(clingo)と Python パッケージを入れる仮想環境を作ります。

```sh
python3 -m venv .venv
.venv/bin/pip install clingo pytest scikit-build-core pybind11
.venv/bin/pip install --no-build-isolation -e third_party/KyotoDD/python   # kyotodd
.venv/bin/pip install --no-build-isolation -e python                       # ddasp
.venv/bin/pytest                                                           # 動作確認
```

> **gringo について**: pip の `clingo` パッケージには `gringo` 実行ファイルが
> 含まれていません。代わりに `python -m clingo --mode=gringo` をグラウンダとして
> 使います。以下では、これを変数 `G` に入れておきます。
>
> ```sh
> G=".venv/bin/python -m clingo --mode=gringo"
> ```
>
> 別途 `gringo` コマンドをインストールしてある場合は `--gringo` を省略できます。

---

## 3. 最初の一歩: 3 つの原子と制約

`examples/choice.lp` を見てみましょう。

```prolog
% Three free atoms, at most two of them true.
{ a ; b ; c }.
:- a, b, c.
#show a/0. #show b/0. #show c/0.
```

- `%` から行末まではコメントです。
- `{ a ; b ; c }.` は **選択ルール** です。「a, b, c のそれぞれを真にしてもしなくてもよい」。
- `:- a, b, c.` は **制約** です。「a, b, c が同時に真になってはいけない」。
- `#show a/0.` は、出力に原子 `a`(引数 0 個)を含める指示です。

### 解の個数を数える

```sh
./build/ddasp --gringo "$G" examples/choice.lp
```

```
answer sets: 7
```

3 つの原子の真偽の組合せは 2³ = 8 通りで、そのうち「全部真」の 1 通りが制約で
除かれるので 7 個です。

### 解を列挙する

```sh
./build/ddasp --gringo "$G" --enumerate examples/choice.lp
```

```
answer sets: 7

c
b
b c
a
a c
a b
```

1 行が 1 つの解集合で、真になった原子が並びます。最初の空行は「どれも真でない」解です。

### clingo と比べてみる

同じプログラムを clingo で解くと、同じ 7 個の解が出ます(`0` は「全解を出力」)。

```sh
.venv/bin/python -m clingo examples/choice.lp 0
```

DDASP は clingo の代わりではなく、**解の全体を 1 つのデータ構造として扱いたいとき** に
使うものだと考えてください。

---

## 4. ASP の基本文法

このガイドで使う文法をまとめます。

### 事実(fact)

無条件に真な原子です。引数(項)を持てます。

```prolog
node(1). node(2).
edge(1,2).
node(1..5).          % node(1). node(2). ... node(5). の省略形
```

### ルール(rule)

`頭 :- 本体.` の形で、「本体がすべて真なら頭も真」を表します。

```prolog
a :- b.              % b が真なら a も真
b :- a.
a :- c.
```

ASP の解集合は「ルールから **根拠をもって** 導けるものだけが真」という意味
(安定モデル意味論)で決まります。`examples/loop.lp` がその例です。

```prolog
{ c }.
a :- b.
b :- a.
a :- c.
#show a/0. #show b/0. #show c/0.
```

```sh
./build/ddasp --gringo "$G" --enumerate examples/loop.lp
```

```
answer sets: 2

a b c
```

`a` と `b` は互いに支え合っていますが、`c` が偽のときは `a`, `b` を真にする
根拠がないので、両方とも偽になります(`{a, b}` だけが真、という解は出ません)。
これが命題論理の「モデル」と ASP の「解集合」の大きな違いです。

### 否定(default negation)

`not` は「真であると導けない」ことを表します。

```prolog
a :- not b.
b :- not a.
```

この 2 行からは `{a}` と `{b}` の 2 つの解集合が得られます。

### 制約(constraint)

頭のないルールです。本体がすべて真になる解を除きます。

```prolog
:- a, b.             % a と b が同時に真の解は捨てる
```

### 選択ルール(choice rule)と基数制約

```prolog
{ a ; b ; c }.                        % それぞれ自由に選ぶ
{ q(X) : p(X) } >= 2.                 % p(X) が成り立つ X のうち 2 つ以上の q(X) を選ぶ
{ col(N,C) : color(C) } = 1 :- node(N).   % 各ノード N に色をちょうど 1 つ割り当てる
```

`{ ... : 条件 }` の `:` の右側は「どの原子を候補にするか」の条件で、
グラウンディング時に展開されます。

### 変数

大文字で始まる名前(`X`, `N`, `C` など)は変数です。ルール内の変数は、
グラウンディングで具体的な値に置き換えられます。変数は本体の肯定リテラルに
現れる必要があります(安全性の条件)。

### #show

出力に含める原子を指定します。`#show` を 1 つでも書くと、それ以外の原子は
出力から(そして DDASP の ZDD からも)隠されます。

```prolog
#show col/2.         % col(N,C) だけを出力
```

> DDASP では、`#show` で指定した原子だけが ZDD の変数になります(**射影**)。
> 補助原子を隠しておくと ZDD が小さくなり、解の個数も「見えている原子の組合せ」
> として数えられます。すべての原子を変数にしたい場合は `--all-atoms` を使います。
> `#show t : not a.` のような条件付きの項も clingo と同じように表示されます
> (条件に現れる原子は ZDD の変数として残ります)。

---

## 5. 例題: N クイーン

`examples/queens.lp`:

```prolog
% N-queens.
#const n = 6.
{ q(X,Y) : Y = 1..n } = 1 :- X = 1..n.
:- q(X,Y), q(X',Y), X < X'.
:- q(X,Y), q(X',Y'), X < X', |Y - Y'| == X' - X.
#show q/2.
```

- `#const n = 6.` は定数の定義です。
- 選択ルール: 各列 X に、行 Y をちょうど 1 つ選んで `q(X,Y)` を置きます。
- 1 つ目の制約: 同じ行 Y に 2 つのクイーンを置かない。
- 2 つ目の制約: 斜めに並ばない。

```sh
./build/ddasp --gringo "$G" examples/queens.lp
```

```
answer sets: 4
```

6 クイーンの解は 4 つです。`-c n=8` を付けると、ファイルを書き換えずに 8 クイーンになります
(プログラム中の `#const n = 6.` の値をコマンドラインの値で置き換えます。gringo / clingo の
`-c` と同じで、複数指定できます)。

```sh
./build/ddasp --gringo "$G" -c n=8 examples/queens.lp     # answer sets: 92
./build/ddasp --gringo "$G" -c n=10 examples/queens.lp    # answer sets: 724
```

`--stats` を付けると ZDD のノード数や所要時間が標準エラー出力に出ます。

```sh
./build/ddasp --gringo "$G" --stats examples/queens.lp
```

```
rules: 236
atoms: 60
shown atoms: 36
...
zdd nodes: 24
bdd nodes: 129
...
compile time: 0.00226819 s
answer sets: 4
```

---

## 6. 例題: グラフ彩色

`examples/coloring.lp`:

```prolog
% 3-coloring of a small graph.
node(1..5).
edge(1,2). edge(2,3). edge(3,4). edge(4,5). edge(5,1). edge(1,3).
color(r). color(g). color(b).
{ col(N,C) : color(C) } = 1 :- node(N).
:- edge(X,Y), col(X,C), col(Y,C).
#show col/2.
```

各ノードに色を 1 つ割り当て、辺で結ばれたノードが同じ色にならないようにします。

```sh
./build/ddasp --gringo "$G" --enumerate --limit 3 examples/coloring.lp
```

```
answer sets: 18
col(1,r) col(2,g) col(3,b) col(4,r) col(5,g)
col(1,r) col(2,g) col(3,b) col(4,r) col(5,b)
col(1,r) col(2,g) col(3,b) col(4,g) col(5,b)
```

`--limit 3` で最初の 3 個だけを表示しています。個数(18)はすべての解を
数えた値です。

---

## 7. 最適化(#minimize)

`#minimize` で解のコストを定義できます。`examples/minimize.lp`:

```prolog
% #minimize: at least one of a, b; a costs 1, b costs 2, c costs 1.
{ a ; b ; c }.
:- not a, not b.
#minimize { 1,a : a ; 2,b : b ; 1,c : c }.
#show a/0. #show b/0. #show c/0.
```

- 制約 `:- not a, not b.` は「a か b の少なくとも一方は真」を表します。
- `#minimize { 1,a : a ; ... }` は「a が真ならコスト 1、b が真ならコスト 2、c が真ならコスト 1」
  の合計を最小化する指示です。`重み,識別子 : 条件` の形で書きます。

`--opt` を付けないと、コストに関係なくすべての解が対象です。

```sh
./build/ddasp --gringo "$G" --enumerate examples/minimize.lp
```

```
answer sets: 6
b
b c
a
a c
a b
a b c
```

`--opt` を付けると、コストが最小の解だけが残ります。

```sh
./build/ddasp --gringo "$G" --opt --enumerate examples/minimize.lp
```

```
answer sets: 1
optimum: 1
a
```

### コストに上限や下限を付ける(`--cost-le` / `--cost-ge` / `--cost-eq`)

最適解だけでなく「コストが K 以下の解」も族として取り出せます。`--cost-le K` はコストが
K 以下の解、`--cost-ge K` は K 以上の解、`--cost-eq K` はちょうど K の解を残します
(`--cost-le` と `--cost-ge` は同時に指定できます。`--cost-eq` と `--opt` は他と併用できません)。

```sh
./build/ddasp --gringo "$G" --cost-le 2 --enumerate examples/minimize.lp
```

```
answer sets: 3
b
a
a c
```

`+d` と書くと「最適値 + d」の意味になり、最適値も表示されます。`--cost-le +1` は
「最適値より高々 1 だけ悪い解」です(`--cost-eq +0` は `--opt` と同じ)。

```sh
./build/ddasp --gringo "$G" --cost-le +1 --enumerate examples/minimize.lp
```

```
answer sets: 3
optimum: 1
b
a
a c
```

`#minimize` に複数の優先度(`1@2,...` のような `@` 付きの重み)があるときは、`--cost-le 3,5`
のように優先度の高い順にコンマで並べます(`optimum:` 行と同じ順)。比較は clingo の
`--opt-mode=enum,3,5` と同じ辞書式です。つまり、上位の優先度のコストが上限より真に小さい解は
下位のコストに関係なく残り、等しい解だけを次の優先度で比べます。省略した下位の優先度は
制限されません。`--opt` と同様、コストは `#show` で射影する前の解集合に対して評価されます。

### コストごとの解の個数を見る(`--cost-histogram`)

`--cost-histogram` は、コストの値ごとの解の個数を `コスト 個数` の行で昇順に表示します
(解の無いコストは出ません)。`examples/minimize.lp` ではコスト 1 の解が 1 個、2 の解が 2 個、…です。

```sh
./build/ddasp --gringo "$G" --cost-histogram examples/minimize.lp
```

```
answer sets: 6
1 1
2 2
3 2
4 1
```

複数の優先度があれば `3,5 12` のようにコスト列ごとに数えます。`#show` で射影しているときは、
表示される集合 1 つを 1 個と数え、その集合に射影される解のうち最小のコストに割り当てます。
したがって個数の合計は `answer sets: N` に一致し、あるコストまでの累積は `--cost-le` でそのコストを
指定したときの個数に一致します。`--opt` や `--cost-le` と併用すると、絞り込んだ後の解について数えます。

### コストの小さい順に解を列挙する(`--by-cost`)

`--by-cost` は、解をコストの小さい順に `コスト: 原子...` の形で表示します(`--enumerate` を含みます)。
`--limit N` と組み合わせると「上位 N 解」が得られます。

```sh
./build/ddasp --gringo "$G" --by-cost --limit 3 examples/minimize.lp
```

```
answer sets: 6
1: a
2: b
2: a c
```

同じコストの解どうしの順序は決まっていません。複数の優先度があれば `3,5: a b c` のように辞書式の
昇順です。`#show` で射影しているときは、表示される集合を 1 回だけ、その集合に射影される解の最小
コストの位置で表示します(`--cost-histogram` と同じ数え方)。優先度が 1 つで、重みを持つ原子が
すべて表示されているときは KyotoDD の k 最良列挙(`ZddMinWeightIterator`)で 1 解ずつ取り出し、
そうでなければコスト値ごとに最小コストの解を取り出して列挙します。

---

## 8. さらに例題: 再帰・数え上げ・頂点被覆・ナップサック

ここまでの文法で書ける、少し実用寄りの例を 4 つ紹介します。いずれも `examples/` にあります。

### 8.1 再帰ルールと到達可能性(`examples/reachability.lp`)

```prolog
node(1..5).
edge(1,2). edge(2,3). edge(3,1). edge(3,4). edge(4,5). edge(5,4).
{ use(X,Y) : edge(X,Y) }.
reach(1).
reach(Y) :- reach(X), use(X,Y).
% Every node must be reachable.
:- node(X), not reach(X).
#show use/2.
```

有向グラフの辺の部分集合 `use/2` を選び、「ノード 1 からすべてのノードに到達できる」
ものだけを解とします。`reach/1` の **再帰ルール** がポイントです。

```sh
./build/ddasp --gringo "$G" --enumerate examples/reachability.lp
```

```
answer sets: 4
use(1,2) use(2,3) use(3,4) use(4,5)
use(1,2) use(2,3) use(3,4) use(4,5) use(5,4)
use(1,2) use(2,3) use(3,1) use(3,4) use(4,5)
use(1,2) use(2,3) use(3,1) use(3,4) use(4,5) use(5,4)
```

辺 1→2→3→4→5 は必須で、`use(3,1)` と `use(5,4)` は自由なので 2² = 4 個です。
注目してほしいのは、**`use(3,4)` のない解が存在しない**ことです。命題論理のモデル
としてなら「`use(4,5)` と `use(5,4)` で `reach(4)` と `reach(5)` が互いに支え合う」
割り当ても条件を満たしますが、ASP ではノード 1 からの根拠のない循環は認められません
(4 節の `loop.lp` と同じ原理)。DDASP はこれを、グラウンドプログラムの強連結成分
(SCC)ごとに「根拠のない集合」を排除することで扱います。`--stats` の `groups` と
`aux vars` がその SCC の数と補助変数の数です。

### 8.2 独立集合の数え上げ(`examples/independent_set.lp`)

```prolog
#const n = 4.
vertex((X,Y)) :- X = 1..n, Y = 1..n.
adj((X,Y),(X+1,Y)) :- vertex((X,Y)), X < n.
adj((X,Y),(X,Y+1)) :- vertex((X,Y)), Y < n.
{ in(V) : vertex(V) }.
:- adj(U,V), in(U), in(V).
#show in/1.
```

n×n の格子グラフで、隣り合う頂点を同時に選ばない頂点集合(独立集合)をすべて求めます。
項は `(X,Y)` のようなタプルにできます。

```sh
./build/ddasp --gringo "$G" examples/independent_set.lp                      # answer sets: 1234
./build/ddasp --gringo "$G --const n=8" --stats examples/independent_set.lp  # answer sets: 660647962955
```

8×8 では解が約 6600 億個ありますが、ZDD のノード数は 1347 で、構築は 0.01 秒程度です。
解を 1 つずつ列挙するソルバでは不可能な規模でも、ZDD なら個数を厳密に数えたり
(11 節)一様ランダムに 1 つ取り出したりできます。

### 8.3 最小頂点被覆(`examples/vertex_cover.lp`)

```prolog
edge(1,2). edge(1,3). edge(2,3). edge(3,4). edge(4,5). edge(4,6). edge(5,6).
vertex(X) :- edge(X,_).
vertex(Y) :- edge(_,Y).
{ in(X) : vertex(X) }.
:- edge(X,Y), not in(X), not in(Y).
#minimize { 1,X : in(X) }.
#show in/1.
```

すべての辺の少なくとも一方の端点を含む頂点集合(頂点被覆)のうち、頂点数が最小のものを
求めます。`_` は「何でもよい」を表す無名変数です。

```sh
./build/ddasp --gringo "$G" examples/vertex_cover.lp          # answer sets: 15  (すべての頂点被覆)
./build/ddasp --gringo "$G" --opt --enumerate examples/vertex_cover.lp
```

```
answer sets: 8
optimum: 4
in(2) in(3) in(5) in(6)
in(2) in(3) in(4) in(6)
...
```

最小サイズは 4 で、そのような被覆は 8 通りあります。「最適解が何通りあるか」を
数えられるのも DDASP の特長です。最適値から 1 だけ悪い解まで含めるなら `--cost-le +1` です。

```sh
./build/ddasp --gringo "$G" --cost-le +1 examples/vertex_cover.lp    # answer sets: 14, optimum: 4
./build/ddasp --gringo "$G" --cost-eq +1 examples/vertex_cover.lp    # answer sets: 6  (5 頂点の被覆)
./build/ddasp --gringo "$G" --cost-histogram examples/vertex_cover.lp  # 4 8 / 5 6 / 6 1: 頂点数ごとの被覆の個数
./build/ddasp --gringo "$G" --by-cost --limit 3 examples/vertex_cover.lp  # 上位 3 解を `4: in(2) in(3) ...` の形で
```

### 8.4 ナップサック問題と `#sum`(`examples/knapsack.lp`)

```prolog
#const capacity = 10.
item(a, 5, 10).   % item(Name, Weight, Value)
item(b, 4,  7).
item(c, 3,  5).
item(d, 2,  3).
item(e, 1,  1).
{ in(I) : item(I,_,_) }.
:- #sum { W,I : in(I), item(I,W,_) } > capacity.
#minimize { -V,I : in(I), item(I,_,V) }.
#show in/1.
```

`#sum { W,I : 条件 }` は条件を満たす `I` ごとの重み `W` の合計で、ここでは
「選んだ品物の重さの合計が容量を超える」解を制約で除いています。価値の合計を
**最大化**したいときは、負の重みを `#minimize` します。

```sh
./build/ddasp --gringo "$G" examples/knapsack.lp                   # answer sets: 25 (容量内の選び方)
./build/ddasp --gringo "$G" --opt --enumerate examples/knapsack.lp
```

```
answer sets: 2
optimum: -18
in(a) in(c) in(d)
in(a) in(b) in(e)
```

価値 18 の選び方が 2 通りあります(`optimum` は最小化した値なので負になります)。

---

## 9. コマンドラインオプション一覧

```
ddasp [options] FILE...
```

`FILE` が `.aspif` なら、グラウンディング済みとしてそのまま読みます。それ以外は
`--gringo` のコマンドでグラウンディングしてから読みます。

| オプション | 意味 |
|---|---|
| `--count` | 解集合の個数だけを(`answer sets:` を付けずに)表示。`-q` と組み合わせて使う |
| `--enumerate` | すべての解集合を 1 行ずつ表示(原子は変数順 = 名前順) |
| `--limit N` | `--enumerate` で最大 N 個だけ表示(`0` = すべて)。ZDD から順に取り出すので大きな族でも軽い |
| `--sample N` | 解を一様ランダムに N 個取り出して 1 行ずつ表示(復元抽出なので同じ解が複数回出ることがある。射影後の各表示集合が等確率。解が無ければ何も出ない) |
| `--distinct` | `--sample` で重複しない N 個をランダムな順に表示(N が解の個数以上なら全部) |
| `--seed S` | 乱数の種(既定はランダム) |
| `--by-cost` | 解をコストの小さい順に `コスト: 原子...` の行で表示(`--enumerate` を含む。`--limit N` で上位 N 解)。射影時は表示集合を最小コストの位置に 1 回だけ表示 |
| `--brave` | **勇敢帰結**: どれかの解集合に現れる名前を 1 行に表示(解集合と同じ形式) |
| `--cautious` | **慎重帰結**: すべての解集合に現れる名前を 1 行に表示(解が無ければ空行) |
| `--frequencies` | 表示される名前ごとに `名前 個数`(その名前を含む解集合の個数)を 1 行ずつ表示 |
| `-o FILE` | ZDD を KyotoDD バイナリ形式で FILE に保存 |
| `--bdd` | `-o` で ZDD の代わりに BDD を保存 |
| `--atoms FILE` | 変数番号 / レベル / 原子番号 / 原子名の対応表と、`#minimize` のコスト関数(`レベル:重み`)を FILE に保存 |
| `--save FILE` | 解集合の族を **保存 DD**(ZDD + 原子名 + 事実名の 1 ファイル)として FILE に保存。後の実行で `--load` する |
| `--load FILE` | 保存 DD を読み込み、その原子を新しいプログラムの `#external` 原子として使えるようにし、解集合をその族に制限する(複数回指定可) |
| `--all-atoms` | `#show` による射影をせず、すべての原子を DD の変数にする |
| `--project` | `#project` 文があれば、表示原子の代わりに `#project` の原子へ射影する(clingo の `--project` 相当。名前のない原子は `__atomN` と表示) |
| `--opt` | `#minimize` の最適解だけを残す(`--cost-*` とは併用不可) |
| `--cost-le K[,K..]` | `#minimize` のコストが K 以下の解だけを残す。優先度ごとに高い順のコンマ区切り(`optimum:` 行の順)で、比較は clingo の `--opt-mode=enum,K,..` と同じ辞書式。省略した下位の優先度は無制限。`+d` は「最適値 + d」(最適値も表示) |
| `--cost-ge K[,K..]` | コストが K 以上の解だけを残す(書式は同じ。`--cost-le` と併用可) |
| `--cost-eq K[,K..]` | コストがちょうど K の解だけを残す(書式は同じ。`--cost-le` / `--cost-ge` / `--opt` とは併用不可) |
| `--cost-histogram` | コストの値(列)ごとの解の個数を `コスト 個数` の行で昇順に表示。射影時は表示集合を最小コストで 1 個と数える(合計は `answer sets: N`) |
| `--external-values` | `#external` 原子を宣言の初期値(`#external a. [true]` など。無印の `#external a.` は false)に固定する。既定では初期値を無視して自由にする |
| `--semantics stable\|supported\|classical` | 計算する族(既定 `stable` = 答集合。`supported` は Clark 完備化のモデル、`classical` はルールを含意として読んだ古典モデル)。要約行は `supported models:` / `classical models:` になる |
| `--method scc\|twocopy` | 安定モデルの構成法(既定 `scc`、`twocopy` は参照実装。`stable` 以外では使われない) |
| `--order name\|id\|force\|FILE` | 変数順序(既定 `name`。`force` は FORCE ヒューリスティック: 名前順から始めて各原子をそれが現れるルールの重心へ動かし、スパン(ルールごとの最初と最後の原子の距離の和)が改善する間繰り返す。FILE は上から順に並べる原子名(名前のない原子は `__atomN`)の一覧) |
| `--gringo CMD` | グラウンダのコマンド(`CMD --output=intermediate [-c NAME=VALUE...] FILE...` として実行) |
| `-c NAME=VALUE` | グラウンダに定数 NAME の値を渡す(gringo の `-c` と同じ。プログラム中の `#const NAME = ...` より優先。複数指定可。`.aspif` 入力では使えない) |
| `--node-max N` | KyotoDD のノード表を N ノードまでに制限する。超えるとメモリを使い切る代わりに明確なエラーで終了(既定は無制限) |
| `--stats` | ノード数と所要時間を標準エラー出力に表示 |
| `-q`, `--quiet` | `answer sets: N` と `optimum: ...` の行を表示しない |
| `--json` | 結果を 1 つの JSON オブジェクトで出力する(`count`、`optimum` と、他のオプションで求めた `brave` / `cautious` / `frequencies` / `cost_histogram` / `stats` / `answer_sets`。`--by-cost` では各解が `{"cost": [..], "atoms": [..]}`。`-q` と `--count` は無視) |

### 安定モデル以外の族を作る(`--semantics`)

`--semantics supported` は Clark 完備化のモデル(supported model: 真の各原子に、それを頭部に含み、
本体が真で、他の頭部原子が偽のルールがある。`#external` の自由な原子は常に支持されている)、
`--semantics classical` はルールを含意 `本体 → 頭部` として読んだ古典モデル(選択ルールは何も課さない)
の族を作ります。要約行は `supported models:` / `classical models:` になり、列挙・射影・`#minimize`・
帰結・保存 DD はその族に対して働きます。

```sh
./build/ddasp --gringo "$G" examples/loop.lp                        # answer sets: 2
./build/ddasp --gringo "$G" --semantics supported examples/loop.lp   # supported models: 3  ({a, b} は自己支持)
./build/ddasp --gringo "$G" --semantics classical examples/loop.lp   # classical models: 3
```

注意: どの意味論も gringo が出力したグラウンドプログラムに対するものです。集約や `not not` の
書き換えで gringo が導入する補助原子も普通の原子として扱われるので、supported / classical の
個数は元のプログラムを手で解釈したものと違うことがあります。

### 解をランダムに取り出す(`--sample`)

解が多すぎて列挙できないときは、`--sample N` で一様ランダムに N 個取り出せます。ZDD からは
一様サンプリングが(個数の計算と同じ 1 回の走査で)厳密にできるので、ASP ソルバでは難しい
「解の一様サンプル」が簡単に得られます。既定は復元抽出(独立な N 回の抽出)で、`--distinct` を
付けると重複しない N 個になります。`--seed S` で再現できます。

```sh
./build/ddasp --gringo "$G" --sample 3 --seed 1 examples/queens.lp
./build/ddasp --gringo "$G" --sample 3 --distinct --seed 1 examples/coloring.lp
```

確率は射影後の表示集合ごとに等しくなります。隠れた原子の数で重み付けしたい(元の解集合ごとに
等確率にしたい)ときは `--all-atoms` を付けます。Python では `res.sample(k, seed=, distinct=)` です。

### 解全体の性質を調べる(`--brave` / `--cautious` / `--frequencies`)

解集合の族が ZDD として手元にあるので、「どれかの解に入る原子」(勇敢帰結)、「すべての解に
入る原子」(慎重帰結)、「各原子が何個の解に入るか」は、解を列挙せずに正確に求まります。
6 クイーンでは、角のマス `q(1,1)` などはどの解にも使われず、`q(1,2)` は 4 解中 1 解に現れます。

```sh
./build/ddasp --gringo "$G" --cautious --frequencies examples/queens.lp
```

```
answer sets: 4

q(1,1) 0
q(1,2) 1
q(1,3) 1
...
```

2 行目の空行が `--cautious` の出力です(すべての解に共通する原子は無い)。

`--brave` と `--cautious` の行は `--enumerate` の 1 行と同じ形式(事実名、原子名、`#show` の項を
変数順に並べたもの)なので、そのまま別のプログラムの入力にできます。`--frequencies` の個数は
射影後の解集合に対するものです(`--all-atoms` なら全解集合)。`-q` を付けても表示されます。
Python では `res.brave`、`res.cautious`、`res.frequencies` で同じものが得られます(10 章)。

### JSON で出力する(`--json`)

スクリプトから結果を読むときは `--json` を付けます。標準出力が 1 つの JSON オブジェクトになり、
`count`(解の個数)、`optimum`(`--opt` や `+d` 付きの境界を使ったとき)と、他のオプションで求めた
項目(`--brave` → `brave`、`--cautious` → `cautious`、`--frequencies` → `frequencies`、
`--cost-histogram` → `cost_histogram`、`--stats` → `stats`、`--enumerate` → `answer_sets`)だけが
入ります。キーは Python の `Result` の属性名と同じです。解は見つかった順に書き出されるので、
大きな族でも `--limit` と組み合わせて使えます。

```sh
./build/ddasp --gringo "$G" --json --enumerate --limit 2 --frequencies examples/choice.lp
```

```json
{
  "count": 7,
  "frequencies": {"a": 3, "b": 3, "c": 3},
  "answer_sets": [
    [],
    ["c"]
  ]
}
```

`--by-cost` では各解が `{"cost": [..], "atoms": [..]}` になり、`--cost-histogram` は
`[{"cost": [..], "count": N}, ...]` です。個数は JSON の整数のまま(桁数の制限なし)書きます。

### ZDD をファイルに保存する

```sh
./build/ddasp --gringo "$G" -o choice.zdd --atoms atoms.txt examples/choice.lp
cat atoms.txt
```

```
# var level atom name
3 3 1 a
2 2 2 b
1 1 3 c
```

バイナリファイルには DD の **レベル** が記録されます。`atoms.txt` を見ると、
どのレベルがどの原子に対応するかが分かります。別の KyotoDD プロセスで読み込むと、
変数番号 = レベルになります。

### DD を保存して、別の ASP プログラムから再利用する(`--save` / `--load`)

一度コンパイルした解集合の族を **保存 DD** として 1 つのファイルにしておくと、
別のプログラムのコンパイルで読み込んで、(a) 追加の制約で絞り込んだり、(b) 新しい原子を
導出したりできます。例として、グラフ彩色の全解を保存してから、頂点 1 を赤以外に制限し、
赤い頂点を表す述語 `red/1` を付け加えます(`examples/coloring_fix.lp`)。

```sh
./build/ddasp --gringo "$G" --save coloring.ddasp examples/coloring.lp
./build/ddasp --gringo "$G" --load coloring.ddasp --enumerate examples/coloring_fix.lp
```

```
% examples/coloring_fix.lp
:- col(1,r).            % 頂点 1 は赤ではない
red(N) :- col(N,r).     % 保存した原子から新しい原子を導出
#show red/1.
```

```
answer sets: 12
col(1,g) col(2,r) col(3,b) col(4,r) col(5,b) red(2) red(4)
...
```

`--load FILE` は、新しいプログラムに次を書き足すのと同じ意味です。

1. 保存 DD の各原子 `n` について `#external n.` と `#show n : n.`
2. 保存 DD の各事実(常に表示される名前)`f` について `f.`
3. 制約「解集合を保存 DD の原子に制限した集合は、保存した族に含まれる」

ですから、`#external` を自分で書く必要はなく、保存した原子をそのまま本体や制約に使えます。
結果は「元のプログラム ∪ 新しいプログラム」の解集合を「保存した原子 ∪ 新しいプログラムの
表示原子」へ射影したものに一致します(保存 DD は `#show` で射影済みなので、元のプログラムで
隠した原子は見えません)。保存した原子は新しいプログラムの `#show` に関係なく常に結果に残り、
新しいプログラムの `#show` は新しいプログラム自身の原子だけを左右します。
新しいプログラムで使わなかった保存原子も、自由変数(保存した族の制約だけを受ける)として残ります。

次の場合はエラーになります。

- 新しいプログラムが保存した原子を **定義** している(ルールの頭部に書く、事実として書く)。
  保存した原子は本体と制約にだけ使えます。
- 新しいプログラムの中で同じ名前が複数の原子に付いている、保存した 2 つの原子が同じ原子を指す、
  ある名前が片方では原子で片方では事実になっている。
- `--save` 時に、変数に名前がない(`--all-atoms` で隠れた原子が残っている)、同じ名前の原子が
  2 つある、名前が原子として書けない(`#show 1 : a.` など)。`#show` の term(第 2 の名前や
  `not` 付きの条件)は保存されず、警告が出ます。

`--load` は何度でも指定でき(原子が重なっていてもよい)、`--load` した結果をまた `--save` できます。
`#minimize` のコストは保存されませんが、新しいプログラムで保存した原子に `#minimize` を書けば
`--opt` が使えます。`.aspif` 入力には宣言を注入しないので、使う保存原子は aspif 側で
`#external` と `#show` を書いておく必要があります(使わない保存原子は自動で補われます)。

制限: 読み込める DD は KyotoDD の import 上限(1000 万ノード、100 万レベル)まで。
BDD 変数の上限 65536 は「保存 DD の原子数 + 新しいプログラムの変数数」で共有します。
保存ファイルは信頼できる入力として扱われます(原子名がそのままプログラム文として
グラウンダに渡されます)。

まとまった例題は 12 章にあります(DDASP で作った DD に 2 段階目の ASP を重ねる例と、
KyotoDD で直接作った DD を ASP から使う例)。

### グラウンディング済みファイルを使う

gringo の出力(aspif 形式)を保存しておけば、同じ入力を何度もグラウンディングせずに済みます。

```sh
$G --output=intermediate examples/choice.lp > choice.aspif
./build/ddasp choice.aspif
```

---

## 10. Python から使う

`ddasp` パッケージを使うと、グラウンディング(clingo)からコンパイルまでを Python の中で行えます。

```python
import ddasp

res = ddasp.compile_text("{ a; b }. :- a, b. #show a/0. #show b/0.")
res.count                     # 3
list(res.answer_sets())       # [frozenset(), frozenset({'a'}), frozenset({'b'})]
res.names                     # {1: 'a', 2: 'b'}   原子番号 -> 名前
res.levels                    # [2, 1]             DD の上から順のレベル
```

ファイルから読む場合と、最適化する場合:

```python
res = ddasp.compile_files(["examples/queens.lp"])
res.count                     # 4

res = ddasp.compile_files(["examples/minimize.lp"], optimize=True)
res.count, res.optimum        # (1, [1])

res = ddasp.compile_files(["examples/vertex_cover.lp"], cost_slack=1)
res.count, res.optimum        # (14, [4])   最適値 + 1 まで(--cost-le +1 相当)

res = ddasp.compile_files(["examples/vertex_cover.lp"], cost_histogram=True)
res.cost_histogram            # {(4,): 8, (5,): 6, (6,): 1}  コストごとの解の個数
next(res.answer_sets(by_cost=True))   # ((4,), frozenset({'in(2)', ...}))  コストの小さい順に (コスト, 解)
ddasp.compile_files(["examples/vertex_cover.lp"], best=3).best   # 上位 3 解の [(コスト, 解), ...]
```

`answer_sets(by_cost=True)` は射影後の ZDD を kyotodd で辿るので、重みを持つ原子がすべて表示されて
いる(または `project=False`)必要があります。隠れた原子に重みがあるときは `compile(best=K)` を使います。

`compile_text` / `compile_files` の主なキーワード引数:

| 引数 | 意味 |
|---|---|
| `clingo_args=["--const", "n=8"]` | clingo に渡す引数(定数の上書きなど) |
| `method="scc"` | `"scc"`(既定)または `"twocopy"` |
| `semantics="stable"` | `"supported"`(Clark 完備化のモデル)または `"classical"`(古典モデル)で `--semantics` 相当 |
| `order="name"` | `"name"`、`"id"`、`"force"`(FORCE ヒューリスティック)、または上に置く原子番号のリスト |
| `project=True` | `False` にすると `--all-atoms` 相当、`"project"` にすると `--project` 相当(`#project` の原子へ射影) |
| `optimize=False` | `True` で `--opt` 相当 |
| `cost_le=None` | `--cost-le` 相当。整数(先頭の優先度の上限)または優先度の高い順の整数リスト |
| `cost_ge=None`, `cost_eq=None` | `--cost-ge` / `--cost-eq` 相当(同じ形式) |
| `cost_slack=None` | `--cost-le +d` 相当。最適値からの余裕 `d`(非負の整数またはリスト)。`res.optimum` も埋まる |
| `cost_histogram=False` | `True` で `--cost-histogram` 相当。`res.cost_histogram` が `{(コスト, ...): 個数}` の dict(昇順。指定しなければ `None`) |
| `best=None` | 整数 K で、コストの小さい順に K 個の解を `res.best`(`(コスト, frozenset)` のリスト)に入れる(`--by-cost --limit K` 相当。隠れた原子に重みがあっても可) |
| `external_values=False` | `True` で `--external-values` 相当(`#external` の初期値で固定) |
| `dds=[...]` | `--load` 相当。読み込む保存 DD(`StoredDD`、`Result`、ファイルパス、bytes)のリスト |
| `node_max=None` | `--node-max` 相当。ノード数の上限(超えると `ddasp.NodeLimitError`)。`None` で無制限 |

`res.stats` にはノード数や時間の統計が入ります。勇敢帰結・慎重帰結・出現回数(9 章)は
`Result` の属性です。

```python
res = ddasp.compile_files(["examples/queens.lp"])
res.brave                     # frozenset({'q(1,2)', 'q(1,3)', ...})  どれかの解に入る原子
res.cautious                  # frozenset()                          すべての解に入る原子
res.frequencies               # {'q(1,1)': 0, 'q(1,2)': 1, ...}      原子ごとの解の個数(表示順)
res.sample(5, seed=1)         # 一様ランダムな 5 個の解(frozenset のリスト。distinct=True で重複なし)

try:
    big = ddasp.compile_files(["big.lp"], node_max=50_000_000)
except ddasp.NodeLimitError:
    ...                       # 変数順序や #show を見直す
```

保存 DD(9 章の `--save` / `--load`)は Python でも同じように使えます。

```python
old = ddasp.compile_files(["examples/coloring.lp"])
old.save("coloring.ddasp")                      # ファイルに保存(old.stored() なら StoredDD オブジェクト)
d = ddasp.load("coloring.ddasp")                # d.names, d.fact_names, d.count, d.answer_sets()
new = ddasp.compile_files(["examples/coloring_fix.lp"], dds=[d])   # dds=[old] でも同じ
new.count                                       # 12
```

KyotoDD で直接作った `kyotodd.ZDD` も、`ddasp.StoredDD.from_zdd(zdd, names)` で原子名を付ければ
保存 DD になり、同じように `dds=` に渡せます(12.2 節)。

`compile_text` / `compile_files` / `compile_control` は、保存 DD の宣言(`#external` など)を
プログラム部 `ddasp_loaded` として一緒にグラウンディングします。`compile(program, dds=...)` に
グラウンディング済みの `Program` を渡す場合は宣言を注入しないので、使う保存原子は
`Program` の中で `#external` かつ名前付きでなければなりません。

---

## 11. ZDD を KyotoDD で活用する

`res.zdd` は `kyotodd.ZDD` オブジェクトです。ここからが DDASP の本領で、
解の族に対して KyotoDD の演算をそのまま使えます。

```python
import ddasp, kyotodd

res = ddasp.compile_files(["examples/queens.lp"])
z = res.zdd

z.exact_count          # 4        解の個数(Python の多倍長整数)
z.raw_size             # 24       ZDD のノード数

# 変数番号 -> 原子名 の対応
name_of = {kyotodd.to_var(lv): res.level_name(lv) for lv in res.levels}

# 解を列挙(各解は変数番号のリスト)
for s in z.enumerate():
    print(sorted(name_of[v] for v in s))

# 一様ランダムに 1 つの解を取り出す
sample = z.uniform_sample()
print(sorted(name_of[v] for v in sample))

# 「q(1,2) を含む解」だけに絞り込む
atom = next(a for a, n in res.names.items() if n == "q(1,2)")
v = kyotodd.to_var(res.atom_to_level[atom])
z.onset(v).exact_count # 1

# ZDD の図を SVG で保存
with open("queens.svg", "w") as f:
    z.save_svg_file(f)
```

このほか、`z.intersec(...)`、`z.minimal()`、重み付きのサンプリングや
k 番目の解の取得(`unrank`)など、KyotoDD の ZDD 演算が一通り使えます。
詳しくは `third_party/KyotoDD/README.md` と Python のスタブ
`third_party/KyotoDD/python/src/kyotodd/_core.pyi` を参照してください。

---

## 12. DD を ASP から使う(2 段階のコンパイル)

9 章の `--save` / `--load` は「ASP プログラム → DD」の一方向だけでなく、
**DD を ASP プログラムから使う** 方向にも使えます。保存 DD の原子は新しいプログラムの中で
`#external` 原子として宣言されるので、ASP 側は保存された原子を本体や制約に書くだけです。
ここでは 2 つの流れを例題で見ます。

- 12.1: DDASP で作った DD に、別の ASP プログラムで制約と最適化を重ねる。
- 12.2: KyotoDD で直接作った族(DD)を、ASP プログラムの制約として使う。

### 12.1 DDASP で作った DD をさらに ASP から使う(`examples/independent_set_maximal.lp`)

8.2 節の独立集合(4×4 格子で 1234 個)の DD を保存し、2 段階目のプログラムで
**極大** 独立集合だけに絞ります。極大とは「選ばれていない頂点はどれも、選ばれた頂点に
隣接している」ことです。

```prolog
#const grid = 4.
vertex((X,Y)) :- X = 1..grid, Y = 1..grid.
adj((X,Y),(X+1,Y)) :- vertex((X,Y)), X < grid.
adj((X,Y),(X,Y+1)) :- vertex((X,Y)), Y < grid.
blocked(V) :- adj(U,V), in(U).
blocked(U) :- adj(U,V), in(V).
:- vertex(V), not in(V), not blocked(V).
#minimize { 1,V : in(V) }.      % --opt を付けると最小の極大独立集合だけになる
#show in/1.
```

`in/1` は保存 DD の原子です。本体(`blocked` の定義、制約、`#minimize`)には自由に使えますが、
頭部に書く(定義する)とエラーになります。1 段階目で `#show` していない `vertex/1` と `adj/2`
は保存されていないので、2 段階目で改めて定義しています(定数を `n` でなく `grid` と
しているのは、2 つのファイルを 1 つのプログラムとして clingo に渡したときに定数が
二重定義にならないようにするためです)。

```sh
./build/ddasp --gringo "$G" --save is.ddasp examples/independent_set.lp           # answer sets: 1234
./build/ddasp --gringo "$G" --load is.ddasp examples/independent_set_maximal.lp   # answer sets: 42
./build/ddasp --gringo "$G" --load is.ddasp --opt --enumerate examples/independent_set_maximal.lp
```

```
answer sets: 2
optimum: 4
in((1,3)) in((2,1)) in((3,4)) in((4,2))
in((1,2)) in((2,4)) in((3,1)) in((4,3))
```

1234 個の独立集合のうち極大なものは 42 個、そのうち最小のもの(独立支配集合)は 4 頂点で
2 つあります。clingo に 2 つのファイルを一緒に渡した結果と一致します。2 段階目は 1 段階目の
計算をやり直さず、保存した族を制約として掛け合わせるだけなので、1 段階目の DD が大きくても
2 段階目は軽く済みます。

Python では `Result` をそのまま `dds=` に渡せます。

```python
import ddasp

first = ddasp.compile_files(["examples/independent_set.lp"])                 # first.count == 1234
maximal = ddasp.compile_files(["examples/independent_set_maximal.lp"], dds=[first])
maximal.count                                                                 # 42
smallest = ddasp.compile_files(["examples/independent_set_maximal.lp"], dds=[first], optimize=True)
smallest.count, smallest.optimum                                              # (2, [4])
```

2 段階目で `size(S) :- S = #count { V : in(V) }. #show size/1.` と書けば、各極大独立集合に
その大きさを付けることもできます(16 頂点なら一瞬で終わります)。

### 12.2 KyotoDD で作った DD を ASP から使う(`examples/shift_dd.py`, `examples/shift.lp`)

今度は DD を ASP からではなく KyotoDD の族演算で直接作り、それを ASP の制約として使います。
題材は 1 週間の勤務パターンです。「働く日の集合」を、`mon`〜`sun` を変数とする ZDD で表し、
「週 4 日以下」と「3 連勤なし」を族の演算で組み立てます(`examples/shift_dd.py`)。

```python
import ddasp, kyotodd

days = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]

kyotodd.init()
var = dict(zip(days, kyotodd.new_vars(len(days))))        # 1 日 = 1 変数

family = kyotodd.ZDD.power_set(len(days)).size_le(4)      # 週 4 日以下
for i in range(len(days) - 2):                            # 3 連勤なし
    three = kyotodd.ZDD.single_set([var[days[i]], var[days[i + 1]], var[days[i + 2]]])
    family = family - family.restrict(three)              # restrict(g): g の集合を含む集合だけ

dd = ddasp.StoredDD.from_zdd(family, {var[d]: f"work({d})" for d in days})
dd.count                                                  # 78
dd.save("shift.ddasp")
```

`StoredDD.from_zdd(zdd, names)` が ZDD を保存 DD にします。`names` は「変数番号 → 原子名」の
辞書(または変数 1, 2, ... の名前を順に並べたリスト)です。名前を付けた変数が kyotodd の
レベル 1..N を占めていること(新しいインスタンスで `kyotodd.new_vars(N)` した直後の状態が
そうです)と、ZDD に現れる変数すべてに名前があることが条件で、名前は ASP の原子として書ける形
(小文字または `_` で始まる)で重複がないこと。`fact_names=[...]` で常に表示される事実も
付けられます。

ASP 側(`examples/shift.lp`)は、保存された `work/1` を使って制約を足し、勤務日数を導出します。

```prolog
:- not work(sat), not work(sun).            % 土日のどちらかは出勤
days(N) :- N = #count { D : work(D) }.      % 勤務日数を導出
#show days/1.
```

```sh
.venv/bin/python examples/shift_dd.py shift.ddasp
./build/ddasp --gringo "$G" --load shift.ddasp --enumerate examples/shift.lp
```

```
78 work patterns, 7 atoms: work(sun) work(sat) work(fri) work(thu) work(wed) work(tue) work(mon)
answer sets: 54
days(4) work(sat) work(thu) work(tue) work(mon)
days(4) work(sat) work(thu) work(wed) work(mon)
...
```

78 個の勤務パターンのうち土日のどちらかを含むものが 54 個で、各解に勤務日数 `days/1` が
付いています。Python だけで済ませるなら、ファイルに保存せず `dd` をそのまま渡します。

```python
res = ddasp.compile_files(["examples/shift.lp"], dds=[dd])
res.count                                                 # 54
```

ZDD の出どころは何でも構いません。KyotoDD の演算で作った族のほかに、フロンティア法などで
列挙したグラフの部分構造(パス、全域木、...)や Graphillion 形式のファイル
(`kyotodd.ZDD.import_graphillion_str`)から読み込んだ族も、同じ手順で ASP の制約として使えます。
「DD で表しやすい大域的な制約」と「ASP で書きやすいルール」を組み合わせるのが、この使い方の狙いです。

---

## 13. よくある質問とトラブルシューティング

**Q. `answer sets: 0` と表示される。**
プログラムに解がありません(clingo でいう UNSATISFIABLE)。制約が厳しすぎないか確認してください。

**Q. `--enumerate` で同じ行が何度も出る。**
`#show` で隠した原子だけが異なる解が複数あるためです。既定の射影(`#show` の原子だけを
ZDD の変数にする)では同一視されて 1 つになります。`--all-atoms` を使っている場合は
隠れた原子も変数になるので、同じ表示の行が複数出ることがあります。

**Q. `ddasp: unknown option` や `gringo` が見つからない。**
`--gringo "$G"` の指定を確認してください。`G` の中身は
`.venv/bin/python -m clingo --mode=gringo` です(引用符で囲んで 1 つの引数として渡します)。

**Q. 大きな問題で時間がかかる、メモリが足りない。**
ZDD の大きさは **変数順序** に強く依存します。まず `--order force`(FORCE ヒューリスティック)を
試し、`--order FILE` で原子名を上から順に並べたファイルを渡せば順序を手動で調整できます。
`--stats` で `order span`、`zdd nodes`、`peak table nodes` を見ながら試してください。また、`#show` で必要な原子だけを残すと
ZDD は小さくなります。なお、グラウンディング後の原子数には上限があります
(KyotoDD の BDD 変数は 65536 個まで。原子 1 つに変数 1 つ、正のループに入る原子は 2 つ)。
ノード数は既定では無制限で、巨大な中間 BDD はマシンのメモリを使い切ります。
`--node-max N`(Python では `node_max=N`)を付けると、N ノードを超えた時点で明確なエラーで
止まるので、まず上限付きで試すと安全です。

**Q. 一度作った DD を別のプログラムから使いたい。**
`--save FILE` で保存し、別のプログラムのコンパイル時に `--load FILE` を付けます
(9 章「DD を保存して、別の ASP プログラムから再利用する」、例題は 12 章)。保存した原子は
新しいプログラムの `#external` 原子になるので、本体や制約にそのまま書けます(頭部に書くとエラー)。
KyotoDD で直接作った ZDD も `StoredDD.from_zdd` で同じように使えます(12.2 節)。

**Q. `#count` / `#sum` / 選言ルール(`a | b :- ...`)は使えますか?**
はい。通常ルール、選択ルール、制約、選言ルール、`#count` / `#sum` を含む本体、
`#show`、`#minimize`、`#external`(初期値は `--external-values` を付けたときだけ使う。ルールの頭部に
現れた外部原子は clingo と同様に外部原子でなくなるが、`a :- a, B` のような恒真ルールは数えない)、
`#project`(`--project` を付けたときだけ使う)、assumption(aspif の 6 文、常に制約として使う)に
対応しています。`#heuristic`、edge、theory は警告を出して無視します。

**Q. 最適解だけでなく、最適値に近い解も見たい。**
`--cost-le +1` のように `+d` で「最適値 + d 以下」の解が残ります(`optimum:` 行も表示)。
絶対値なら `--cost-le 5`、下限や等号は `--cost-ge` / `--cost-eq` です(7 章)。Python では
`cost_slack=1` / `cost_le=5` などを `compile_text` / `compile_files` に渡します(10 章)。
コストごとの解の個数は `--cost-histogram`(Python では `cost_histogram=True`)で分かります。
上位 K 解だけを見るなら `--by-cost --limit K`(Python では `answer_sets(by_cost=True)` か `compile(best=K)`)です。

**Q. 仕組みをもっと知りたい。**
[docs/DESIGN.md](DESIGN.md) に、安定モデル意味論を BDD で構成する方法
(SCC ごとの非根拠集合の排除)が書かれています。
