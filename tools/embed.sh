#!/bin/sh
# embed.sh OUT.c OUT.h FILE...
# Emits every FILE as a byte array reachable by its path via asset_find().
set -eu

out_c=$1
out_h=$2
shift 2

cat >"$out_h" <<'EOF'
#ifndef CARSHIP_ASSETS_H
#define CARSHIP_ASSETS_H
#include <stddef.h>

struct asset {
	const char *name;
	const char *data;
	size_t len;
};

extern const struct asset carship_assets[];
extern const size_t carship_assets_count;

const struct asset *asset_find(const char *name);

#endif
EOF

{
	printf '#include "assets.h"\n#include <string.h>\n\n'

	i=0
	for f in "$@"; do
		printf 'static const char blob_%d[] = {' "$i"
		od -An -v -tu1 <"$f" | tr -s ' ' '\n' | while read -r b; do
			[ -n "$b" ] && printf '%s,' "$b"
		done
		printf '0};\n'
		i=$((i + 1))
	done

	printf '\nconst struct asset carship_assets[] = {\n'
	i=0
	for f in "$@"; do
		size=$(wc -c <"$f" | tr -d ' ')
		printf '\t{"%s", blob_%d, %s},\n' "$f" "$i" "$size"
		i=$((i + 1))
	done
	printf '};\nconst size_t carship_assets_count = %d;\n\n' "$#"

	cat <<'EOF'
const struct asset *asset_find(const char *name)
{
	for (size_t i = 0; i < carship_assets_count; i++)
		if (strcmp(carship_assets[i].name, name) == 0)
			return &carship_assets[i];
	return NULL;
}
EOF
} >"$out_c"
