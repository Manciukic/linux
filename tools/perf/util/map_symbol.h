// SPDX-License-Identifier: GPL-2.0
#ifndef __PERF_MAP_SYMBOL
#define __PERF_MAP_SYMBOL 1

#include <linux/types.h>
#include "map.h"
#include "maps.h"

struct maps;
struct map;
struct symbol;

struct map_symbol {
	struct maps   *maps;
	struct map    *map;
	struct symbol *sym;
};

struct addr_map_symbol {
	struct map_symbol ms;
	u64	      addr;
	u64	      al_addr;
	u64	      phys_addr;
	u64	      data_page_size;
};

static inline void map_symbol__put_members(struct map_symbol *ms)
{
	maps__put(ms->maps);
	map__put(ms->map);
}

static inline void map_symbol__zput_members(struct map_symbol *ms)
{
	maps__zput(ms->maps);
	map__zput(ms->map);
}
#endif // __PERF_MAP_SYMBOL
