# micro Scheme 10B 解説

最終更新: 2026-05-01  
対象実装: micro_scheme10.cpp（現行）

---

## 1. 本ドキュメントの目的

この文書は、ここ数日の改修を反映した micro_scheme10.cpp の最新仕様を整理するためのものです。  
とくに次の更新点を反映しています。

- selftest の拡充（mlib utility カバレッジ）
- 新オプション `--selftest-mlib-utils` の追加
- mlib8.scm 側の `make-promise` / `make-iter` の互換実装調整

---

## 2. 実行モデルの要点

micro Scheme 10 は、次のハイブリッド実行系です。

- VM（コンパイル済みバイトコードを実行）
- evaluator（S 式を直接評価）

基本動作は「まず VM コンパイルを試す。難しい式は evaluator にフォールバック」です。

```
入力式
  -> compile_subset_expr() を試行
     -> 成功: vm() 実行
     -> 失敗: eval_expr() で評価
```

このため、コンパイル失敗件数が 0 でないこと自体は異常ではありません。

---

## 3. データ型と主要構造

実装上の Type は以下です。

- INT / INT64 / DOUBLE
- SYMBOL / STRING
- PAIR
- CLOSURE
- CONTINUATION
- PRIMITIVE
- PORT
- UNDEF
- VECTOR

`VECTOR` は micro_scheme10 系で明示対応済みで、GC の mark/sweep にも追跡対象として組み込まれています。

---

## 4. 初期化と mlib 読み込み

`init_env(load_mlib=true)` は次を行います。

1. `true`, `false`, `:undef`, `:eof` の登録
2. コアプリミティブ登録
3. `mlib8.scm` を条件付き読み込み
4. ブートストラップ評価後に lexical frame をクリア

注意点:

- 読み込むのは `mlib8.scm`（C++ 実装と衝突しない調整版）
- C++ 側内蔵の特殊形式（let/cond/do など）を mlib 側で再定義しない方針

---

## 5. 直近改修の重要点

### 5.1 selftest-full の mlib utility 拡充

`run_full_selftest()` に次の検証を追加済みです。

- `map-2`
- `delay` / `force`
- `make-promise`（メモ化が 1 回だけ評価されること）
- `for-each-tree`
- `make-iter`

### 5.2 新オプション `--selftest-mlib-utils`

`run_mlib_utils_selftest()` を追加し、上記 utility のみを短時間で回帰確認できるようにしました。

```powershell
.\micro_scheme10.exe --selftest-mlib-utils
```

### 5.3 mlib8.scm の互換調整

以下を「状態セル（pair）で保持する方式」に変更済みです。

- `make-promise`
- `make-iter`

背景:

- 旧実装（`set!` による局所変数再束縛）では、現行 evaluator/VM の組み合わせで期待どおりに状態保持されず、
  promise の再評価や iterator の戻り値不整合が出るケースがありました。
- 状態セル方式にすることで、期待した挙動に安定しました。

補足:

- `(list (it) (it) (it) (it))` の最終値は false 表示が `0` として出るため、期待値は `(10 20 30 0)` に合わせています。

---

## 6. 現在のセルフテスト体系

### 6.1 GC 簡易テスト

```powershell
.\micro_scheme10.exe --selftest
```

### 6.2 evaluator/VM 経路テスト

```powershell
.\micro_scheme10.exe --selftest-eval
```

- 経路ログ（`[ROUTE] vm/evaluator`）を表示
- コンパイル成功件数 / フォールバック件数を表示

### 6.3 総合テスト

```powershell
.\micro_scheme10.exe --selftest-full
```

- 言語機能全般 + mlib utility 拡張ケース

### 6.4 mlib 有無比較

```powershell
.\micro_scheme10.exe --selftest-mlib-compare
```

- `load_mlib=false/true` の評価結果を大量ケースで比較
- 期待は `mismatches=0`

### 6.5 mlib utility 専用

```powershell
.\micro_scheme10.exe --selftest-mlib-utils
```

- map-2 / delay-force / make-promise / for-each-tree / make-iter を集中確認

---

## 7. 直近の確認結果（2026-05-01）

手元実行で以下を確認済みです。

- `--selftest` : 正常
- `--selftest-eval` : all tests passed (38)
- `--selftest-full` : all tests passed
- `--selftest-mlib-compare` : total_cases=800, mismatches=0
- `--selftest-mlib-utils` : all tests passed

現時点では、今回追加した機能・互換修正を含めて回帰は確認されていません。

---

## 8. 既知の設計注意

- 「コンパイル失敗件数 > 0」自体は、フォールバック設計上あり得る
- 速度最適化より、正しさ優先で evaluator に落とすケースがある
- mlib は C++ 内蔵機能と重複再定義しない前提で運用する

---

## 9. 運用推奨

変更時は次の順で確認するのが安全です。

1. `--selftest-full`
2. `--selftest-eval`
3. `--selftest-mlib-compare`
4. mlib まわりを触った場合は `--selftest-mlib-utils`

必要に応じて `MS_TRACE_LETREC=1` を使って追跡します。

```powershell
$env:MS_TRACE_LETREC='1'
.\micro_scheme10.exe --selftest-eval
```

---

## 10. まとめ

micro Scheme 10B（現行）は、

- VM + evaluator のハイブリッド実行
- mlib8 競合回避方針
- selftest 拡充（mlib utility を直接検証）
- 追加オプション `--selftest-mlib-utils`

を備えた、保守しやすい実運用向け構成になっています。
