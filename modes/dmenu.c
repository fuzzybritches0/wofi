/*
 *  Copyright (C) 2019 Scoopta
 *  This file is part of Wofi
 *  Wofi is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Wofi is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Wofi.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <wofi.h>

#define MODE "dmenu"

static const char* arg_names[] = {"parse_action"};

static bool parse_action;

void wofi_dmenu_init(struct map* config) {
	parse_action = strcmp(config_get(config, "parse_action", "false"), "true") == 0;

	struct map* cached = map_init();
	struct wl_list* cache = wofi_read_cache(MODE);

	struct cache_line* node, *tmp;
	wl_list_for_each_safe(node, tmp, cache, link) {
		wofi_insert_widget(MODE, &node->line, node->line, &node->line, 1);
		map_put(cached, node->line, "true");
		free(node->line);
		wl_list_remove(&node->link);
		free(node);
	}

	free(cache);

	char* line = NULL;
	size_t size = 0;
	while(getline(&line, &size, stdin) != -1) {
		char* lf = strchr(line, '\n');
		if(lf != NULL) {
			*lf = 0;
		}
		if(map_contains(cached, line)) {
			continue;
		}
		wofi_insert_widget(MODE, &line, line, &line, 1);
	}
	free(line);
	map_free(cached);
}

void wofi_dmenu_exec(const gchar* cmd) {
	char* action = strdup(cmd);
	if(parse_action) {
		if(wofi_allow_images()) {
			free(action);
			action = wofi_parse_image_escapes(cmd);
		}
		if(wofi_allow_markup()) {
			char* out;
			pango_parse_markup(action, -1, 0, NULL, &out, NULL, NULL);
			free(action);
			action = out;
		}
	}
	wofi_write_cache(MODE, cmd);
	printf("%s\n", action);
	free(action);
	exit(0);
}

const char** wofi_dmenu_get_arg_names() {
	return arg_names;
}

size_t wofi_dmenu_get_arg_count() {
	return sizeof(arg_names) / sizeof(char*);
}
