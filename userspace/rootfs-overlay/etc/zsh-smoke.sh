#!/bin/zsh
# Feature coverage for the interactive shell, which is zsh since M98 retired
# GNU bash. Every check below is the zsh equivalent of what BASH-SMOKE used to
# assert, so the shell swap does not quietly lose coverage: arrays, [[ ]] glob
# and regex matching, arithmetic, brace ranges, C-style for, pattern
# substitution, function-local variables and UTF-8-aware string handling.
#
# musl only recognizes "C"/"POSIX" (byte locale) vs any *.UTF-8 name (its one
# built-in multibyte locale), and the multibyte checks need the latter or the
# raw UTF-8 bytes are counted as individual bytes.
export LANG=C.UTF-8

# KSH_ARRAYS is deliberately NOT set: zsh arrays are 1-based natively, which is
# the behaviour these checks assert.
[ -n "$ZSH_VERSION" ] && echo "ZSH-SMOKE: ok version $ZSH_VERSION"
a=(alpha beta gamma)
[ "${a[2]}" = beta ] && [ "${#a[@]}" -eq 3 ] && echo "ZSH-SMOKE: ok arrays"
[[ abcde == a*e ]] && echo "ZSH-SMOKE: ok dbracket-glob"
[[ hello123 =~ ^[a-z]+[0-9]+$ ]] && echo "ZSH-SMOKE: ok regex-match"
[ $((6 * 7)) -eq 42 ] && echo "ZSH-SMOKE: ok arithmetic"
[ "$(echo {1..5})" = "1 2 3 4 5" ] && echo "ZSH-SMOKE: ok brace-range"
s=0; for ((i=1;i<=4;i++)); do s=$((s+i)); done
[ "$s" -eq 10 ] && echo "ZSH-SMOKE: ok cstyle-for"
v=foobarbar; [ "${v//bar/X}" = fooXX ] && echo "ZSH-SMOKE: ok pattern-subst"
f() { local x=inner; echo "$x"; }
[ "$(f)" = inner ] && echo "ZSH-SMOKE: ok local-vars"
v=$'αβγ'
[ "${#v}" -eq 3 ] && echo "ZSH-SMOKE: ok utf8-length"
[ "${v[2]}" = $'β' ] && echo "ZSH-SMOKE: ok utf8-substr"
echo "ZSH-SMOKE: done"
