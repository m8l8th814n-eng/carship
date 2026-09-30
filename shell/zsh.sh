[[ -o interactive ]] || return

carship_preexec() {
	CARSHIP_START_TIME=$("@CARSHIP@" time)
}

carship_precmd() {
	CARSHIP_STATUS=$?
	CARSHIP_PIPESTATUS=("${pipestatus[@]}")

	if [[ -n "${CARSHIP_START_TIME}" ]]; then
		CARSHIP_DURATION=$(($("@CARSHIP@" time) - CARSHIP_START_TIME))
		unset CARSHIP_START_TIME
	else
		CARSHIP_DURATION=""
	fi

	CARSHIP_JOBS="${#jobstates}"
}

autoload -Uz add-zsh-hook
add-zsh-hook preexec carship_preexec
add-zsh-hook precmd carship_precmd

# zsh 5.3 and newer chains the special zle- widgets through
# add-zle-hook-widget, and every plugin worth trusting registers that way.
# Taking the widget over by hand and calling the previous one through works
# until something wraps carship's widget in turn: the chain then runs back into
# carship, and zsh gives up with
#
#     azhw:zle-line-finish:9: job table full or recursion limit exceeded
#
# The hook functions below therefore never call a saved "original" widget. The
# dispatcher already runs the other hooks.
carship_have_zle_hooks() {
	# add-zle-hook-widget opens with "zmodload -e zsh/zle || return 1" and says
	# nothing when that fails, so a plugin-less rc file would lose every hook it
	# registered. zle is normally loaded by the first "zle -N" anywhere, which
	# there is no guarantee has happened yet.
	zmodload -i zsh/zle 2>/dev/null || return 1
	zmodload -i zsh/parameter 2>/dev/null || return 1
	zmodload -i zsh/zleparameter 2>/dev/null || return 1

	[[ $(type -w -- add-zle-hook-widget) == *': function' ]] && return 0
	# Loaded in a subshell so a failed lookup leaves nothing behind.
	(autoload -Uz +X -- add-zle-hook-widget 2>/dev/null)
}

carship_azhw=""
if carship_have_zle_hooks; then
	autoload -Uz add-zle-hook-widget
	carship_azhw=1
fi
unfunction carship_have_zle_hooks

# A carship from before this shell script used the dispatcher may already have
# taken these widgets over here. Hand them back rather than leaving an older
# carship sitting in the chain.
if [[ -n "$carship_azhw" ]]; then
	carship_release_widget() {
		local hook="zle-$1" mine="carship_zle_$2" orig="carship_orig_$2"

		add-zle-hook-widget -d "$hook" "user:$mine" 2>/dev/null
		[[ ${widgets[$hook]:-} == "user:$mine" ]] || return 0

		if (( ${+widgets[$orig]} )); then
			zle -A "$orig" "$hook"
			zle -D "$orig"
		else
			zle -D "$hook"
		fi
		(( ${+functions[$mine]} )) && unfunction "$mine"
		return 0
	}
	carship_release_widget keymap-select keymap_select
	carship_release_widget line-init line_init
	carship_release_widget line-finish line_finish
	unfunction carship_release_widget
fi

# Track the vi keymap so the character module can show its vicmd symbol.
carship_keymap_select() {
	CARSHIP_KEYMAP="$KEYMAP"
	zle reset-prompt
}

CARSHIP_ZLE_HOOKS=""
if [[ -n "$carship_azhw" ]]; then
	add-zle-hook-widget zle-keymap-select carship_keymap_select
	# Proof that the dispatcher took the hook, rather than trust: it declines
	# quietly, and a prompt that has silently stopped tracking the vi keymap is
	# not worth the tidier code.
	[[ ${widgets[zle-keymap-select]:-} == "user:azhw:zle-keymap-select" ]] &&
		CARSHIP_ZLE_HOOKS=1
fi
unset carship_azhw

if [[ -z "${CARSHIP_ZLE_HOOKS}" ]]; then
	# Any widget already bound here is kept and called through.
	if [[ -n ${widgets[zle-keymap-select]} &&
		${widgets[zle-keymap-select]} != user:carship_zle_keymap_select ]]; then
		zle -A zle-keymap-select carship_orig_keymap_select
	fi

	carship_zle_keymap_select() {
		CARSHIP_KEYMAP="$KEYMAP"
		(( ${+widgets[carship_orig_keymap_select]} )) && zle carship_orig_keymap_select
		zle reset-prompt
	}
	zle -N zle-keymap-select carship_zle_keymap_select
fi

# Key bindings. Set CARSHIP_NO_KEYBINDINGS=1 before loading carship to keep
# your own bindings untouched.
if [[ -z "${CARSHIP_NO_KEYBINDINGS}" ]]; then
	zmodload -i zsh/terminfo 2>/dev/null

	# terminfo key sequences only match while the terminal is in application
	# mode, which zle has to switch on for the duration of each line.
	if (( ${+terminfo[smkx]} && ${+terminfo[rmkx]} )); then
		carship_line_init() { echoti smkx }
		carship_line_finish() { echoti rmkx }

		if [[ -n "${CARSHIP_ZLE_HOOKS}" ]]; then
			add-zle-hook-widget zle-line-init carship_line_init
			add-zle-hook-widget zle-line-finish carship_line_finish
		else
			carship_zle_line_init() {
				carship_line_init
				(( ${+widgets[carship_orig_line_init]} )) && zle carship_orig_line_init
			}
			carship_zle_line_finish() {
				carship_line_finish
				(( ${+widgets[carship_orig_line_finish]} )) && zle carship_orig_line_finish
			}

			if [[ -n ${widgets[zle-line-init]} &&
				${widgets[zle-line-init]} != user:carship_zle_line_init ]]; then
				zle -A zle-line-init carship_orig_line_init
			fi
			if [[ -n ${widgets[zle-line-finish]} &&
				${widgets[zle-line-finish]} != user:carship_zle_line_finish ]]; then
				zle -A zle-line-finish carship_orig_line_finish
			fi

			zle -N zle-line-init carship_zle_line_init
			zle -N zle-line-finish carship_zle_line_finish
		fi
	fi

	# Up/Down search history for entries starting with what is already typed,
	# instead of walking every command blindly.
	autoload -Uz up-line-or-beginning-search down-line-or-beginning-search
	zle -N up-line-or-beginning-search
	zle -N down-line-or-beginning-search

	carship_bindkey() {
		local key=$1 widget=$2 map
		[[ -n $key ]] || return
		for map in emacs viins vicmd; do
			bindkey -M $map "$key" "$widget"
		done
	}

	carship_bindkey '^[[A' up-line-or-beginning-search
	carship_bindkey '^[[B' down-line-or-beginning-search
	carship_bindkey "${terminfo[kcuu1]}" up-line-or-beginning-search
	carship_bindkey "${terminfo[kcud1]}" down-line-or-beginning-search

	carship_bindkey "${terminfo[khome]}" beginning-of-line
	carship_bindkey "${terminfo[kend]}" end-of-line
	carship_bindkey '^[[H' beginning-of-line
	carship_bindkey '^[[F' end-of-line
	carship_bindkey "${terminfo[kdch1]}" delete-char
	carship_bindkey '^[[3~' delete-char
	carship_bindkey "${terminfo[kich1]}" overwrite-mode
	carship_bindkey "${terminfo[kpp]}" up-line-or-history
	carship_bindkey "${terminfo[knp]}" down-line-or-history
	carship_bindkey "${terminfo[kcbt]}" reverse-menu-complete
	carship_bindkey '^[[Z' reverse-menu-complete

	# Word-wise movement and deletion.
	carship_bindkey '^[[1;5C' forward-word
	carship_bindkey '^[[1;5D' backward-word
	carship_bindkey '^[[1;3C' forward-word
	carship_bindkey '^[[1;3D' backward-word
	carship_bindkey '^H' backward-kill-word
	carship_bindkey '^[[3;5~' kill-word

	carship_bindkey '^R' history-incremental-search-backward
	carship_bindkey '^S' history-incremental-search-forward
	carship_bindkey '^U' backward-kill-line
	carship_bindkey '^[[2~' overwrite-mode

	unfunction carship_bindkey
fi

# Idle redraw for animated prompts. Off unless CARSHIP_ANIMATE is set, since
# TMOUT without a TRAPALRM makes zsh exit on timeout. The trap is therefore
# defined first, and `zle` on its own returns false unless the line editor is
# actually active, which keeps the redraw out of the way of running commands.
if [[ -n "${CARSHIP_ANIMATE}" ]]; then
	TRAPALRM() {
		zle && zle reset-prompt
	}
	if [[ "${CARSHIP_ANIMATE}" == <-> ]]; then
		TMOUT=${CARSHIP_ANIMATE}
	else
		TMOUT=1
	fi
fi

export CARSHIP_SHELL="zsh"

setopt promptsubst

PROMPT='$("@CARSHIP@" prompt --shell=zsh --terminal-width="$COLUMNS" --status="$CARSHIP_STATUS" --pipestatus="${CARSHIP_PIPESTATUS[*]}" --jobs="$CARSHIP_JOBS" --keymap="$CARSHIP_KEYMAP" --cmd-duration="$CARSHIP_DURATION")'
