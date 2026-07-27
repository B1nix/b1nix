#!/bin/bash
# musl only recognizes "C"/"POSIX" (byte locale) vs any *.UTF-8 name (its one
# built-in multibyte locale) — HANDLE_MULTIBYTE needs the latter or bash's
# mbschar path mishandles the raw UTF-8 bytes $'\uXXXX' quoting produces.
export LANG=C.UTF-8
[ -n "$BASH_VERSION" ] && echo "BASH-SMOKE: ok version $BASH_VERSION"
a=(alpha beta gamma)
[ "${a[1]}" = beta ] && [ "${#a[@]}" -eq 3 ] && echo "BASH-SMOKE: ok arrays"
[[ abcde == a*e ]] && echo "BASH-SMOKE: ok dbracket-glob"
[[ hello123 =~ ^[a-z]+[0-9]+$ ]] && echo "BASH-SMOKE: ok regex-match"
[ $((6 * 7)) -eq 42 ] && echo "BASH-SMOKE: ok arithmetic"
[ "$(echo {1..5})" = "1 2 3 4 5" ] && echo "BASH-SMOKE: ok brace-range"
s=0; for ((i=1;i<=4;i++)); do s=$((s+i)); done
[ "$s" -eq 10 ] && echo "BASH-SMOKE: ok cstyle-for"
v=foobarbar; [ "${v//bar/X}" = fooXX ] && echo "BASH-SMOKE: ok pattern-subst"
f() { local x=inner; echo "$x"; }
[ "$(f)" = inner ] && echo "BASH-SMOKE: ok local-vars"
v=$'\u03b1\u03b2\u03b3'
[ "${#v}" -eq 3 ] && echo "BASH-SMOKE: ok utf8-length"
[ "${v:1:1}" = $'\u03b2' ] && echo "BASH-SMOKE: ok utf8-substr"
echo "BASH-SMOKE: done"
