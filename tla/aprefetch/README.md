# 先読み（再生）と失敗時の挙動の TLA+ モデル

[AutumnBSD_SPEC_HDD.md](../../AutumnBSD_SPEC_HDD.md) の「再生（プリフェッチ）」と「失敗時の挙動と安全性」の並行動作を TLA+ で書き、TLC で検査する。予算と停止しきい値は「資源制御」の章から取った。

## ファイル

| ファイル | 内容 |
| --- | --- |
| `AutumnPrefetch.tla` | モデル本体。公平性を動作ごとに付けるため、PlusCal は使わず TLA+ で直接書いた。定数 `Bug` で故障を1つ注入できる |
| `MC.tla` | 検査に使う具体的な定数（ページ3、vnode 2、FS 2、プロセス2） |
| `MC.cfg` | 正しいモデル。予算2ページ、同時I/O上限2。全性質を検査する |
| `MC_io1.cfg` | 正しいモデル。同時I/O上限を1にして、上限で眠る経路も通す |
| `MC_acyclic.cfg` | 状態グラフに足踏み以外の閉路がないことの検査（SF が要らない根拠） |
| `MC_bug_*.cfg` | 壊したモデル。故障を1つ注入し、狙った性質1つだけを検査する |
| `MC_reach.tla`, `MC_reach.cfg` | 到達確認。主な競合の状況まで探索が届いていることを確かめる |
| `check.sh` | 上の全部を TLC にかけ、期待どおりの結果かを判定する |

## 実行方法

[TLA+ の v1.8.0 リリース](https://github.com/tlaplus/tlaplus/releases/tag/v1.8.0)の tla2tools.jar と、Java 11 以上を使う。

```sh
TLA2TOOLS=/path/to/tla2tools.jar sh check.sh
```

1つだけ回すとき:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers auto -config MC.cfg MC.tla
```

## モデルの範囲

| ページ | ファイル | FS |
| --- | --- | --- |
| p1 | v1 | fsRoot（アンマウントされない） |
| p2 | v2 | fsData（アンマウント要求の対象） |
| p3 | v2 | fsData |

- プロファイルは全ページ。予算は2ページなので、1ページは予算で切られうる。
- 本来の読み込みを行うプロセスは2つ。r1 は p1、p2 の順に、r2 は p2、p3 の順に読む。p2 は両方が読む。
- I/O は、ページごとの実行中の先読みI/O数（`pfIO`）と、通常の読み込みI/O数（`rdIO`）で表す。完了はディスクの動作（`PfDone`、`RdDone`）で、先読みの成功とエラーは非決定に選ぶ。
- LBAソート、VOP_BMAP、ディスクの物理順序はモデル化しない。再生は残りのページから任意の1つを選ぶので、あらゆる発行順を調べる。
- 時間は扱わない。タイムアウトとメモリ逼迫は、再生中ならいつ起きてもよく、起きなくてもよい1回限りの事象とする。
- 本来の読み込みは、1回の読み込み（open、read、close）のあいだだけ vnode参照を持つ。開いたままのファイルによる EBUSY は扱わない。
- FSの状態 `unmounting` は `vfs_suspend` で止まった状態（`vfs_busy` は `fstrans_start` で眠る）に、`unmounted` は `IMNT_GONE`（`vfs_busy` は ENOENT を返す）にあたる。このツリーの `sys/kern/vfs_mount.c` の `dounmount` と `_vfs_busy` で確かめた。
- 扱わないもの: 記録（トレース）、メタデータ先読み、アプリ単位プロファイル、未マウントFSの保留（手順3）、通常の読み込みのI/Oエラー、ページの追い出し。

仕様の記述と動作の対応:

| 仕様の記述 | 動作 |
| --- | --- |
| 手順1 読み込み。プロファイルがない・壊れていれば再生しない | `RpLoad` |
| 手順2 解決。世代番号が一致しなければそのエントリだけ捨てる | `RpResolve`, `RpVget` |
| 手順6 発行。同時に投げるI/O数に上限がある | `RpIssue` |
| 手順7 終了。全件の発行、予算の使い切り、タイムアウトで終わり、vnode参照をすべて解放する | `RpIssue`, `RpAbort`, `RpDrain` |
| 発行済みI/Oがあるページは完了を待つ。未発行なら通常の読み込みにし、再生側では飛ばす | `RdFault`, `RdWake`, `RpIssue` |
| 先読みのI/Oエラー。その範囲を飛ばして続行する | `PfDone` |
| メモリ逼迫。再生を打ち切る | `MemPressure`, `RpAbort` |
| アンマウント要求。そのFSへの再生を中断し、vnode参照を解放してから応じる | `UmRequest`, `RpBeginDetach`, `RpDetach`, `UmWaitReplay`, `UmFinish` |

## モデル化で決めたこと

モデルを書くには、もとの仕様に書かれていなかった順序やロックの範囲を決める必要があった。次の8つは、どれも逆にした壊したモデルで性質が崩れることを確かめている（括弧内）。仕様書にも、「再生（プリフェッチ）」の章の「発行と完了の決まり」と、「失敗時の挙動と安全性」の章のアンマウント要求への応じ方として追記した。

1. 予算は発行した時点で差し引く（S1）。
2. ページが未取得かを確かめてから busy にして発行するまでを、同じロック区間（UVMオブジェクトのロック）で行う。本来の読み込み側も同じにする（S3b）。
3. 「その範囲を飛ばし、続行する」は、エラーになったページを発行し直さないことを意味する（S3a）。
4. 先読みがエラーで終わってページが捨てられたら、そのページを待っていた本来の読み込みを起こし、通常の読み込みでやり直させる（L1）。
5. アンマウントは、`vfs_suspend` でFSを止める前に再生へ知らせ、その応答を待つ。逆の順だと、VFS_VGET のための `vfs_busy` で眠った再生とアンマウントが互いを待つ（L2）。既存の `vfs_hooks_unmount` は `VFS_UNMOUNT` の成功後に呼ばれるので、この通知には使えない。
6. 再生が数える実行中I/O数は、エラーで完了したときも減らす（L3a）。
7. 打ち切り（メモリ逼迫・タイムアウト）でも、通常の終了と同じく発行済みI/Oの完了を待ってから全参照を解放する（L3b）。
8. アンマウント要求には、そのFSへの発行済みI/Oの完了を待ち、vrele してから応答する（S2、A1）。

`UmFinish` は、読み込み中のプロセスが参照を返すのは待つが、再生の参照は数えない。再生については応答（`detached`）を信じる。実カーネルの vflush ならここで残った参照を見つけて EBUSY を返すので、S2 の違反は実機では「再生のせいでアンマウントが EBUSY になる」形で現れる。

## 検査する性質

| ID | 性質 | 定義 |
| --- | --- | --- |
| S1 | 先読み予算を超えない | `BudgetOK`: 発行した先読みの総数 ≤ `Budget` |
| S2 | アンマウント完了時に、そのFSの vnode参照が0 | `UnmountedNoRefs` |
| S3a | 同じページへの先読みを二重に発行しない | `NoDoublePrefetch`: 各ページの発行回数 ≤ 1 |
| S3b | 本来の読み込みと同じページに、先読みを重ねて発行しない | `NoDuplicateIO`: 各ページの実行中I/O ≤ 1 |
| L1 | 本来の読み込みは必ず完了する（ENOENT など失敗で終わる場合を含む） | `ReadsComplete`: ∀r: ◇(rdpc[r] = "done") |
| L2 | アンマウント要求は必ず完了する | `UnmountCompletes`: umPhase = "notify" ⤳ umPhase = "done" |
| L3a | 再生は必ず終了する | `ReplayTerminates`: ◇(rpc = "done") |
| L3b | 再生は vnode参照をすべて解放する | `ReplayReleasesAll`: ◇□(held = {}) |
| A1 | （補助）実行中のI/Oが読むファイルには、誰かが参照を持っている | `IOHoldsRef` |
| A2 | （補助）同時に投げる先読みI/O数の上限を守る | `IOLimit` |

「二重に発行」は2つに分けた。S3a は再生自身の重複、S3b は本来の読み込みとの重複で、仕様の「未発行のページは通常の読み込みとして処理し、再生側では飛ばす」にあたる。

## 公平性の仮定

`AutumnPrefetch.tla` の `Fairness` に書いたものがすべてで、SF は使っていない。

| 動作 | 公平性 | 意味 |
| --- | --- | --- |
| `Replay`（`RpLoad`、`RpResolve`、`RpVget`、`RpIssue`、`RpClaim`、`RpDetach`、`RpDrain` の選言） | WF | 再生スレッドは、動ける状態が続けばいずれ動く（スケジューリング優先度が低くても、いずれ CPU が回ってくる） |
| `Reader(r)`（`RdOpen`、`RdFault`、`RdWake` の選言）、プロセスごと | WF | 同上 |
| `PfDone(p)`、ページごと | WF | 発行した先読みI/Oはいずれ完了する（成功かエラーかは問わない） |
| `RdDone(p)`、ページごと | WF | 通常の読み込みI/Oはいずれ完了する |
| `UmWaitReplay`、`UmFinish` | WF | アンマウント要求が来た後の処理は進む |
| `UmRequest` | なし | アンマウント要求は来ないこともある |
| `MemPressure` | なし | メモリ逼迫は起きないこともある |
| `Timeout` | なし | タイムアウトは来ないこともある |
| `Terminating` | なし | 全員が終わった後の足踏み |

SF が要らない理由: 状態グラフには足踏み以外の閉路がない（`MC_acyclic.cfg` で、公平性なしに `◇□[FALSE]_vars` が成り立つことを確かめている）。したがって無限の振る舞いは、どれも最後はある1つの状態で足踏みし続ける。その状態では「無限回実行可能」と「ずっと実行可能」が同じことになり、SF は WF と同じ意味になる。

タイムアウトに公平性を付けない理由: 付けると「いずれタイムアウトが来るから終わる」で活性が成り立ってしまい、待ちが解けないバグを隠すおそれがある。そこで、タイムアウトが来ても来なくても終わることを検査する。実際、L2 の反例ではタイムアウトが来た後も再生は VFS_VGET で眠ったままで、タイムアウトでは救えない。

どの WF も必要: 1つずつ `TRUE` に置き換えて活性を検査すると、次のとおり崩れる（✗ が違反。MC.cfg の定数で確認）。

| 外した WF | L1 | L2 | L3a | L3b |
| --- | --- | --- | --- | --- |
| `Replay` | ✓ | ✗ | ✗ | ✗ |
| `Reader(r)` | ✗ | ✗ | ✓ | ✓ |
| `PfDone(p)` | ✗ | ✗ | ✗ | ✗ |
| `RdDone(p)` | ✗ | ✗ | ✓ | ✓ |
| `UmWaitReplay`、`UmFinish` | ✗ | ✗ | ✓ | ✓ |

## 壊したモデルで違反を確かめる手順

1. `sh check.sh` を実行し、全行が `ok` になることを確かめる。
2. 1つずつ確かめるときは、壊したモデルの cfg を指定して TLC を回す。

   ```sh
   java -cp /path/to/tla2tools.jar tlc2.TLC -workers auto -config MC_bug_S1_budget.cfg MC.tla
   ```

   下の表の「期待する出力」が出て、反例が「筋書き」と同じ仕組みになっていることを見る。反例の細部は、ワーカー数や実行のたびに変わりうる。
3. その cfg の `Bug` を `"none"` に変えて同じコマンドを回し、違反が消えることを確かめる。

各 cfg は狙った性質1つだけを検査し、`CHECK_DEADLOCK FALSE` にしてある。デッドロックやほかの性質で先に止まらないようにするためで、活性の違反は「行き詰まった状態での無限の足踏み（Stuttering）」として報告される。

| 性質 | cfg | `Bug` | 壊したこと | 期待する出力 | 反例の筋書き |
| --- | --- | --- | --- | --- | --- |
| S1 | `MC_bug_S1_budget.cfg` | `ChargeOnCompletion` | 予算を発行時でなく完了時に差し引く | `Invariant BudgetOK is violated.` | 2ページ発行（使用量は0のまま）→1つ完了（使用量1）→3ページ目を発行して予算2を超える |
| S2 | `MC_bug_S2_unmount_refs.cfg` | `AckBeforeRelease` | 要求に気づいた時点で応答し、排出と vrele を後に回す | `Invariant UnmountedNoRefs is violated.` | v1、v2 を VGET→アンマウント要求→再生がすぐ応答→アンマウント完了。v2 の参照が残っている |
| S3a | `MC_bug_S3a_double_prefetch.cfg` | `RetryOnError` | エラーになったページを発行待ちに戻す | `Invariant NoDoublePrefetch is violated.` | p1 を発行→エラー→p1 をもう一度発行 |
| S3b | `MC_bug_S3b_duplicate_io.cfg` | `NonAtomicClaim` | 確かめる区間と確保する区間を分ける | `Invariant NoDuplicateIO is violated.` | 再生が p1 は未取得と見る→r1 が p1 を通常の読み込みで確保→再生も p1 を発行 |
| L1 | `MC_bug_L1_reads.cfg` | `WakeOnlyOnSuccess` | 先読みが失敗しても、待っている読み込みを起こさない | `Temporal property ReadsComplete was violated.` | r1 が先読み中の p1 を待つ→先読みがエラーでページが捨てられる→r1 は眠ったまま |
| L2 | `MC_bug_L2_unmount.cfg` | `SuspendBeforeNotify` | FSを止めてから再生に知らせる | `Temporal property UnmountCompletes was violated.` | 再生が v2 の VGET に入る→要求と同時に fsData が止まる→再生は vfs_busy で眠り、アンマウントは再生の応答を待つ |
| L3a | `MC_bug_L3a_terminate.cfg` | `IoCountLeakOnError` | エラー完了で実行中I/O数を減らさない | `Temporal property ReplayTerminates was violated.` | p1 を発行→エラー→数が1のまま残り、終了前の排出待ちが解けない |
| L3b | `MC_bug_L3b_release.cfg` | `LeakOnAbort` | 打ち切りで排出と vrele を飛ばして終える | `Temporal property ReplayReleasesAll was violated.` | タイムアウトかメモリ逼迫→再生は v1 の参照を持ったまま終了 |
| A1 | `MC_bug_A1_io_ref.cfg` | `ReleaseBeforeDrain` | 発行済みI/Oの完了を待たずに vrele する | `Invariant IOHoldsRef is violated.` | p1、p2 を発行→予算を使い切る→完了を待たずに全参照を解放 |

`LeakOnAbort` は S2 も壊す（`Bug = "LeakOnAbort"` で `UnmountedNoRefs` を検査すると違反が出る）。アンマウントは「再生が終わった」ことを信じて進むので、打ち切りで漏れた fsData の参照がアンマウントの後まで残る。

## 到達確認

正しいモデルの検査が空振りしていないことを確かめるため、`MC_reach.tla` に「この状況には到達しない」という式を13個並べ、正しいモデル（`MC_reach.cfg`、同時I/O上限1）で1つずつ `-inv` で検査している。すべて違反になる、つまり探索がその状況まで届いている。対象は、VFS_VGET 中のアンマウント要求、アンマウントによる読み込みの待ちと ENOENT、先読みエラー後のやり直し、予算による打ち切り、同時I/O上限での待ちなど（一覧は `MC_reach.tla`）。

## 検査結果

TLC 2026.10.30.154139（tla2tools.jar v1.8.0）、12ワーカー。

| cfg | 状態数 | 深さ | 時間 | 結果 |
| --- | --- | --- | --- | --- |
| `MC.cfg`（同時I/O上限2） | 154,369 | 31 | 24秒 | エラーなし |
| `MC_io1.cfg`（同時I/O上限1） | 152,629 | 31 | 24秒 | エラーなし |

`check.sh` 全体（正しいモデル2、閉路の検査1、壊したモデル9、到達確認13）は約1分半で、すべて期待どおりだった。

ページ、vnode、FS、プロセスと読む順は `MC.tla` で、予算と同時I/O上限は各 cfg で変えられる。
