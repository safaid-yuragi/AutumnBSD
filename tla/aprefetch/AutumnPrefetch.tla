--------------------------- MODULE AutumnPrefetch ---------------------------
\* AutumnBSD_SPEC_HDD.md の「再生（プリフェッチ）」と「失敗時の挙動と安全性」
\* の並行動作のモデル。PlusCal は使わず、公平性は Fairness に動作ごとに書く。
\*
\* 主体と動作:
\*   再生スレッド（1つ）        RpLoad RpResolve RpVget RpIssue RpClaim
\*                              RpDetach RpDrain
\*   本来の読み込み（Readers）  RdOpen RdFault RdWake
\*   ディスク（I/Oの完了）      PfDone RdDone
\*   アンマウント要求           UmRequest UmWaitReplay UmFinish
\*   メモリ逼迫による打ち切り   MemPressure
\*   タイムアウト               Timeout
\*
\* 抽象化:
\*   - ページ・vnode・FS は小さな有限集合。I/O はページごとの「実行中の
\*     先読みI/O数」と「実行中の通常読み込みI/O数」で表す。
\*   - LBAソート、VOP_BMAP、ディスクの物理順序はモデル化しない。再生は残りの
\*     ページから任意の1つを選ぶので、あらゆる発行順を網羅する。
\*   - 時間は扱わない。タイムアウトとメモリ逼迫は、再生中のいつ起きてもよく、
\*     起きなくてもよい1回限りの事象とする。
\*   - 本来の読み込みは、1回の読み込みのあいだだけ vnode 参照を持つ
\*     （open、read、close を1回の読み込みにまとめる）。
\*
\* 定数 Bug で故障を1つ注入できる。"none" が正しいモデル。

EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
    Pages,      \* ページの集合（プロファイルは全ページを含む）
    Vnodes,     \* vnode（ファイル）の集合
    FS,         \* ファイルシステムの集合
    UmountFs,   \* アンマウント要求の対象になるFS
    FileOf,     \* [Pages -> Vnodes] ページを含むファイル
    FsOf,       \* [Vnodes -> FS] ファイルがあるFS
    Readers,    \* 本来の読み込みを行うプロセスの集合
    Script,     \* [Readers -> Seq(Pages)] 各プロセスが順に読むページ
    Budget,     \* 先読み予算（ページ数）
    MaxIO,      \* 同時に投げる先読みI/O数の上限
    Bug         \* 注入する故障（BugNames のどれか）

BugNames ==
    { "none",
      "ChargeOnCompletion",   \* S1  予算を発行時でなく完了時に差し引く
      "AckBeforeRelease",     \* S2  vnode参照を解放する前にアンマウントへ応答する
      "RetryOnError",         \* S3a 先読みI/Oのエラー後にそのページを発行し直す
      "NonAtomicClaim",       \* S3b ページの確認と確保を別のロック区間で行う
      "WakeOnlyOnSuccess",    \* L1  先読みが失敗しても待っている読み込みを起こさない
      "SuspendBeforeNotify",  \* L2  FSを止めてから再生に知らせる（順序の逆転）
      "IoCountLeakOnError",   \* L3a エラー完了で実行中I/O数を減らさない
      "LeakOnAbort",          \* L3b 打ち切りの経路で vnode参照を解放しない
      "ReleaseBeforeDrain" }  \* A1  発行済みI/Oの完了を待たずに vrele する

ASSUME
    /\ FileOf \in [Pages -> Vnodes]
    /\ FsOf \in [Vnodes -> FS]
    /\ UmountFs \in FS
    /\ \A r \in Readers : \A i \in 1..Len(Script[r]) : Script[r][i] \in Pages
    /\ Budget \in Nat
    /\ MaxIO \in Nat \ {0}
    /\ Bug \in BugNames

VARIABLES
    \* カーネルの共有状態
    mstate,     \* [FS -> {"mounted", "unmounting", "unmounted"}]
    cache,      \* [Pages -> {"absent", "busy", "valid"}] busy は PG_BUSY（I/O中）
    pfIO,       \* [Pages -> Nat] 実行中の先読みI/O（真の値）
    rdIO,       \* [Pages -> Nat] 実行中の通常読み込みI/O（真の値）
    memLow,     \* 空きページが停止しきい値を下回った
    timedOut,   \* 再生のタイムアウトが来た
    \* 再生スレッド
    rpc,        \* プログラムカウンタ
    toResolve,  \* まだ VFS_VGET していない vnode
    cur,        \* VFS_VGET 中の vnode（NonAtomicClaim では確保しかけのページ）
    held,       \* 再生が参照を持っている vnode
    todo,       \* まだ発行も飛ばしもしていないページ
    used,       \* 再生が数えている予算の使用量
    nIO,        \* [Vnodes -> Nat] 再生が数えている実行中の先読みI/O数
    detached,   \* 再生が「手を引いた」と応答したFS
    \* 本来の読み込みを行うプロセス
    rdpc,       \* [Readers -> {"open", "fault", "wait", "done"}]
    ridx,       \* [Readers -> Nat] 次に読む Script の位置
    rvn,        \* [Readers -> Vnodes \cup {Nil}] 読み込み中に持つ vnode参照
    \* アンマウント
    umPhase,    \* "idle" -> "notify" -> "suspend" -> "done"
    \* 検査用の履歴変数
    pfIssued    \* [Pages -> Nat] そのページに先読みを発行した回数

envVars == <<memLow, timedOut>>
rpVars  == <<rpc, toResolve, cur, held, todo, used, nIO, detached>>
rdVars  == <<rdpc, ridx, rvn>>
vars    == <<mstate, cache, pfIO, rdIO, envVars, rpVars, rdVars, umPhase, pfIssued>>

Nil == "-"

PagesOf(v) == {p \in Pages : FileOf[p] = v}
VnOf(f)    == {v \in Vnodes : FsOf[v] = f}
PagesOn(f) == {p \in Pages : FsOf[FileOf[p]] = f}
ProfVnodes == {FileOf[p] : p \in Pages}

RECURSIVE Sum(_, _)
Sum(f, S) == IF S = {} THEN 0
             ELSE LET x == CHOOSE y \in S : TRUE
                  IN  f[x] + Sum(f, S \ {x})

InflightAll   == Sum(nIO, Vnodes)
InflightOn(f) == Sum(nIO, VnOf(f))

\* vnode の参照数（再生の分と、読み込み中のプロセスの分）
RefCnt(v) == (IF v \in held THEN 1 ELSE 0)
             + Cardinality({r \in Readers : rvn[r] = v})

Abort       == memLow \/ timedOut
UmPending   == umPhase # "idle" /\ UmountFs \notin detached
NoInterrupt == ~Abort /\ ~UmPending

CurPage(r) == Script[r][ridx[r]]

Init ==
    /\ mstate = [f \in FS |-> "mounted"]
    /\ cache = [p \in Pages |-> "absent"]
    /\ pfIO = [p \in Pages |-> 0]
    /\ rdIO = [p \in Pages |-> 0]
    /\ memLow = FALSE
    /\ timedOut = FALSE
    /\ rpc = "load"
    /\ toResolve = {}
    /\ cur = Nil
    /\ held = {}
    /\ todo = {}
    /\ used = 0
    /\ nIO = [v \in Vnodes |-> 0]
    /\ detached = {}
    /\ rdpc = [r \in Readers |-> "open"]
    /\ ridx = [r \in Readers |-> 1]
    /\ rvn = [r \in Readers |-> Nil]
    /\ umPhase = "idle"
    /\ pfIssued = [p \in Pages |-> 0]

\* ---------------------------------------------------------------------------
\* 再生スレッド
\* ---------------------------------------------------------------------------

\* 手順1 読み込み: ブートプロファイルを読んで検証する。
RpLoad ==
    /\ rpc = "load"
    /\ \/ /\ rpc' = "resolve"                    \* 検証に通った
          /\ toResolve' = ProfVnodes
          /\ todo' = Pages
       \/ /\ rpc' = "done"                       \* ない・壊れている: 再生しない
          /\ UNCHANGED <<toResolve, todo>>
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, cur, held, used, nIO,
                   detached, rdVars, umPhase, pfIssued>>

\* 打ち切り（メモリ逼迫・タイムアウト）: 以後は発行せず、排出してから解放する。
RpAbort ==
    /\ Abort
    /\ rpc' = IF Bug = "LeakOnAbort" THEN "done" ELSE "drain"
    /\ toResolve' = {}
    /\ todo' = {}
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, cur, held, used, nIO,
                   detached, rdVars, umPhase, pfIssued>>

\* アンマウント要求に気づいた: そのFSの未処理分を捨て、そのFSのI/Oの排出に入る。
RpBeginDetach ==
    /\ ~Abort
    /\ UmPending
    /\ toResolve' = toResolve \ VnOf(UmountFs)
    /\ todo' = todo \ PagesOn(UmountFs)
    /\ detached' = IF Bug = "AckBeforeRelease"
                     THEN detached \cup {UmountFs}
                     ELSE detached
    /\ rpc' = "detach"
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, cur, held, used, nIO,
                   rdVars, umPhase, pfIssued>>

\* 解決と発行のループの先頭で、打ち切りとアンマウント要求を確かめる。
RpCheck == RpAbort \/ RpBeginDetach

\* 手順2 解決: 次に VFS_VGET する vnode を選ぶ。
RpResolve ==
    /\ rpc = "resolve"
    /\ \/ RpCheck
       \/ /\ NoInterrupt
          /\ toResolve = {}
          /\ rpc' = "issue"
          /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, toResolve, cur, held,
                         todo, used, nIO, detached, rdVars, umPhase, pfIssued>>
       \/ /\ NoInterrupt
          /\ \E v \in toResolve :
               /\ cur' = v
               /\ toResolve' = toResolve \ {v}
          /\ rpc' = "vget"
          /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, held, todo, used, nIO,
                         detached, rdVars, umPhase, pfIssued>>

\* VFS_VGET。アンマウント処理中のFSでは vfs_busy で眠る（その間この動作は実行できない）。
RpVget ==
    /\ rpc = "vget"
    /\ LET f == FsOf[cur] IN
         \/ /\ mstate[f] = "mounted"                  \* 成功: 参照を1つ得る
            /\ held' = held \cup {cur}
            /\ UNCHANGED todo
         \/ /\ mstate[f] \in {"mounted", "unmounted"} \* 世代番号の不一致、またはFSがない
            /\ todo' = todo \ PagesOf(cur)            \* そのエントリだけ捨てる
            /\ UNCHANGED held
    /\ cur' = Nil
    /\ rpc' = "resolve"
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, toResolve, used, nIO,
                   detached, rdVars, umPhase, pfIssued>>

\* ページを busy にして非同期の先読みI/Oを出す。
StartPrefetch(p) ==
    /\ cache' = [cache EXCEPT ![p] = "busy"]
    /\ pfIO' = [pfIO EXCEPT ![p] = @ + 1]
    /\ nIO' = [nIO EXCEPT ![FileOf[p]] = @ + 1]
    /\ used' = IF Bug = "ChargeOnCompletion" THEN used ELSE used + 1
    /\ pfIssued' = [pfIssued EXCEPT ![p] = @ + 1]

\* 手順6 発行と手順7 終了の判定。I/O数が上限なら完了を待って眠る。
RpIssue ==
    /\ rpc = "issue"
    /\ \/ RpCheck
       \/ /\ NoInterrupt
          /\ todo = {} \/ used >= Budget          \* 全件発行した、または予算を使い切った
          /\ rpc' = "drain"
          /\ todo' = {}
          /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, toResolve, cur, held,
                         used, nIO, detached, rdVars, umPhase, pfIssued>>
       \/ /\ NoInterrupt
          /\ todo # {} /\ used < Budget
          /\ InflightAll < MaxIO
          /\ \E p \in todo :
               /\ todo' = todo \ {p}
               /\ IF Bug = "NonAtomicClaim"
                    THEN \* 状態を見ただけでロックを離し、確保は RpClaim で行う
                         /\ IF cache[p] = "absent"
                              THEN /\ cur' = p
                                   /\ rpc' = "claim"
                              ELSE UNCHANGED <<cur, rpc>>
                         /\ UNCHANGED <<cache, pfIO, nIO, used, pfIssued>>
                    ELSE \* オブジェクトのロック下で、見てから確保するまでを1動作で行う
                         /\ IF cache[p] = "absent"
                              THEN StartPrefetch(p)
                              ELSE UNCHANGED <<cache, pfIO, nIO, used, pfIssued>>
                         /\ UNCHANGED <<cur, rpc>>
                         \* absent 以外（読み込み中・キャッシュ済み）は飛ばす
          /\ UNCHANGED <<mstate, rdIO, envVars, toResolve, held, detached,
                         rdVars, umPhase>>

\* NonAtomicClaim のときだけ通る: 状態を確かめ直さずに確保して発行する。
RpClaim ==
    /\ rpc = "claim"
    /\ StartPrefetch(cur)
    /\ cur' = Nil
    /\ rpc' = "issue"
    /\ UNCHANGED <<mstate, rdIO, envVars, toResolve, held, todo, detached,
                   rdVars, umPhase>>

\* 対象FSへの先読みI/Oが済んだら、そのFSの vnode参照を解放してから応答する。
RpDetach ==
    /\ rpc = "detach"
    /\ Bug = "ReleaseBeforeDrain" \/ InflightOn(UmountFs) = 0
    /\ held' = held \ VnOf(UmountFs)
    /\ detached' = detached \cup {UmountFs}
    /\ rpc' = IF toResolve # {} THEN "resolve" ELSE "issue"
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, toResolve, cur, todo, used,
                   nIO, rdVars, umPhase, pfIssued>>

\* 手順7 終了: 発行済みI/Oの完了を待ち、取得した vnode参照をすべて解放する。
RpDrain ==
    /\ rpc = "drain"
    /\ Bug = "ReleaseBeforeDrain" \/ InflightAll = 0
    /\ held' = {}
    /\ rpc' = "done"
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, toResolve, cur, todo, used,
                   nIO, detached, rdVars, umPhase, pfIssued>>

Replay == RpLoad \/ RpResolve \/ RpVget \/ RpIssue \/ RpClaim \/ RpDetach \/ RpDrain

\* ---------------------------------------------------------------------------
\* ディスク: 発行済みI/Oの完了（iodone）
\* ---------------------------------------------------------------------------

\* 先読みI/Oの完了。成功ならページは有効になり、エラーならページを捨てる。
\* エラーはその範囲を飛ばして続行するだけで、再生は止めない。
PfDone(p) ==
    /\ pfIO[p] > 0
    /\ pfIO' = [pfIO EXCEPT ![p] = @ - 1]
    /\ used' = IF Bug = "ChargeOnCompletion" THEN used + 1 ELSE used
    /\ \/ /\ cache' = [cache EXCEPT ![p] = "valid"]
          /\ nIO' = [nIO EXCEPT ![FileOf[p]] = @ - 1]
          /\ UNCHANGED todo
       \/ /\ cache' = [cache EXCEPT ![p] = "absent"]
          /\ nIO' = IF Bug = "IoCountLeakOnError"
                      THEN nIO
                      ELSE [nIO EXCEPT ![FileOf[p]] = @ - 1]
          /\ todo' = IF Bug = "RetryOnError" /\ rpc = "issue"
                       THEN todo \cup {p}
                       ELSE todo
    /\ UNCHANGED <<mstate, rdIO, envVars, rpc, toResolve, cur, held, detached,
                   rdVars, umPhase, pfIssued>>

\* 通常の読み込みI/Oの完了（こちらのエラーは扱わない）。
RdDone(p) ==
    /\ rdIO[p] > 0
    /\ rdIO' = [rdIO EXCEPT ![p] = @ - 1]
    /\ cache' = [cache EXCEPT ![p] = "valid"]
    /\ UNCHANGED <<mstate, pfIO, envVars, rpVars, rdVars, umPhase, pfIssued>>

\* ---------------------------------------------------------------------------
\* 本来の読み込みを行うプロセス
\* ---------------------------------------------------------------------------

\* 1回の読み込みを終え、vnode参照を返して次へ進む。
RdFinish(r) ==
    /\ rvn' = [rvn EXCEPT ![r] = Nil]
    /\ ridx' = [ridx EXCEPT ![r] = @ + 1]
    /\ rdpc' = [rdpc EXCEPT ![r] = "open"]

\* 次のページのファイルを開く。アンマウント処理中のFSでは眠って待つ。
RdOpen(r) ==
    /\ rdpc[r] = "open"
    /\ IF ridx[r] > Len(Script[r])
         THEN /\ rdpc' = [rdpc EXCEPT ![r] = "done"]
              /\ UNCHANGED <<ridx, rvn>>
         ELSE LET v == FileOf[CurPage(r)] IN
                \/ /\ mstate[FsOf[v]] = "mounted"       \* vnode参照を得る
                   /\ rvn' = [rvn EXCEPT ![r] = v]
                   /\ rdpc' = [rdpc EXCEPT ![r] = "fault"]
                   /\ UNCHANGED ridx
                \/ /\ mstate[FsOf[v]] = "unmounted"     \* FSがない: ENOENT で完了
                   /\ ridx' = [ridx EXCEPT ![r] = @ + 1]
                   /\ UNCHANGED <<rdpc, rvn>>
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, rpVars, umPhase, pfIssued>>

\* ページを要求する（genfs_getpages）。確認と確保はオブジェクトのロック下で行う。
RdFault(r) ==
    /\ rdpc[r] = "fault"
    /\ LET p == CurPage(r) IN
         \/ /\ cache[p] = "valid"                     \* キャッシュにある
            /\ RdFinish(r)
            /\ UNCHANGED <<cache, rdIO>>
         \/ /\ cache[p] = "busy"                      \* 発行済みI/Oの完了を待つ
            /\ rdpc' = [rdpc EXCEPT ![r] = "wait"]
            /\ UNCHANGED <<cache, rdIO, ridx, rvn>>
         \/ /\ cache[p] = "absent"                    \* 未発行: 通常の読み込みとして処理
            /\ cache' = [cache EXCEPT ![p] = "busy"]
            /\ rdIO' = [rdIO EXCEPT ![p] = @ + 1]
            /\ rdpc' = [rdpc EXCEPT ![r] = "wait"]
            /\ UNCHANGED <<ridx, rvn>>
    /\ UNCHANGED <<mstate, pfIO, envVars, rpVars, umPhase, pfIssued>>

\* busy が外れたら起きる。先読みがエラーでページが捨てられていたらやり直す。
RdWake(r) ==
    /\ rdpc[r] = "wait"
    /\ LET p == CurPage(r) IN
         \/ /\ cache[p] = "valid"
            /\ RdFinish(r)
         \/ /\ cache[p] = "absent"
            /\ Bug # "WakeOnlyOnSuccess"
            /\ rdpc' = [rdpc EXCEPT ![r] = "fault"]
            /\ UNCHANGED <<ridx, rvn>>
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, envVars, rpVars, umPhase, pfIssued>>

Reader(r) == RdOpen(r) \/ RdFault(r) \/ RdWake(r)

\* ---------------------------------------------------------------------------
\* メモリ逼迫・タイムアウト・アンマウント
\* ---------------------------------------------------------------------------

MemPressure ==
    /\ rpc # "done"
    /\ ~memLow
    /\ memLow' = TRUE
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, timedOut, rpVars, rdVars, umPhase,
                   pfIssued>>

Timeout ==
    /\ rpc # "done"
    /\ ~timedOut
    /\ timedOut' = TRUE
    /\ UNCHANGED <<mstate, cache, pfIO, rdIO, memLow, rpVars, rdVars, umPhase,
                   pfIssued>>

\* アンマウント要求が来る。dounmount はまずフックで再生に知らせる。
UmRequest ==
    /\ umPhase = "idle"
    /\ umPhase' = "notify"
    /\ mstate' = IF Bug = "SuspendBeforeNotify"
                   THEN [mstate EXCEPT ![UmountFs] = "unmounting"]
                   ELSE mstate
    /\ UNCHANGED <<cache, pfIO, rdIO, envVars, rpVars, rdVars, pfIssued>>

\* 再生がそのFSから手を引いた（または終わった）ら、FSを止めて（vfs_suspend）
\* 新しい参照を断つ。以後そのFSの vfs_busy は眠る。
UmWaitReplay ==
    /\ umPhase = "notify"
    /\ UmountFs \in detached \/ rpc = "done"
    /\ umPhase' = "suspend"
    /\ mstate' = [mstate EXCEPT ![UmountFs] = "unmounting"]
    /\ UNCHANGED <<cache, pfIO, rdIO, envVars, rpVars, rdVars, pfIssued>>

\* 進行中の読み込みが参照を返したらアンマウントを完了する。
\* 再生の参照は数えない。再生については応答（detached）を信じる。
UmFinish ==
    /\ umPhase = "suspend"
    /\ \A r \in Readers : rvn[r] \notin VnOf(UmountFs)
    /\ umPhase' = "done"
    /\ mstate' = [mstate EXCEPT ![UmountFs] = "unmounted"]
    /\ UNCHANGED <<cache, pfIO, rdIO, envVars, rpVars, rdVars, pfIssued>>

\* ---------------------------------------------------------------------------
\* 全体
\* ---------------------------------------------------------------------------

AllDone ==
    /\ rpc = "done"
    /\ \A r \in Readers : rdpc[r] = "done"
    /\ umPhase \in {"idle", "done"}

\* 全員が終わった後の足踏み（デッドロックと区別する）
Terminating == AllDone /\ UNCHANGED vars

Next ==
    \/ Replay
    \/ \E r \in Readers : Reader(r)
    \/ \E p \in Pages : PfDone(p) \/ RdDone(p)
    \/ MemPressure \/ Timeout \/ UmRequest
    \/ UmWaitReplay \/ UmFinish
    \/ Terminating

\* 公平性の仮定。SF はどこにも使わない。
\* ここにない UmRequest、MemPressure、Timeout、Terminating には公平性を付けない
\* （アンマウント要求、メモリ逼迫、タイムアウトは起きなくてもよい）。
Fairness ==
    /\ WF_vars(Replay)                                     \* 再生スレッドの全動作
    /\ \A r \in Readers : WF_vars(Reader(r))               \* 各プロセスの全動作
    /\ \A p \in Pages : WF_vars(PfDone(p)) /\ WF_vars(RdDone(p))  \* I/Oの完了
    /\ WF_vars(UmWaitReplay)                               \* 要求後のアンマウント処理
    /\ WF_vars(UmFinish)

Spec == Init /\ [][Next]_vars /\ Fairness

\* ---------------------------------------------------------------------------
\* 性質
\* ---------------------------------------------------------------------------

TypeOK ==
    /\ mstate \in [FS -> {"mounted", "unmounting", "unmounted"}]
    /\ cache \in [Pages -> {"absent", "busy", "valid"}]
    /\ pfIO \in [Pages -> Nat]
    /\ rdIO \in [Pages -> Nat]
    /\ memLow \in BOOLEAN
    /\ timedOut \in BOOLEAN
    /\ rpc \in {"load", "resolve", "vget", "issue", "claim", "detach", "drain", "done"}
    /\ toResolve \subseteq Vnodes
    /\ cur \in Vnodes \cup Pages \cup {Nil}
    /\ held \subseteq Vnodes
    /\ todo \subseteq Pages
    /\ used \in Nat
    /\ nIO \in [Vnodes -> Nat]
    /\ detached \subseteq FS
    /\ rdpc \in [Readers -> {"open", "fault", "wait", "done"}]
    /\ ridx \in [Readers -> Nat]
    /\ rvn \in [Readers -> Vnodes \cup {Nil}]
    /\ umPhase \in {"idle", "notify", "suspend", "done"}
    /\ pfIssued \in [Pages -> Nat]

\* S1 先読み予算を超えない（発行した先読みの総数で数える）
BudgetOK == Sum(pfIssued, Pages) <= Budget

\* S2 アンマウント完了時に、そのFSの vnode参照が0
UnmountedNoRefs ==
    \A f \in FS : mstate[f] = "unmounted" => \A v \in VnOf(f) : RefCnt(v) = 0

\* S3a 同じページへの先読みを二重に発行しない
NoDoublePrefetch == \A p \in Pages : pfIssued[p] <= 1

\* S3b 本来の読み込みと先読みが、同じページに重ねてI/Oを出さない
NoDuplicateIO == \A p \in Pages : pfIO[p] + rdIO[p] <= 1

\* A1（補助）実行中のI/Oが読むファイルには、誰かが vnode参照を持っている
IOHoldsRef == \A p \in Pages : pfIO[p] + rdIO[p] > 0 => RefCnt(FileOf[p]) > 0

\* A2（補助）同時に投げる先読みI/O数の上限を守る
IOLimit == Sum(pfIO, Pages) <= MaxIO

\* L1 本来の読み込みは必ず完了する（失敗で終わる場合を含む）
ReadsComplete == \A r \in Readers : <>(rdpc[r] = "done")

\* L2 アンマウント要求は必ず完了する
UnmountCompletes == (umPhase = "notify") ~> (umPhase = "done")

\* L3a 再生は必ず終了する
ReplayTerminates == <>(rpc = "done")

\* L3b 再生は vnode参照をすべて解放し、以後持たない
ReplayReleasesAll == <>[](held = {})

\* 補助: 状態グラフに足踏み以外の閉路がない（公平性なしでも、どの振る舞いも
\* いずれずっと足踏みになる）。UnfairSpec の下で検査する（MC_acyclic.cfg）。
\* これが成り立つので、無限の振る舞いでは「無限回実行可能」と「ずっと実行可能」
\* が一致し、どの動作についても SF は WF と同じ意味になる。
UnfairSpec == Init /\ [][Next]_vars
EventuallyQuiet == <>[][FALSE]_vars

=============================================================================
