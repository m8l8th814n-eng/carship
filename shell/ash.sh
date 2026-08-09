case $- in
*i*) ;;
*) return ;;
esac

# busybox ash has no preexec hook, so command duration is not reported here;
# everything else works the same as in bash and zsh.
carship_prompt() {
	CARSHIP_STATUS=$?
	CARSHIP_JOBS=$(jobs 2>/dev/null | wc -l)

	"@CARSHIP@" prompt --shell=ash \
		--status="$CARSHIP_STATUS" \
		--jobs="$CARSHIP_JOBS"
}

CARSHIP_SHELL="ash"
export CARSHIP_SHELL

PS1='$(carship_prompt)'
