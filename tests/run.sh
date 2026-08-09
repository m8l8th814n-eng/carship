#!/bin/sh
# run.sh [path-to-carship]
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bin=${1:-$root/build/carship}
work=${TMPDIR:-/tmp}/carship-tests.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

CC=${CC:-clang}
CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -D_GNU_SOURCE -I$root/src"
UNITS="$root/src/toml.c $root/src/util.c $root/src/style.c $root/src/format.c"

status=0

for t in "$root"/tests/test_*.c; do
	name=$(basename "$t" .c)
	printf '\n=== %s ===\n' "$name"
	# shellcheck disable=SC2086
	$CC $CFLAGS -o "$work/$name" "$t" $UNITS
	"$work/$name" || status=1
done

printf '\n=== cli ===\n'

expect_ok() {
	if "$@" >/dev/null 2>&1; then
		printf 'ok   %s\n' "$*"
	else
		printf 'FAIL %s\n' "$*"
		status=1
	fi
}

expect_out() {
	want=$1
	shift
	got=$("$@" 2>/dev/null || true)
	case $got in
	*"$want"*) printf 'ok   %s ~ %s\n' "$*" "$want" ;;
	*)
		printf 'FAIL %s\n  want substring [%s]\n  got [%s]\n' "$*" "$want" "$got"
		status=1
		;;
	esac
}

if [ -x "$bin" ]; then
	# Run shell checks against empty rc files so the developer's own
	# configuration cannot make them pass or fail by accident.
	mkdir -p "$work/zdot"
	: >"$work/zdot/.zshrc"
	zsh_clean() { env -u CARSHIP_NO_COMPLETION ZDOTDIR="$work/zdot" zsh "$@"; }
	bash_clean() { env -u CARSHIP_NO_COMPLETION bash --norc --noprofile "$@"; }

	expect_ok "$bin" --version
	expect_out "carship" "$bin" --version
	expect_out "print-full-init" "$bin" init bash
	expect_out "CARSHIP_START_TIME" "$bin" init bash --print-full-init
	expect_out "add-zsh-hook" "$bin" init zsh --print-full-init
	expect_out "carship" "$bin" init zsh
	expect_out "palette" "$bin" preset catppuccin-powerline
	expect_out "complete" "$bin" completion bash
	expect_out "compdef" "$bin" completion zsh
	expect_ok "$bin" prompt

	# The completion scripts must survive being eval'd from a shell rc, and
	# must query the binary being completed rather than PATH.
	if command -v bash >/dev/null; then
		got=$(bash_clean -c '
			eval "$('"$bin"' completion bash)" || exit 1
			COMP_WORDS=('"$bin"' preset ""); COMP_CWORD=2; COMPREPLY=()
			_carship || exit 1
			printf "%s" "${COMPREPLY[*]}"' 2>&1)
		case $got in
		*catppuccin-powerline*) printf 'ok   bash completion lists presets\n' ;;
		*)
			printf 'FAIL bash completion\n  got [%s]\n' "$got"
			status=1
			;;
		esac
	fi

	if command -v zsh >/dev/null; then
		# After compinit.
		got=$(zsh_clean -c '
			autoload -Uz compinit; compinit -u -d "'"$work"'/zcd1"
			eval "$('"$bin"' completion zsh)"
			print -r -- "${_comps[carship]:-NONE}"' 2>&1)
		case $got in
		*_carship*) printf 'ok   zsh completion registers after compinit\n' ;;
		*)
			printf 'FAIL zsh completion after compinit\n  got [%s]\n' "$got"
			status=1
			;;
		esac

		# Deferred registration happens on the first prompt, which only a real
		# terminal draws, so drive these through script(1).
		zsh_prompt_check() {
			rcbody=$1
			label=$2
			mkdir -p "$work/pty"
			printf '%s\n' "$rcbody" >"$work/pty/.zshrc"
			printf 'exit\n' >"$work/pty/in"
			rm -f "$work/pty/result"
			ZDOTDIR="$work/pty" script -qec "zsh -i" /dev/null \
				<"$work/pty/in" >/dev/null 2>&1 || true
			if grep -q '_carship' "$work/pty/result" 2>/dev/null; then
				printf 'ok   %s\n' "$label"
			else
				printf 'FAIL %s\n  got [%s]\n' "$label" \
					"$(cat "$work/pty/result" 2>/dev/null)"
				status=1
			fi
		}

		zsh_prompt_check "eval \"\$($bin completion zsh)\"
autoload -Uz compinit
compinit -u -d $work/pty/zcd
precmd_functions+=(_cs_probe)
_cs_probe() { print -r -- \"\${_comps[carship]:-NONE}\" > $work/pty/result }" \
			"zsh completion registers before compinit"

		zsh_prompt_check "eval \"\$($bin init zsh)\"
autoload -Uz compinit
compinit -u -d $work/pty/zcd2
precmd_functions+=(_cs_probe)
_cs_probe() { print -r -- \"\${_comps[carship]:-NONE}\" > $work/pty/result }" \
			"zsh init carries completion"
	fi

	if command -v zsh >/dev/null; then
		got=$(CARSHIP_NO_COMPLETION=1 ZDOTDIR="$work/zdot" zsh -ic '
			eval "$('"$bin"' init zsh)"
			autoload -Uz compinit; compinit -u -d "'"$work"'/zcd4"
			print -r -- "${_comps[carship]:-NONE}"' 2>&1)
		case $got in
		*NONE*) printf 'ok   CARSHIP_NO_COMPLETION opts out\n' ;;
		*)
			printf 'FAIL CARSHIP_NO_COMPLETION ignored\n  got [%s]\n' "$got"
			status=1
			;;
		esac
	fi

	if command -v bash >/dev/null; then
		got=$(bash_clean -ic 'eval "$('"$bin"' init bash)"; complete -p carship' 2>&1)
		case $got in
		*_carship*) printf 'ok   bash init carries completion\n' ;;
		*)
			printf 'FAIL bash init completion\n  got [%s]\n' "$got"
			status=1
			;;
		esac
	fi

	# `preset set` writes the config file and keeps one backup.
	CARSHIP_CONFIG="$work/set.toml" "$bin" preset set nord >/dev/null 2>&1
	CARSHIP_CONFIG="$work/set.toml" "$bin" preset set dracula >/dev/null 2>&1
	if grep -q "palette = 'dracula'" "$work/set.toml" 2>/dev/null &&
		grep -q "palette = 'nord'" "$work/set.toml.bak" 2>/dev/null; then
		printf 'ok   preset set writes config and backs up\n'
	else
		printf 'FAIL preset set\n'
		status=1
	fi

	# An unknown preset must leave the existing config alone.
	CARSHIP_CONFIG="$work/set.toml" "$bin" preset set nosuchpreset >/dev/null 2>&1 || true
	if grep -q "palette = 'dracula'" "$work/set.toml" 2>/dev/null; then
		printf 'ok   preset set rejects unknown names safely\n'
	else
		printf 'FAIL preset set clobbered config on error\n'
		status=1
	fi

	# An absent right_format must not report a parse error or print a fallback,
	# since zsh would run this on every prompt.
	err=$(CARSHIP_CONFIG="$work/set.toml" "$bin" prompt --shell=zsh --right 2>&1 >/dev/null)
	out=$(CARSHIP_CONFIG="$work/set.toml" "$bin" prompt --shell=zsh --right 2>/dev/null)
	if [ -z "$err" ] && [ -z "$out" ]; then
		printf 'ok   empty right_format is silent\n'
	else
		printf 'FAIL right_format\n  stderr [%s]\n  stdout [%s]\n' "$err" "$out"
		status=1
	fi

	# A relative PATH element resolves against the working directory, so a
	# repository could otherwise ship its own "go" and have it run at prompt
	# time. Version lookups must ignore those elements.
	mkdir -p "$work/hostile"
	printf 'module hostile\n' >"$work/hostile/go.mod"
	printf '#!/bin/sh\ntouch "%s/executed"\necho "go version go1.0.0"\n' "$work" \
		>"$work/hostile/go"
	chmod +x "$work/hostile/go"

	path_safe=yes
	for prefix in ".:" ":" "./bin:"; do
		rm -f "$work/executed"
		(cd "$work/hostile" && PATH="$prefix$PATH" "$bin" module golang) >/dev/null 2>&1 || true
		[ -e "$work/executed" ] && path_safe=no
	done
	if [ "$path_safe" = yes ]; then
		printf 'ok   relative PATH elements are ignored when running tools\n'
	else
		printf 'FAIL a relative PATH element let a local binary run\n'
		status=1
	fi

	# No preset may paint a segment in its own background colour.
	if command -v python3 >/dev/null; then
		python3 "$root/tests/check-contrast.py" "$bin" || status=1
	fi

	# Every embedded preset must parse and render.
	for p in $("$bin" preset --list); do
		"$bin" preset "$p" >"$work/preset.toml"
		err=$(CARSHIP_CONFIG="$work/preset.toml" "$bin" prompt 2>&1 >/dev/null)
		if [ -n "$err" ]; then
			printf 'FAIL preset %s: %s\n' "$p" "$err"
			status=1
		else
			printf 'ok   preset %s\n' "$p"
		fi
	done
else
	printf 'skip cli tests: %s not built\n' "$bin"
fi

exit $status
