_carship() {
	local cur prev self commands shells presets
	COMPREPLY=()
	cur="${COMP_WORDS[COMP_CWORD]}"
	prev="${COMP_WORDS[COMP_CWORD - 1]}"
	# Ask the binary being completed, not whatever "carship" PATH resolves to.
	self="${COMP_WORDS[0]}"
	commands="prompt init preset completion module explain config print-config time help"
	shells="bash zsh ash"

	if [[ $COMP_CWORD -eq 1 ]]; then
		COMPREPLY=($(compgen -W "$commands --help --version" -- "$cur"))
		return 0
	fi

	case "${COMP_WORDS[1]}" in
	init)
		if [[ $COMP_CWORD -eq 2 ]]; then
			COMPREPLY=($(compgen -W "$shells" -- "$cur"))
		else
			COMPREPLY=($(compgen -W "--print-full-init" -- "$cur"))
		fi
		;;
	completion)
		[[ $COMP_CWORD -eq 2 ]] && COMPREPLY=($(compgen -W "$shells" -- "$cur"))
		;;
	preset)
		if [[ "$prev" == "-o" || "$prev" == "--output" ]]; then
			COMPREPLY=($(compgen -f -- "$cur"))
		elif [[ $COMP_CWORD -eq 2 ]]; then
			presets="$("$self" preset --list 2>/dev/null)"
			COMPREPLY=($(compgen -W "$presets set --list" -- "$cur"))
		elif [[ "${COMP_WORDS[2]}" == "set" && $COMP_CWORD -eq 3 ]]; then
			COMPREPLY=($(compgen -W "$("$self" preset --list 2>/dev/null)" -- "$cur"))
		else
			COMPREPLY=($(compgen -W "--list --output" -- "$cur"))
		fi
		;;
	module)
		[[ $COMP_CWORD -eq 2 ]] &&
			COMPREPLY=($(compgen -W "$("$self" module --list 2>/dev/null)" -- "$cur"))
		;;
	prompt)
		COMPREPLY=($(compgen -W "--status --cmd-duration --jobs --keymap --terminal-width --pipestatus --shell" -- "$cur"))
		;;
	esac
	return 0
}

complete -F _carship carship
