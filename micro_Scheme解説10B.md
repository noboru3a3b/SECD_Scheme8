# micro Scheme 10 実装解説（単体完結版）

最終更新: 2026-05-02  
対象: micro_scheme10.cpp / mlib8.scm（現行）

---

## 1. この文書の位置付け

この文書は、micro Scheme 10 の現行実装を、この文書だけで理解できるように整理したものです。  
旧版文書の参照を前提にせず、次を一通り説明します。

- 実行モデル（VM と evaluator の役割分担）
- データ構造と GC の要点
- call/cc と継続再開の扱い
- mlib8.scm との境界
- 現在のセルフテスト体系と運用
- 最近の修正ポイントと既知挙動

---

## 2. 全体アーキテクチャ

micro Scheme 10 はハイブリッド実行系です。

- VM 経路: 式をバイトコードへコンパイルして高速実行
- evaluator 経路: 直接評価（複雑式や安全側フォールバック）

基本フローは次の通りです。

1. 入力式を読み取り
2. compile_subset_expr で VM コンパイルを試行
3. 成功なら vm 実行
4. 失敗なら evaluator にフォールバック

したがって、コンパイル失敗件数が 0 でないこと自体は正常です。  
設計上、正しさ優先で evaluator 側に落とすケースがあります。

---

## 3. サポート型と実行コンテキスト

主要 Type:

- INT / INT64 / DOUBLE
- SYMBOL / STRING
- PAIR
- CLOSURE
- CONTINUATION
- PRIMITIVE
- VECTOR
- PORT
- UNDEF

実行中に重要な状態:

- VMContext: S/E/C/D レジスタ相当（スタック、環境、コード、ダンプ）
- globals / macros / symbols: グローバルテーブル
- g_eval_env_stack: evaluator 用 lexical 環境
- g_closure_lexenv: eval 系クロージャが捕捉した lexical 環境

---

## 4. 初期化と mlib8.scm

init_env(load_mlib=true) の概略:

1. true/false/:undef/:eof を登録
2. コアプリミティブを登録
3. mlib8.scm を読み込み
4. ブートストラップ評価後に一時 lexical frame を整理

方針:

- C++ 実装済みの特殊形式（let, let*, letrec, begin, and, or, cond, case, do など）を mlib で再定義しない
- mlib は補助関数群に集中し、VM/evaluator の評価経路を乱さない

---

## 5. GC 設計と最近の安定化

### 5.1 GC 方式

- Mark & Sweep
- ルートは VMContext（S/E/D）、constants、globals/macros/symbols、一時 roots など

### 5.2 g_closure_lexenv の剪定

GC 時に、g_closure_lexenv の key（クロージャ）が未到達なら削除します。  
これにより古い lexical 捕捉情報の蓄積を抑えます。

### 5.3 中間オブジェクト誤回収対策

重い cons 連鎖や call frame 構築中に GC が走る場合を想定し、

- vector_to_pair
- build_vm_call_frame

で中間生成物を一時ルートに保持するよう修正済みです。  
これにより、高負荷時の値崩れや不安定挙動を防止しています。

---

## 6. call/cc と継続実装

### 6.1 基本方針

- VM の CALLCC 命令で CONTINUATION を捕捉
- 継続オブジェクトに S/E/C/D と constants を保存
- 継続再開時は保存コンテキストへ復帰

### 6.2 C++ プリミティブ境界を跨ぐ非局所脱出

継続呼び出しが VM セッション内で起きた場合、通常 return ではなく ContinuationEscape で unwind し、
適切な VM 呼び出しフレームまで戻して復元する設計です。  
これにより apply / for-each 経由の call/cc でも一貫性を保ちます。

---

## 7. mlib8.scm 側ユーティリティ仕様（現行）

### 7.1 make-promise / delay / force

- promise は一度だけ本体を評価し、以後はキャッシュ値を返す

### 7.2 make-iter / make-iter-lazy

- 現在は make-iter-lazy-available が true
- make-iter は make-iter-lazy を既定で使用
- 期待挙動例: (list (it) (it) (it) (it)) が (10 20 30 0)

0 は false 表示です（実装上 false は整数 0 相当として表示される場面があります）。

---

## 8. セルフテスト一覧

### 8.1 GC 簡易

- micro_scheme10.exe --selftest

### 8.2 evaluator/VM 検証

- micro_scheme10.exe --selftest-eval

出力内容:

- 全ケース pass/fail
- vm route 使用件数
- compilation success/fallback 件数

補足: ROUTE 詳細ログは通常非表示。必要時のみ以下で有効化。

- MS_TRACE_SELFTEST_ROUTE=1

### 8.3 総合

- micro_scheme10.exe --selftest-full

言語機能 + mlib utility + call/cc をまとめて確認します。

### 8.4 mlib 比較

- micro_scheme10.exe --selftest-mlib-compare

load_mlib=false/true で結果一致を確認します。  
期待値は mismatches=0。

### 8.5 mlib utility 集中

- micro_scheme10.exe --selftest-mlib-utils

対象:

- map-2
- delay/force
- make-promise
- for-each-tree
- make-iter / make-iter-lazy

### 8.6 call/cc 集中

- micro_scheme10.exe --selftest-callcc

対象:

- 基本 escape
- 継続保存と再呼び出し
- 早期脱出パターン
- iterator と call/cc の相互作用

---

## 9. 既知挙動と読み方

### 9.1 GC ログの lexenv と REPL 末尾 LexEnv が違う

これは表示タイミング差で起こり得ます。

- GC ログ: GC 実行時点
- REPL の Heap size / LexEnv: 式評価完了時点

### 9.2 fallback 件数がある

異常ではありません。  
VM コンパイル非対応構文や安全側判定で evaluator に回る設計です。

---

## 10. 日常運用の推奨手順

実装変更後の推奨確認順:

1. micro_scheme10.exe --selftest-full
2. micro_scheme10.exe --selftest-eval
3. micro_scheme10.exe --selftest-callcc
4. mlib 変更時は --selftest-mlib-utils と --selftest-mlib-compare も実行

必要なトレース例:

- MS_TRACE_LETREC=1
- MS_TRACE_CALLCC=1
- MS_TRACE_VM_ALL=1
- MS_TRACE_SELFTEST_ROUTE=1

---

## 11. まとめ

現行 10 は、

- VM と evaluator のハイブリッド実行
- call/cc を含む継続復帰の安定化
- GC での中間オブジェクト保護強化
- mlib8 utility を含むセルフテスト体系の整備

まで含めて、単体で保守・検証しやすい状態です。
