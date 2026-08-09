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

# Track the vi keymap so the character module can show its vicmd symbol.
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

# Key bindings. Set CARSHIP_NO_KEYBINDINGS=1 before loading carship to keep
# your own bindings untouched.
if [[ -z "${CARSHIP_NO_KEYBINDINGS}" ]]; then
	zmodload -i zsh/terminfo 2>/dev/null

	# terminfo key sequences only match while the terminal is in application
	# mode, which zle has to switch on for the duration of each line.
	if (( ${+terminfo[smkx]} && ${+terminfo[rmkx]} )); then
		carship_zle_line_init() {
			echoti smkx
			(( ${+widgets[carship_orig_line_init]} )) && zle carship_orig_line_init
		}
		carship_zle_line_finish() {
			echoti rmkx
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
