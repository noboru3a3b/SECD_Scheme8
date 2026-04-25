# micro Scheme 10 解説

最終更新: 2026-04-25
対象実装: micro_scheme10.cpp

---

## 1. 何を実装しているか

micro Scheme 10 は、次の 2 系統を実装した Scheme 処理系です。

- SECD 風 VM（バイトコード実行）
- 評価器（eval ベース実行）

基本方針は「VM で実行できる式は VM へ」「VM で扱いづらい式は評価器へフォールバック」です。
このため、コンパイル失敗は必ずしも異常なケースではなく、仕様上のフォールバックとして正しい動作であり得ます。

---

## 2. ビルドと実行

### 2.1 ビルド（Windows, g++）

```powershell
g++ -std=c++17 -O2 -o .\micro_scheme10.exe .\micro_scheme10.cpp
```

### 2.2 REPL 起動

```powershell
.\micro_scheme10.exe
```

### 2.3 終了

REPL で `exit` を入力するか EOF で終了します。

---

## 3. セルフテスト

### 3.1 総合テスト

```powershell
.\micro_scheme10.exe --selftest-full
```

主に言語機能の正当性を検証します（let/let*/letrec, macro, backquote, do, call/cc, vector など）。

### 3.2 評価経路テスト

```powershell
.\micro_scheme10.exe --selftest-eval
```

各式が VM 経路か評価器経路かをログ表示しつつ検証します。

### 3.3 mlib 有無の比較テスト

```powershell
.\micro_scheme10.exe --selftest-mlib-compare
```

同じ式群を「mlib なし/あり」で評価し、出力差分を比較します。

---

## 4. 言語機能の対応範囲

### 4.1 主な特殊形式

- quote
- backquote（unquote / unquote-splicing）
- if
- begin
- lambda
- define
- define-macro
- set!
- let
- let*
- letrec
- and
- or
- cond
- when
- unless
- case
- do
- call/cc

### 4.2 データ型

- 整数
- 文字列
- シンボル
- ペア（リスト）
- クロージャ
- 継続
- ポート
- ベクタ
- 特殊シンボル（:undef, :eof など）

---

## 5. 実行モデル（最重要）

### 5.0 設計の本質：コンパイラ優先・インタプリタ保証

本処理系は **コンパイラとインタプリタのハイブリッド** です。

```
ソースコード（S式）
    ↓
compile_subset_expr() でコンパイル試行
    ↓
  成功？
  YES → バイトコード列を生成 → VM が実行（高速）
  NO  → S式ツリーをそのまま保持 → eval_expr() が再帰走査（インタプリタ）
```

**「コンパイラ優先・インタプリタ保証」** という方針です。
大部分のコード（`let`, `letrec`, 通常 `lambda`, `if`, `begin` など）は
バイトコードにコンパイルして VM が高速実行します。
コンパイルが難しいエッジケースは、正確さ優先でインタプリタが処理します。
利用者からは同じ関数呼び出しに見えるため、フォールバックは透過的です。

#### フォールバックする主なケース

| ケース | 理由 |
|---|---|
| `(lambda (a . r) ...)` rest 引数 | 可変長引数の処理をコンパイラが非対応 |
| eval フレーム内の `set!` | `skip_vm_set` フラグで意図的に回避 |
| 一部のマクロ展開結果 | 展開後に再コンパイル試行するが失敗すると落ちる |

#### eval closure の構造

インタプリタ側は S 式ツリー（`Object*` の連結リスト）を直接たどります。
「eval closure」は **バイトコードを持たず、S 式の params と body を直接格納** したクロージャです：

```
eval closure  = { code: [],  constants: [params_expr, body_list] }
                      ^^^空               ^^^S式ツリーそのもの

VM  closure   = { code: [LD, JZ, ...バイトコード列], constants: [...] }
```

`invoke_callable()` は `closure.code->empty()` でこの 2 種類を自動判別し、
empty なら S 式を `eval_expr()` で逐次評価（インタプリタ動作）、
non-empty なら VM バイトコードを実行します。

#### 通常 lambda のバイトコード構造

rest 引数なしの lambda は **入れ子のバイトコード列** に変換されます：

```scheme
((lambda (x y) (+ x y)) 3 4)
```

外側の命令列：

```
LDC 3
LDC 4
LDF → 本体コード: [LD(0,0), LD(0,1), CALLG +, 2, RTN]
CALL 2
STOP
```

`LDF` がクロージャオブジェクトを生成し、**本体コードは定数プールに埋め込まれます**。
外側の命令列から見ると「LDF 1 命令」であり、本体は別の命令列として入れ子になっています。

---

### 5.1 eval_expr の処理順

1. マクロ呼び出しか判定
2. 条件を満たせば `try_eval_expr_via_vm()` を試行
3. VM コンパイル成功なら VM で実行して返す
4. 失敗なら評価器ハンドラへフォールバック

この設計により、次が両立します。

- 可能な式は VM 実行で高速化
- 難しい式（例: 一部の可変長引数・未対応形）は評価器で正しく実行

### 5.2 「コンパイル失敗」は常に異常か

常に異常ではありません。

- 想定された非対応形での失敗: 正常（評価器へフォールバック）
- 本来コンパイル可能な式での失敗増加: 要調査

自己診断のため、コンパイル成功/フォールバック件数を各 selftest で表示します。

---

## 6. VM の安全ガード

VM 実行中の暴走や不整合に備え、次のガードを入れています。

- 最大命令ステップ数制限（無限ループ検出）
- 環境深度上限制限
- スタックサイズ上限制限
- 予期しない例外を捕捉してエラーメッセージ出力
- `ContinuationEscape` は call/cc の制御用として再送出

加えて、`g_active_ctx` / `g_active_constants` は RAII で復元されるため、例外経路でも実行コンテキスト破損を防げます。

---

## 7. letrec / begin / set! まわりの重要仕様

### 7.1 letrec 初期値

`letrec` 展開時の初期バインドは `nil` ではなく `:undef`。
これにより `(letrec ((a a)) a)` は `:undef` を返します。

### 7.2 空 begin

`(begin)` は `nil` ではなく `:undef` を返します。

### 7.3 rest 引数への set!

`((lambda (a b . r) (set! r '(9 8)) r) 1 2 3 4)` が `(9 8)` になるよう修正済みです。
評価器内でローカル束縛 `set!` を優先し、VM 側の誤ったグローバル書き換え経路を回避しています。

---

## 8. load のスコープ仕様（ユーザファイル含む）

`load` はトップレベル環境として評価されるべきです。
micro Scheme 10 では次を保証しています。

- `load` 実行前に呼び出し元 lexical 環境を退避
- `load` 中はトップレベル（`g_eval_env_stack` を空）で評価
- 実行後に元の lexical 環境を復元

これにより、ローカル変数の混入や load 後の環境汚染を防止します。

---

## 9. mlib8.scm の位置づけ

mlib8.scm は「C++ 側実装と衝突しない補助ライブラリ」です。

方針:

- C++ 側にある特殊形式（let, let*, letrec, begin, and/or, cond/case/do など）を再定義しない
- 重複実装を避け、互換性と経路安定性を優先する

`--selftest-mlib-compare` で mlib の有無差分がないことを検証可能です。

---

## 10. GC とメモリ管理

- オブジェクトはヒープ管理
- mark-and-sweep 方式
- VM スタックや定数プールをルートとして利用
- ベクタ要素もマーク対象

必要時には GC ログ（`[GC] Collected ...`）が出ます。

---

## 11. トラブルシュート

### 11.1 「コンパイル失敗が見える」

まず selftest の compilation 統計を確認します。
フォールバック件数が一定で、結果が正しければ仕様上問題ない場合があります。

### 11.2 「VM が止まらない/おかしい」

VM ガードが働くとエラーメッセージで停止します。
無音でハングするより安全側に倒す設計です。

### 11.3 「mlib ありなしで結果が違う」

`--selftest-mlib-compare` を実行し、差分式を特定してください。
1式ごとの独立環境で比較するため、状態持ち越し由来の誤差を最小化しています。

---

## 12. 既知の設計上の注意点

- すべての式が VM コンパイル可能なわけではない
- フォールバック前提の実装領域がある
- 速度よりも「正しさ + 安全停止」を優先する箇所がある

---

## 13. 運用の推奨手順

1. 変更後に `--selftest-full` を実行
2. `--selftest-eval` で VM/評価器経路を確認
3. `--selftest-mlib-compare` で mlib 差分を確認
4. 必要なら `MS_TRACE_LETREC=1` で詳細トレース

例:

```powershell
$env:MS_TRACE_LETREC='1'
.\micro_scheme10.exe --selftest-eval
```

---

## 14. まとめ

micro Scheme 10 は、

- VM と評価器の二段実行
- 安全ガードつき VM
- load スコープ隔離
- letrec/begin/set! の整合修正
- mlib 競合回避

を組み合わせた、実運用寄りの安定版です。

この文書だけで、ビルド・実行・内部設計・検証観点の全体像を追えるように構成しています。
