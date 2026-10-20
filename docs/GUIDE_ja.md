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
7. [最適化(#minimize)](#7-最適化-minimize)
8. [コマンドラインオプション一覧](#8-コマンドラインオプション一覧)
9. [Python から使う](#9-python-から使う)
10. [ZDD を KyotoDD で活用する](#10-zdd-を-kyotodd-で活用する)
11. [よくある質問とトラブルシューティング](#11-よくある質問とトラブルシューティング)

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

Linux(または macOS)で、C++ コンパイラ、CMake(3.14 以上)、Python 3 が
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
c b
a
c a
b a
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

c b a
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

6 クイーンの解は 4 つです。`--const n=8` をグラウンダに渡すと 8 クイーンになります。

```sh
./build/ddasp --gringo "$G --const n=8" examples/queens.lp     # answer sets: 92
./build/ddasp --gringo "$G --const n=10" examples/queens.lp    # answer sets: 724
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
col(5,g) col(4,r) col(3,b) col(2,g) col(1,r)
col(5,b) col(4,r) col(3,b) col(2,g) col(1,r)
col(5,b) col(4,g) col(3,b) col(2,g) col(1,r)
```

`--limit 3` で最初の 3 個だけを表示しています。個数(18)はすべての解を
数えた値です。

---

## 7. 最適化(#minimize)

`#minimize` で解のコストを定義できます。次のプログラムを `opt.lp` として保存してください。

```prolog
{ a ; b ; c }.
:- not a, not b.                              % a か b の少なくとも一方は必要
#minimize { 1,a : a ; 2,b : b ; 1,c : c }.    % a のコスト 1, b は 2, c は 1
#show a/0. #show b/0. #show c/0.
```

`--opt` を付けないと、コストに関係なくすべての解が対象です。

```sh
./build/ddasp --gringo "$G" --enumerate opt.lp
```

```
answer sets: 6
b
c b
a
c a
b a
c b a
```

`--opt` を付けると、コストが最小の解だけが残ります。

```sh
./build/ddasp --gringo "$G" --opt --enumerate opt.lp
```

```
answer sets: 1
optimum: 1
a
```

---

## 8. コマンドラインオプション一覧

```
ddasp [options] FILE...
```

`FILE` が `.aspif` なら、グラウンディング済みとしてそのまま読みます。それ以外は
`--gringo` のコマンドでグラウンディングしてから読みます。

| オプション | 意味 |
|---|---|
| `--count` | 解集合の個数を表示(既定の動作) |
| `--enumerate` | すべての解集合を 1 行ずつ表示 |
| `--limit N` | `--enumerate` で最大 N 個だけ表示 |
| `-o FILE` | ZDD を KyotoDD バイナリ形式で FILE に保存 |
| `--bdd` | `-o` で ZDD の代わりに BDD を保存 |
| `--atoms FILE` | 変数番号 / レベル / 原子番号 / 原子名の対応表を FILE に保存 |
| `--all-atoms` | `#show` による射影をせず、すべての原子を DD の変数にする |
| `--opt` | `#minimize` の最適解だけを残す |
| `--method scc\|twocopy` | 安定モデルの構成法(既定 `scc`、`twocopy` は参照実装) |
| `--order name\|id\|FILE` | 変数順序(既定 `name`。FILE は上から順に並べる原子名の一覧) |
| `--gringo CMD` | グラウンダのコマンド(`CMD --output=intermediate FILE...` として実行) |
| `--stats` | ノード数と所要時間を標準エラー出力に表示 |
| `-q`, `--quiet` | `answer sets: N` の行を表示しない |

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

### グラウンディング済みファイルを使う

gringo の出力(aspif 形式)を保存しておけば、同じ入力を何度もグラウンディングせずに済みます。

```sh
$G --output=intermediate examples/choice.lp > choice.aspif
./build/ddasp choice.aspif
```

---

## 9. Python から使う

`ddasp` パッケージを使うと、グラウンディング(clingo)からコンパイルまでを Python の中で行えます。

```python
import ddasp

res = ddasp.compile_text("{ a; b }. :- a, b. #show a/0. #show b/0.")
res.count                     # 3
list(res.answer_sets())       # [frozenset(), frozenset({'b'}), frozenset({'a'})]
res.names                     # {1: 'a', 2: 'b'}   原子番号 -> 名前
res.levels                    # [2, 1]             DD の上から順のレベル
```

ファイルから読む場合と、最適化する場合:

```python
res = ddasp.compile_files(["examples/queens.lp"])
res.count                     # 4

res = ddasp.compile_files(["opt.lp"], optimize=True)
res.count, res.optimum        # (1, [1])
```

`compile_text` / `compile_files` の主なキーワード引数:

| 引数 | 意味 |
|---|---|
| `clingo_args=["--const", "n=8"]` | clingo に渡す引数(定数の上書きなど) |
| `method="scc"` | `"scc"`(既定)または `"twocopy"` |
| `order="name"` | `"name"`、`"id"`、または上に置く原子番号のリスト |
| `project=True` | `False` にすると `--all-atoms` 相当 |
| `optimize=False` | `True` で `--opt` 相当 |

`res.stats` にはノード数や時間の統計が入ります。

---

## 10. ZDD を KyotoDD で活用する

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

## 11. よくある質問とトラブルシューティング

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
ZDD の大きさは **変数順序** に強く依存します。`--order FILE` で原子名を上から順に
並べたファイルを渡すと、順序を手動で調整できます。`--stats` で `zdd nodes` と
`peak live nodes` を見ながら試してください。また、`#show` で必要な原子だけを残すと
ZDD は小さくなります。

**Q. `#count` / `#sum` / 選言ルール(`a | b :- ...`)は使えますか?**
はい。通常ルール、選択ルール、制約、選言ルール、`#count` / `#sum` を含む本体、
`#show`、`#minimize`、`#external` に対応しています。

**Q. 仕組みをもっと知りたい。**
[docs/DESIGN.md](DESIGN.md) に、安定モデル意味論を BDD で構成する方法
(SCC ごとの非根拠集合の排除)が書かれています。
