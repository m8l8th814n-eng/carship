#compdef carship

_carship() {
	local -a commands shells
	local self="${words[1]:-carship}"
	commands=(
		'prompt:Print the full prompt'
		'init:Print the shell integration script'
		'preset:Print a built-in preset configuration'
		'completion:Print the shell completion script'
		'module:Print a single module'
		'explain:Show what each module contributes'
		'config:Edit the configuration file'
		'print-config:Print the computed configuration'
		'time:Print the current time in milliseconds'
		'help:Show usage'
	)
	shells=(bash zsh ash)

	_arguments -C \
		'(- *)--help[Show usage]' \
		'(- *)--version[Show version]' \
		'1: :->command' \
		'*:: :->args'

	case $state in
	command)
		_describe -t commands 'carship command' commands
		;;
	args)
		case $words[1] in
		init)
			_arguments '1:shell:(bash zsh ash)' '--print-full-init[Print the full script]'
			;;
		completion)
			_arguments '1:shell:(bash zsh ash)'
			;;
		preset)
			if [[ $words[2] == set ]]; then
				_arguments "2:preset:(\$($self preset --list 2>/dev/null))"
			else
				_arguments \
					"1:preset:(set \$($self preset --list 2>/dev/null))" \
					'--list[List available presets]' \
					'(-o --output)'{-o,--output}'[Write to a file]:file:_files'
			fi
			;;
		module)
			_arguments "1:module:(\$($self module --list 2>/dev/null))"
			;;
		prompt)
			_arguments \
				'--status[Exit status of the last command]:status:' \
				'--cmd-duration[Duration of the last command in ms]:duration:' \
				'--jobs[Number of running jobs]:jobs:' \
				'--keymap[Current keymap]:keymap:' \
				'--terminal-width[Terminal width in columns]:width:' \
				'--pipestatus[Exit statuses of the last pipeline]:pipestatus:' \
				'--shell[Shell to target]:shell:(bash zsh ash)'
			;;
		esac
		;;
	esac
}

_carship_register() {
	(( $+functions[compdef] )) || return 1
	compdef _carship carship
	return 0
}

# Three ways in: sourced by the completion system as an fpath file named
# _carship, eval'd after compinit, or eval'd before compinit (which is what
# happens when carship is loaded at the top of .zshrc and a framework runs
# compinit further down). In the last case compdef does not exist yet, so
# registration waits for the first prompt.
if [ "$funcstack[1]" = "_carship" ]; then
	_carship "$@"
elif ! _carship_register; then
	autoload -Uz add-zsh-hook

	_carship_deferred_register() {
		_carship_register && add-zsh-hook -d precmd _carship_deferred_register
	}

	add-zsh-hook precmd _carship_deferred_register
fi
