------------------------------ MODULE MC_reach ------------------------------
\* 到達確認（検査が空振りしていないことの確認）。
\* 各式は「その状況には到達しない」という主張で、正しいモデルで TLC が違反を
\* 報告すれば、その状況が実際に探索されていることになる。check.sh が1つずつ
\* -inv で検査する。MC_reach.cfg（同時I/O上限1）で全部に到達する。
EXTENDS MC

\* 再生が VFS_VGET で対象FSの vnode を取りに行っている最中にアンマウント要求が来る
Reach_VgetDuringUnmount == ~(rpc = "vget" /\ umPhase = "notify" /\ FsOf[cur] = UmountFs)

\* 再生が対象FSへの発行済みI/Oの完了を待ってから応答しようとしている
Reach_DetachWaitsIO == ~(rpc = "detach" /\ InflightOn(UmountFs) > 0)

\* アンマウントが、読み込み中のプロセスが参照を返すのを待っている
Reach_UnmountWaitsReader ==
    ~(umPhase = "suspend" /\ \E r \in Readers : rvn[r] \in VnOf(UmountFs))

\* アンマウント処理中のFSのファイルを開こうとしたプロセスが眠っている
Reach_ReaderBlocked ==
    ~(\E r \in Readers : /\ rdpc[r] = "open" /\ ridx[r] <= Len(Script[r])
                         /\ mstate[FsOf[FileOf[CurPage(r)]]] = "unmounting")

\* アンマウント後に、そのFSのファイルを読もうとする（ENOENT で完了する）
Reach_ReadAfterUnmount ==
    ~(\E r \in Readers : /\ rdpc[r] = "open" /\ ridx[r] <= Len(Script[r])
                         /\ mstate[FsOf[FileOf[CurPage(r)]]] = "unmounted")

\* アンマウントが完了した後も、再生は残りのFSで続いている
Reach_ReplayOutlivesUnmount == ~(umPhase = "done" /\ rpc # "done" /\ held # {})

\* 本来の読み込みが、発行済みの先読みI/Oの完了を待っている
Reach_ReaderWaitsPrefetch ==
    ~(\E r \in Readers : rdpc[r] = "wait" /\ pfIO[CurPage(r)] > 0)

\* 先読みがエラーで終わり、待っていた読み込みが起きてやり直す
Reach_ReaderRetriesAfterError ==
    ~(\E r \in Readers : /\ rdpc[r] = "wait" /\ cache[CurPage(r)] = "absent"
                         /\ pfIssued[CurPage(r)] > 0)

\* 再生が未発行のページを、本来の読み込みが先に読んでいる（再生は飛ばす）
Reach_ReplaySkipsRead == ~(rpc = "issue" /\ \E p \in todo : rdIO[p] > 0)

\* 予算を使い切ったときにまだ未発行のページがある
Reach_BudgetCutsProfile == ~(rpc = "issue" /\ used = Budget /\ todo # {})

\* 同時I/O数の上限に達して、再生が完了を待って眠っている
Reach_IOLimitWait ==
    ~(rpc = "issue" /\ NoInterrupt /\ todo # {} /\ used < Budget /\ InflightAll >= MaxIO)

\* 打ち切り（メモリ逼迫・タイムアウト）の時点で先読みI/Oが実行中
Reach_AbortWithInflight == ~(rpc = "drain" /\ Abort /\ InflightAll > 0)

\* 世代番号の不一致で v2 のエントリが捨てられ、v1 だけで発行に進む
Reach_GenMismatch == ~(rpc = "issue" /\ held = {"v1"} /\ umPhase = "idle")

=============================================================================
