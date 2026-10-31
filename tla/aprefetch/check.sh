#!/bin/sh
# 正しいモデルと壊したモデルをすべて TLC にかけ、期待どおりの結果か確かめる。
#
#	使い方: TLA2TOOLS=/path/to/tla2tools.jar sh check.sh
#
# 1. 正しいモデル（MC.cfg, MC_io1.cfg）と、閉路がないことの検査
#    （MC_acyclic.cfg）は「エラーなし」を期待する。
# 2. 壊したモデル（MC_bug_*.cfg）は、それぞれ狙った性質の違反を期待する。
# 3. 到達確認（MC_reach.tla の Reach_*）は、正しいモデルで違反が出る、
#    つまりその状況まで探索が届いていることを期待する。
# ログと TLC の作業ディレクトリは $OUT（既定は ${TMPDIR:-/tmp}/aprefetch-tlc）に置く。

set -u
cd "$(dirname "$0")" || exit 1
JAR=${TLA2TOOLS:-tla2tools.jar}
OUT=${OUT:-${TMPDIR:-/tmp}/aprefetch-tlc}
[ -f "$JAR" ] || { echo "tla2tools.jar が見つかりません: $JAR" >&2; exit 1; }
mkdir -p "$OUT" || exit 1

status=0

tlc() {		# tlc <ログ名> <TLC への引数...>
	name=$1
	shift
	java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers auto \
	    -metadir "$OUT/states/$name" -noGenerateSpecTE "$@" \
	    > "$OUT/$name.log" 2>&1
}

report() {	# report <ログ名> <ログに現れるべき行>
	if grep -qF "$2" "$OUT/$1.log"; then
		result=ok
	else
		result=NG
		status=1
	fi
	printf '%-3s %-31s %s\n' "$result" "$1" "$2"
}

check() {	# check <cfg> <ログに現れるべき行>
	tlc "${1%.cfg}" -config "$1" MC.tla
	report "${1%.cfg}" "$2"
}

reach() {	# reach <Reach_* の名前>
	tlc "$1" -config MC_reach.cfg -inv "$1" MC_reach.tla
	report "$1" "Invariant $1 is violated."
}

echo '-- 正しいモデル'
check MC.cfg				'Model checking completed. No error has been found.'
check MC_io1.cfg			'Model checking completed. No error has been found.'
check MC_acyclic.cfg			'Model checking completed. No error has been found.'

echo '-- 壊したモデル'
check MC_bug_S1_budget.cfg		'Invariant BudgetOK is violated.'
check MC_bug_S2_unmount_refs.cfg	'Invariant UnmountedNoRefs is violated.'
check MC_bug_S3a_double_prefetch.cfg	'Invariant NoDoublePrefetch is violated.'
check MC_bug_S3b_duplicate_io.cfg	'Invariant NoDuplicateIO is violated.'
check MC_bug_A1_io_ref.cfg		'Invariant IOHoldsRef is violated.'
check MC_bug_L1_reads.cfg		'Temporal property ReadsComplete was violated.'
check MC_bug_L2_unmount.cfg		'Temporal property UnmountCompletes was violated.'
check MC_bug_L3a_terminate.cfg		'Temporal property ReplayTerminates was violated.'
check MC_bug_L3b_release.cfg		'Temporal property ReplayReleasesAll was violated.'

echo '-- 到達確認（違反が出るのが正しい）'
for w in $(sed -n 's/^\(Reach_[A-Za-z]*\) ==.*/\1/p' MC_reach.tla); do
	reach "$w"
done

echo "ログ: $OUT"
exit $status
