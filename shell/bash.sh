case $- in
*i*) ;;
*) return ;;
esac

carship_preexec() {
	[ -n "${CARSHIP_PREEXEC_READY:-}" ] || return
	CARSHIP_PREEXEC_READY=""
	CARSHIP_START_TIME=$("@CARSHIP@" time)
}

carship_precmd() {
	CARSHIP_STATUS=$?
	CARSHIP_PIPESTATUS=("${CARSHIP_LAST_PIPESTATUS[@]}")

	if [ -n "${CARSHIP_START_TIME:-}" ]; then
		CARSHIP_DURATION=$(("$("@CARSHIP@" time)" - CARSHIP_START_TIME))
		CARSHIP_START_TIME=""
	else
		CARSHIP_DURATION=""
	fi

	CARSHIP_JOBS=$(jobs -p | wc -l)
	CARSHIP_PREEXEC_READY="1"
}

# PROMPT_COMMAND runs after PIPESTATUS has already been clobbered by the
# trap, so stash it in the DEBUG trap's sibling assignment first.
carship_capture_pipestatus() {
	CARSHIP_LAST_PIPESTATUS=("${PIPESTATUS[@]}")
}

if [[ -z "${carship_preserved_prompt_command:-}" ]]; then
	carship_preserved_prompt_command="${PROMPT_COMMAND:-}"
fi

if [[ "$(type -t "${carship_preserved_prompt_command}")" == "function" ]]; then
	PROMPT_COMMAND="carship_capture_pipestatus; ${carship_preserved_prompt_command}; carship_precmd"
else
	PROMPT_COMMAND="carship_capture_pipestatus; ${carship_preserved_prompt_command:+${carship_preserved_prompt_command}; }carship_precmd"
fi

if [[ -z "${CARSHIP_DEBUG_TRAP_INSTALLED:-}" ]]; then
	carship_previous_debug_trap="$(trap -p DEBUG)"
	carship_previous_debug_trap="${carship_previous_debug_trap#trap -- \'}"
	carship_previous_debug_trap="${carship_previous_debug_trap%\' DEBUG}"

	if [[ -n "$carship_previous_debug_trap" ]]; then
		trap "$carship_previous_debug_trap; carship_preexec" DEBUG
	else
		trap 'carship_preexec' DEBUG
	fi
	CARSHIP_DEBUG_TRAP_INSTALLED=1
fi

CARSHIP_SHELL="bash"
export CARSHIP_SHELL

PS1='$("@CARSHIP@" prompt --shell=bash --terminal-width="${COLUMNS}" --status="${CARSHIP_STATUS}" --pipestatus="${CARSHIP_PIPESTATUS[*]}" --jobs="${CARSHIP_JOBS}" --cmd-duration="${CARSHIP_DURATION}")'
PS2='$("@CARSHIP@" prompt --shell=bash --continuation)'
