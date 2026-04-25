# micro Scheme 9 解説

> 2026-04-25 okadan
> SECD 仮想マシンベースの Micro Scheme 実装ノート
> micro_scheme8.cpp をベースに VECTOR 型を追加した版

---

## 1. 概要

micro_scheme9 は micro_scheme8 を土台として、ベクタ型を追加した実装である。
主な狙いは、リスト中心の処理に加えて、添字アクセスとソートを高速に試せることにある。

今回の追加要素は次のとおり。

- Object の型に VECTOR を追加
- リーダに #(...) ベクタリテラルを追加
- ベクタ関連プリミティブを追加
- GC の mark フェーズでベクタ要素を追跡

この設計により、ベクタは GC の管理対象として扱える。
手動 delete の導入は不要である。

---

## 2. 追加されたデータモデル

### 2.1 Type 列挙

Type 列挙に VECTOR を追加している。

- 既存: INT, INT64, DOUBLE, SYMBOL, PAIR, CLOSURE, CONTINUATION, PRIMITIVE, PORT, UNDEF
- 追加: VECTOR

### 2.2 Object のペイロード

Object の union に以下を追加。

- std::vector<Object*>* vec

コンストラクタでは VECTOR 時に vec=nullptr で初期化し、
デストラクタでは VECTOR 時に delete vec を行う。

### 2.3 表示と等値

- print_obj_stream で VECTOR を #(a b c) の形式で表示
- objects_equal で VECTOR 同士の深い比較を実施
  - 長さ一致
  - 各要素を objects_equal で比較

---

## 3. GC との関係

### 3.1 mark の拡張

mark(Object*) に VECTOR 分岐を追加している。

- obj->vec が存在する場合、全要素に対して mark を再帰適用

これにより、ベクタ経由で到達可能な Pair や Closure なども正しく保護される。

### 3.2 sweep の挙動

sweep 自体は既存と同じで、未マーク Object を delete する。
VECTOR は Object デストラクタで vec 本体が解放されるため、
メモリ解放経路は既存設計に自然に統合されている。

---

## 4. リーダ拡張

### 4.1 字句解析

tokenize に #(... ) を認識するトークン TOK_HASH_LPAREN を追加。

- 入力 #(... で TOK_HASH_LPAREN を生成
- 通常の ( は TOK_LPAREN のまま

### 4.2 構文解析

s_read に TOK_HASH_LPAREN の分岐を追加し、s_read_vector を呼び出す。

s_read_vector は次を行う。

1. ) まで要素を順に s_read で読み取る
2. 要素を std::vector<Object*> に格納
3. VECTOR Object を alloc して vec に格納

読み取り中の要素は、既存のリーダ設計に合わせて一時的に VM スタックへ積み、
GC から保護している。

---

## 5. 追加プリミティブ一覧

### 5.1 生成・判定

- make-vector
  - 形式: (make-vector n [fill])
  - n 要素のベクタを作る。fill 省略時は nil。
- vector
  - 形式: (vector e0 e1 ...)
  - 引数列からベクタを作る。
- vector?
  - 形式: (vector? obj)
  - obj がベクタなら true。

### 5.2 参照・更新

- vector-length
  - 形式: (vector-length v)
  - 長さを返す。
- vector-ref
  - 形式: (vector-ref v i)
  - i 番目を返す。範囲外は VMError。
- vector-set!
  - 形式: (vector-set! v i x)
  - i 番目に x を設定し x を返す。範囲外は VMError。
- vector-fill!
  - 形式: (vector-fill! v fill)
  - 全要素を fill で上書きし v を返す。
- vector-copy
  - 形式: (vector-copy v)
  - シャローコピーを返す。

### 5.3 変換

- vector->list
  - 形式: (vector->list v)
  - 要素順を保ってリスト化。
- list->vector
  - 形式: (list->vector lst)
  - リストをベクタ化。

### 5.4 高階関数

- vector-sort
  - 形式: (vector-sort pred v)
  - v を壊さず、ソート済みコピーを返す。
- vector-sort!
  - 形式: (vector-sort! pred v)
  - v 自体を破壊的にソートして返す。
- vector-map
  - 形式: (vector-map proc v)
  - 各要素へ proc を適用した新しいベクタを返す。
- vector-for-each
  - 形式: (vector-for-each proc v)
  - 副作用目的で proc を各要素へ適用し、nil を返す。

補足:

- sort は std::stable_sort を利用
- 比較は pred に (a b) を渡し、戻り値が true なら a を先に並べる

---

## 6. 実行例

### 6.1 リテラルと基本操作

```scheme
(define v #(3 1 4 1 5))
(vector-length v)          ; => 5
(vector-ref v 2)           ; => 4
(vector-set! v 2 99)       ; => 99
v                          ; => #(3 1 99 1 5)
```

### 6.2 変換

```scheme
(vector->list #(10 20 30)) ; => (10 20 30)
(list->vector '(a b c))    ; => #(a b c)
```

### 6.3 ソート

```scheme
(define u #(3 1 4 1 5 9 2 6))
(vector-sort < u)          ; => #(1 1 2 3 4 5 6 9)
u                          ; => #(3 1 4 1 5 9 2 6)

(vector-sort! < u)
u                          ; => #(1 1 2 3 4 5 6 9)
```

### 6.4 map/for-each

```scheme
(vector-map (lambda (x) (* x 2)) #(1 2 3))
; => #(2 4 6)

(vector-for-each (lambda (x) (display x) (newline)) #(10 20 30))
; 10
; 20
; 30
```

---

## 7. 確認用スクリプト

同梱の test_vector9.scm で次をまとめて確認できる。

- 生成、参照、更新
- リテラル #(...)
- list との相互変換
- copy/fill
- sort/sort!
- map/for-each
- 100 要素ソート

実行例:

```powershell
echo '(load "test_vector9.scm")' | .\micro_scheme9.exe
```

---

## 8. 現時点の注意点

1. 文字列型は未実装

- 現実装では "abc" を独立した文字列オブジェクトとして扱わない。
- display のラベル文字列は意図どおりに出力されない場合がある。

2. インデックス型

- vector-ref / vector-set! のインデックスは INT を想定している。
- INT64 や DOUBLE を直接添字に渡すと失敗する。

3. sort の比較関数

- pred は厳密な順序関係を満たす関数を使うこと。
- 非推移な比較関数を渡すと期待しない順序になる。

---

## 9. 今後の拡張候補

- 文字列型の導入と display 体験の改善
- vector-append, vector-take, vector-drop の追加
- list / vector 共通の高階 API 層の整理
- ベンチマーク用の連続アクセス・ランダムアクセス比較

---

## 10. まとめ

micro_scheme9 は、micro_scheme8 の SECD/GC 設計を維持したまま、
ベクタ型を追加して配列的なワークロードを扱えるようにした版である。

とくに次の点が重要である。

- ベクタは GC に統合済みであり、手動管理は不要
- #(...) リテラルによりテスト記述性が向上
- vector-sort / vector-sort! により実用的な並べ替えが可能

これにより、リスト中心の処理だけでなく、
添字アクセスを活かしたアルゴリズム検証が容易になった。
