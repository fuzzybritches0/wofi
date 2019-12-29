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

static const char* terminals[] = {"kitty", "termite", "gnome-terminal", "weston-terminal"};

enum matching_mode {
	MATCHING_MODE_CONTAINS,
	MATCHING_MODE_FUZZY
};

static uint64_t width, height;
static int64_t x, y;
static struct zwlr_layer_shell_v1* shell;
static GtkWidget* window, *outer_box, *scroll, *entry, *inner_box, *previous_selection = NULL;
static const gchar* filter;
static char* mode = NULL;
static time_t filter_time;
static int64_t filter_rate;
static bool allow_images, allow_markup;
static uint64_t image_size;
static char* cache_file = NULL;
static char* config_dir;
static bool mod_shift;
static bool mod_ctrl;
static char* terminal;
static GtkOrientation outer_orientation;
static bool exec_search;
static struct map* modes;
static enum matching_mode matching;
static bool insensitive;
static bool parse_search;
static struct map* config;

struct node {
	size_t action_count;
	char* mode, **text, *search_text, **actions;
};

struct mode {
	void (*mode_exec)(const gchar* cmd);
};

static void nop() {}

static void add_interface(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
	(void) data;
	if(strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version);
	}
}

static void config_surface(void* data, struct zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t _width, uint32_t _height) {
	(void) data;
	(void) _width;
	(void) _height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	zwlr_layer_surface_v1_set_size(surface, width, height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(surface, true);
	if(x >= 0 && y >= 0) {
		zwlr_layer_surface_v1_set_margin(surface, y, 0, 0, x);
		zwlr_layer_surface_v1_set_anchor(surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	}
}

static void get_input(GtkSearchEntry* entry, gpointer data) {
	(void) data;
	if(utils_get_time_millis() - filter_time > filter_rate) {
		filter = gtk_entry_get_text(GTK_ENTRY(entry));
		filter_time = utils_get_time_millis();
		gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(inner_box));
		gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(inner_box));
	}
}

static void get_search(GtkSearchEntry* entry, gpointer data) {
	(void) data;
	filter = gtk_entry_get_text(GTK_ENTRY(entry));
	gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(inner_box));
	gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(inner_box));
}

static char* parse_images(WofiPropertyBox* box, const char* text, bool create_widgets) {
	char* ret = strdup("");
	struct map* mode_map = map_init();
	map_put(mode_map, "img", "true");
	map_put(mode_map, "text", "true");

	char* tmp = strdup(text);

	struct wl_list modes;
	struct node {
		char* str;
		struct wl_list link;
	};

	wl_list_init(&modes);

	bool data = false;

	char* save_ptr;
	char* str = strtok_r(tmp, ":", &save_ptr);
	do {
		if(str == NULL) {
			break;
		}
		if(map_contains(mode_map, str) || data) {
			struct node* node = malloc(sizeof(struct node));
			node->str = str;
			wl_list_insert(&modes, &node->link);
			data = !data;
		}
	} while((str = strtok_r(NULL, ":", &save_ptr)) != NULL);

	char* tmp2 = strdup(text);
	char* start = tmp2;

	char* mode = NULL;

	struct node* node = wl_container_of(modes.prev, node, link);
	while(true) {
		if(mode == NULL) {
			if(start == NULL) {
				break;
			}
			char* tmp_start = (start - tmp2) + tmp;
			if(!wl_list_empty(&modes) && tmp_start == node->str) {
				if(node->link.prev == &modes) {
					break;
				}
				mode = node->str;
				node = wl_container_of(node->link.prev, node, link);
				str = node->str;
				start = ((str + strlen(str) + 1) - tmp) + tmp2;
				if(((start - tmp2) + text) > (text + strlen(text))) {
					start = NULL;
				}
				if(node->link.prev != &modes) {
					node = wl_container_of(node->link.prev, node, link);
				}
			} else {
				mode = "text";
				str = start;
				if(!wl_list_empty(&modes)) {
					start = (node->str - tmp - 1) + tmp2;
					*start = 0;
					++start;
				}
			}
		} else {
			if(strcmp(mode, "img") == 0 && create_widgets) {
				GdkPixbuf* buf = gdk_pixbuf_new_from_file(str, NULL);
				int width = gdk_pixbuf_get_width(buf);
				int height = gdk_pixbuf_get_height(buf);
				if(height > width) {
					float percent = (float) image_size / height;
					GdkPixbuf* tmp = gdk_pixbuf_scale_simple(buf, width * percent, image_size, GDK_INTERP_BILINEAR);
					g_object_unref(buf);
					buf = tmp;
				} else {
					float percent = (float) image_size / width;
					GdkPixbuf* tmp = gdk_pixbuf_scale_simple(buf, image_size, height * percent, GDK_INTERP_BILINEAR);
					g_object_unref(buf);
					buf = tmp;
				}
				GtkWidget* img = gtk_image_new_from_pixbuf(buf);
				gtk_widget_set_name(img, "img");
				gtk_container_add(GTK_CONTAINER(box), img);
			} else if(strcmp(mode, "text") == 0) {
				if(create_widgets) {
					GtkWidget* label = gtk_label_new(str);
					gtk_widget_set_name(label, "text");
					gtk_label_set_use_markup(GTK_LABEL(label), allow_markup);
					gtk_label_set_xalign(GTK_LABEL(label), 0);
					gtk_container_add(GTK_CONTAINER(box), label);
				} else {
					char* tmp = ret;
					ret = utils_concat(2, ret, str);
					free(tmp);
				}
			}
			mode = NULL;
			if(wl_list_empty(&modes)) {
				break;
			}
		}
	}
	free(tmp);
	free(tmp2);
	map_free(mode_map);

	struct node* tmp_node;
	wl_list_for_each_safe(node, tmp_node, &modes, link) {
		wl_list_remove(&node->link);
		free(node);
	}
	if(create_widgets) {
		free(ret);
		return NULL;
	} else {
		return ret;
	}
}

char* wofi_parse_image_escapes(const char* text) {
	return parse_images(NULL, text, false);
}

static GtkWidget* create_label(char* mode, char* text, char* search_text, char* action) {
	GtkWidget* box = wofi_property_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_name(box, "unselected");
	GtkStyleContext* style = gtk_widget_get_style_context(box);
	gtk_style_context_add_class(style, "entry");
	wofi_property_box_add_property(WOFI_PROPERTY_BOX(box), "mode", mode);
	wofi_property_box_add_property(WOFI_PROPERTY_BOX(box), "action", action);
	if(allow_images) {
		parse_images(WOFI_PROPERTY_BOX(box), text, true);
	} else {
		GtkWidget* label = gtk_label_new(text);
		gtk_widget_set_name(label, "text");
		gtk_label_set_use_markup(GTK_LABEL(label), allow_markup);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_container_add(GTK_CONTAINER(box), label);
	}
	if(parse_search) {
		search_text = strdup(search_text);
		if(allow_images) {
			char* tmp = search_text;
			search_text = parse_images(WOFI_PROPERTY_BOX(box), search_text, false);
			free(tmp);
		}
		if(allow_markup) {
			char* out;
			pango_parse_markup(search_text, -1, 0, NULL, &out, NULL, NULL);
			free(search_text);
			search_text = out;
		}
	}
	wofi_property_box_add_property(WOFI_PROPERTY_BOX(box), "filter", search_text);
	if(parse_search) {
		free(search_text);
	}
	return box;
}

static char* get_cache_path(const gchar* mode) {
	if(cache_file != NULL) {
		return strdup(cache_file);
	}
	char* cache_path = getenv("XDG_CACHE_HOME");
	if(cache_path == NULL) {
		cache_path = utils_concat(3, getenv("HOME"), "/.cache/wofi-", mode);
	} else {
		cache_path = utils_concat(3, cache_path, "/wofi-", mode);
	}
	return cache_path;
}

static void execute_action(const gchar* mode, const gchar* cmd) {
	struct mode* mode_ptr = map_get(modes, mode);
	mode_ptr->mode_exec(cmd);
}

static void activate_item(GtkFlowBox* flow_box, GtkFlowBoxChild* row, gpointer data) {
	(void) flow_box;
	(void) data;
	GtkWidget* box = gtk_bin_get_child(GTK_BIN(row));
	bool primary_action = GTK_IS_EXPANDER(box);
	if(primary_action) {
		box = gtk_expander_get_label_widget(GTK_EXPANDER(box));
	}
	execute_action(wofi_property_box_get_property(WOFI_PROPERTY_BOX(box), "mode"), wofi_property_box_get_property(WOFI_PROPERTY_BOX(box), "action"));
}

static void expand(GtkExpander* expander, gpointer data) {
	(void) data;
	GtkWidget* box = gtk_bin_get_child(GTK_BIN(expander));
	gtk_widget_set_visible(box, !gtk_expander_get_expanded(expander));
}

static gboolean _insert_widget(gpointer data) {
	struct node* node = data;
	GtkWidget* parent;
	if(node->action_count > 1) {
		parent = gtk_expander_new("");
		g_signal_connect(parent, "activate", G_CALLBACK(expand), NULL);
		GtkWidget* box = create_label(node->mode, node->text[0], node->search_text, node->actions[0]);
		gtk_expander_set_label_widget(GTK_EXPANDER(parent), box);

		GtkWidget* exp_box = gtk_list_box_new();
		gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(exp_box), FALSE);
		g_signal_connect(exp_box, "row-activated", G_CALLBACK(activate_item), NULL);
		gtk_container_add(GTK_CONTAINER(parent), exp_box);
		for(size_t count = 1; count < node->action_count; ++count) {
			box = create_label(node->mode, node->text[count], node->search_text, node->actions[count]);
			gtk_container_add(GTK_CONTAINER(exp_box), box);
		}
	} else {
		parent = create_label(node->mode, node->text[0], node->search_text, node->actions[0]);
	}
	gtk_container_add(GTK_CONTAINER(inner_box), parent);
	gtk_widget_show_all(parent);

	if(GTK_IS_EXPANDER(parent)) {
		GtkWidget* box = gtk_bin_get_child(GTK_BIN(parent));
		gtk_widget_set_visible(box, FALSE);
	}

	free(node->mode);
	for(size_t count = 0; count < node->action_count; ++count) {
		free(node->text[count]);
	}
	free(node->text);
	free(node->search_text);
	for(size_t count = 0; count < node->action_count; ++count) {
		free(node->actions[count]);
	}
	free(node->actions);
	free(node);
	return FALSE;
}

void wofi_write_cache(const gchar* mode, const gchar* cmd) {
	char* cache_path = get_cache_path(mode);
	struct wl_list lines;
	wl_list_init(&lines);
	bool inc_count = false;
	if(access(cache_path, R_OK) == 0) {
		FILE* file = fopen(cache_path, "r");
		char* line = NULL;
		size_t size = 0;
		while(getline(&line, &size, file) != -1) {
			char* space = strchr(line, ' ');
			char* nl = strchr(line, '\n');
			if(nl != NULL) {
				*nl = 0;
			}
			if(space != NULL && strcmp(cmd, space + 1) == 0) {
				struct cache_line* node = malloc(sizeof(struct cache_line));
				uint64_t count = strtol(line, NULL, 10) + 1;
				char num[6];
				snprintf(num, 5, "%" PRIu64, count);
				node->line = utils_concat(4, num, " ", cmd, "\n");
				inc_count = true;
				wl_list_insert(&lines, &node->link);
			}
		}
		free(line);
		line = NULL;
		size = 0;
		fseek(file, 0, SEEK_SET);
		while(getline(&line, &size, file) != -1) {
			char* space = strchr(line, ' ');
			char* nl = strchr(line, '\n');
			if(nl != NULL) {
				*nl = 0;
			}
			if(space == NULL || strcmp(cmd, space + 1) != 0) {
				struct cache_line* node = malloc(sizeof(struct cache_line));
				node->line = utils_concat(2, line, "\n");
				wl_list_insert(&lines, &node->link);
			}
		}
		free(line);
		fclose(file);
	}
	if(!inc_count) {
		struct cache_line* node = malloc(sizeof(struct cache_line));
		node->line = utils_concat(3, "1 ", cmd, "\n");
		wl_list_insert(&lines, &node->link);
	}

	FILE* file = fopen(cache_path, "w");
	struct cache_line* node, *tmp;
	wl_list_for_each_safe(node, tmp, &lines, link) {
		fwrite(node->line, 1, strlen(node->line), file);
		free(node->line);
		wl_list_remove(&node->link);
		free(node);
	}

	fclose(file);

	free(cache_path);
}

struct wl_list* wofi_read_cache(char* mode) {
	char* cache_path = get_cache_path(mode);
	struct wl_list* cache = malloc(sizeof(struct wl_list));
	wl_list_init(cache);
	struct wl_list lines;
	wl_list_init(&lines);
	if(access(cache_path, R_OK) == 0) {
		FILE* file = fopen(cache_path, "r");
		char* line = NULL;
		size_t size = 0;
		while(getline(&line, &size, file) != -1) {
			struct cache_line* node = malloc(sizeof(struct cache_line));
			char* lf = strchr(line, '\n');
			if(lf != NULL) {
				*lf = 0;
			}
			node->line = strdup(line);
			wl_list_insert(&lines, &node->link);
		}
		free(line);
		fclose(file);
	}
	while(wl_list_length(&lines) > 0) {
		uint64_t smallest = UINT64_MAX;
		struct cache_line* node, *smallest_node = NULL;
		wl_list_for_each(node, &lines, link) {
			uint64_t num = strtol(node->line, NULL, 10);
			if(num < smallest) {
				smallest = num;
				smallest_node = node;
			}
		}
		char* tmp = strdup(strchr(smallest_node->line, ' ') + 1);
		free(smallest_node->line);
		smallest_node->line = tmp;
		wl_list_remove(&smallest_node->link);
		wl_list_insert(cache, &smallest_node->link);
	}
	free(cache_path);
	return cache;
}

void wofi_insert_widget(char* mode, char** text, char* search_text, char** actions, size_t action_count) {
	struct node* widget = malloc(sizeof(struct node));
	widget->mode = strdup(mode);
	widget->text = malloc(action_count * sizeof(char*));
	for(size_t count = 0; count < action_count; ++count) {
		widget->text[count] = strdup(text[count]);
	}
	widget->search_text = strdup(search_text);
	widget->action_count = action_count;
	widget->actions = malloc(action_count * sizeof(char*));
	for(size_t count = 0; count < action_count; ++count) {
		widget->actions[count] = strdup(actions[count]);
	}
	g_idle_add(_insert_widget, widget);
	utils_sleep_millis(1);
}

bool wofi_allow_images() {
	return allow_images;
}

bool wofi_allow_markup() {
	return allow_markup;
}

uint64_t wofi_get_image_size() {
	return image_size;
}

bool wofi_mod_shift() {
	return mod_shift;
}

bool wofi_mod_control() {
	return mod_ctrl;
}

void wofi_term_run(const char* cmd) {
	if(terminal != NULL) {
		execlp(terminal, terminal, "--", cmd, NULL);
	}
	size_t term_count = sizeof(terminals) / sizeof(char*);
	for(size_t count = 0; count < term_count; ++count) {
		execlp(terminals[count], terminals[count], "--", cmd, NULL);
	}
	fprintf(stderr, "No terminal emulator found please set term in config or use --term\n");
	exit(1);
}

static void select_item(GtkFlowBox* flow_box, gpointer data) {
	(void) data;
	if(previous_selection != NULL) {
		gtk_widget_set_name(previous_selection, "unselected");
	}
	GList* selected_children = gtk_flow_box_get_selected_children(flow_box);
	GtkWidget* box = gtk_bin_get_child(GTK_BIN(selected_children->data));
	g_list_free(selected_children);
	gtk_widget_set_name(box, "selected");
	previous_selection = box;
}

static GtkWidget* get_first_child(GtkContainer* container) {
	GList* children = gtk_container_get_children(container);
	GList* list = children;
	GtkWidget* min_child = NULL;
	int64_t x = INT64_MAX;
	int64_t y = INT64_MAX;
	for(; list != NULL; list = list->next) {
		GtkWidget* child = list->data;
		GtkAllocation alloc;
		gtk_widget_get_allocation(child, &alloc);
		if(alloc.x <= x && alloc.y <= y && alloc.x != -1 && alloc.y != -1 && alloc.width != 1 && alloc.height != 1 && gtk_widget_get_child_visible(child)) {
			x = alloc.x;
			y = alloc.y;
			min_child = child;
		}
	}
	g_list_free(children);
	return min_child;
}

static void activate_search(GtkEntry* entry, gpointer data) {
	(void) data;
	GtkWidget* child = get_first_child(GTK_CONTAINER(inner_box));
	if(exec_search || child == NULL) {
		execute_action(mode, gtk_entry_get_text(entry));
	} else {
		GtkWidget* box = gtk_bin_get_child(GTK_BIN(child));
		bool primary_action = GTK_IS_EXPANDER(box);
		if(primary_action) {
			box = gtk_expander_get_label_widget(GTK_EXPANDER(box));
		}
		execute_action(wofi_property_box_get_property(WOFI_PROPERTY_BOX(box), "mode"), wofi_property_box_get_property(WOFI_PROPERTY_BOX(box), "action"));
	}
}

static gboolean do_filter(GtkFlowBoxChild* row, gpointer data) {
	(void) data;
	GtkWidget* box = gtk_bin_get_child(GTK_BIN(row));
	if(GTK_IS_EXPANDER(box)) {
		box = gtk_expander_get_label_widget(GTK_EXPANDER(box));
	}
	const gchar* text = wofi_property_box_get_property(WOFI_PROPERTY_BOX(box), "filter");
	if(filter == NULL || strcmp(filter, "") == 0) {
		return TRUE;
	}
	if(text == NULL) {
		return FALSE;
	}
	if(insensitive) {
		return strcasestr(text, filter) != NULL;
	} else {
		return strstr(text, filter) != NULL;
	}
}

static gint do_sort(GtkFlowBoxChild* child1, GtkFlowBoxChild* child2, gpointer data) {
	(void) data;
	GtkWidget* box1 = gtk_bin_get_child(GTK_BIN(child1));
	GtkWidget* box2 = gtk_bin_get_child(GTK_BIN(child2));
	if(GTK_IS_EXPANDER(box1)) {
		box1 = gtk_expander_get_label_widget(GTK_EXPANDER(box1));
	}
	if(GTK_IS_EXPANDER(box2)) {
		box2 = gtk_expander_get_label_widget(GTK_EXPANDER(box2));
	}

	const gchar* text1 = wofi_property_box_get_property(WOFI_PROPERTY_BOX(box1), "filter");
	const gchar* text2 = wofi_property_box_get_property(WOFI_PROPERTY_BOX(box2), "filter");
	if(filter == NULL || strcmp(filter, "") == 0) {
		return 0;
	}
	if(text1 == NULL || text2 == NULL) {
		return 0;
	}
	size_t dist1 = utils_distance(text1, filter);
	size_t dist2 = utils_distance(text2, filter);
	if(dist1 < dist2) {
		return -1;
	} else if(dist1 > dist2) {
		return 1;
	} else {
		return 0;
	}
}

static gboolean key_press(GtkWidget* widget, GdkEvent* event, gpointer data) {
	(void) widget;
	(void) data;
	switch(event->key.keyval) {
	case GDK_KEY_Escape:
		exit(0);
		break;
	case GDK_KEY_Up:
	case GDK_KEY_Left:
		break;
	case GDK_KEY_Return:
		mod_shift = (event->key.state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK;
		mod_ctrl = (event->key.state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK;
		if(mod_shift) {
			event->key.state &= ~GDK_SHIFT_MASK;
		}
		if(mod_ctrl) {
			event->key.state &= ~GDK_CONTROL_MASK;
		}
		if(gtk_widget_has_focus(scroll)) {
			gtk_entry_grab_focus_without_selecting(GTK_ENTRY(entry));
		}
		break;
	case GDK_KEY_Tab:
	case GDK_KEY_Down:
	case GDK_KEY_Right:
		if(event->key.keyval == GDK_KEY_Down && outer_orientation == GTK_ORIENTATION_HORIZONTAL) {
			return FALSE;
		} else if(event->key.keyval == GDK_KEY_Right && outer_orientation == GTK_ORIENTATION_VERTICAL) {
			return FALSE;
		}
		if(gtk_widget_has_focus(entry) || gtk_widget_has_focus(scroll)) {
			GtkWidget* child = get_first_child(GTK_CONTAINER(inner_box));
			gtk_widget_grab_focus(child);
			gtk_flow_box_select_child(GTK_FLOW_BOX(inner_box), GTK_FLOW_BOX_CHILD(child));
			return TRUE;
		}
		break;
	case GDK_KEY_Shift_L:case GDK_KEY_Shift_R:
	case GDK_KEY_Control_L:case GDK_KEY_Control_R:
		break;
	default:
		if(!gtk_widget_has_focus(entry)) {
			gtk_entry_grab_focus_without_selecting(GTK_ENTRY(entry));
		}
		break;
	}
	return FALSE;
}

static void* get_plugin_proc(const char* prefix, const char* suffix) {
	char* proc_name = utils_concat(3, "wofi_", prefix, suffix);
	void* proc = dlsym(RTLD_DEFAULT, proc_name);
	free(proc_name);
	return proc;
}

static void add_mode(char* mode) {
	char* dso = strstr(mode, ".so");
	struct mode* mode_ptr = calloc(1, sizeof(struct mode));
	map_put_void(modes, mode, mode_ptr);

	void (*init)(struct map* props);
	const char** (*get_arg_names)();
	size_t (*get_arg_count)();
	if(dso == NULL) {
		init = get_plugin_proc(mode, "_init");
		get_arg_names = get_plugin_proc(mode, "_get_arg_names");
		get_arg_count = get_plugin_proc(mode, "_get_arg_count");
		mode_ptr->mode_exec = get_plugin_proc(mode, "_exec");
	} else {
		char* plugins_dir = utils_concat(2, config_dir, "/plugins/");
		char* full_name = utils_concat(2, plugins_dir, mode);
		void* plugin = dlopen(full_name, RTLD_LAZY | RTLD_LOCAL);
		free(full_name);
		free(plugins_dir);
		init = dlsym(plugin, "init");
		get_arg_names = dlsym(plugin, "get_arg_names");
		get_arg_count = dlsym(plugin, "get_arg_count");
		mode_ptr->mode_exec = dlsym(plugin, "exec");
	}

	const char** arg_names = NULL;
	size_t arg_count = 0;
	if(get_arg_names != NULL && get_arg_count != NULL) {
		arg_names = get_arg_names();
		arg_count = get_arg_count();
	}

	struct map* props = map_init();
	for(size_t count = 0; count < arg_count; ++count) {
		const char* arg = arg_names[count];
		char* full_name = utils_concat(3, mode, "-", arg);
		map_put(props, arg, config_get(config, full_name, NULL));
		free(full_name);
	}

	if(init != NULL) {
		init(props);
	} else {
		fprintf(stderr, "I would love to show %s but Idk what it is\n", mode);
		exit(1);
	}

	map_free(props);
}

static void* start_thread(void* data) {
	(void) data;
	if(strchr(mode, ',') != NULL) {
		char* save_ptr;
		char* str = strtok_r(mode, ",", &save_ptr);
		do {
			add_mode(str);
		} while((str = strtok_r(NULL, ",", &save_ptr)) != NULL);
	} else {
		add_mode(mode);
	}
	return NULL;
}

void wofi_init(struct map* _config) {
	config = _config;
	width = strtol(config_get(config, "width", "1000"), NULL, 10);
	height = strtol(config_get(config, "height", "400"), NULL, 10);
	x = strtol(config_get(config, "x", "-1"), NULL, 10);
	y = strtol(config_get(config, "y", "-1"), NULL, 10);
	bool normal_window = strcmp(config_get(config, "normal_window", "false"), "true") == 0;
	mode = map_get(config, "mode");
	uint8_t orientation = config_get_mnemonic(config, "orientation", "vertical", 2, "vertical", "horizontal");
	outer_orientation = config_get_mnemonic(config, "orientation", "vertical", 2, "horizontal", "vertical");
	uint8_t halign = config_get_mnemonic(config, "halign", "fill", 4, "fill", "start", "end", "center");
	char* default_valign = "start";
	if(outer_orientation == GTK_ORIENTATION_HORIZONTAL) {
		default_valign = "center";
	}
	uint8_t valign = config_get_mnemonic(config, "valign", default_valign, 4, "fill", "start", "end", "center");
	char* prompt = config_get(config, "prompt", mode);
	filter_rate = strtol(config_get(config, "filter_rate", "100"), NULL, 10);
	filter_time = utils_get_time_millis();
	allow_images = strcmp(config_get(config, "allow_images", "false"), "true") == 0;
	allow_markup = strcmp(config_get(config, "allow_markup", "false"), "true") == 0;
	image_size = strtol(config_get(config, "image_size", "32"), NULL, 10);
	cache_file = map_get(config, "cache_file");
	config_dir = map_get(config, "config_dir");
	terminal = map_get(config, "term");
	char* password_char = map_get(config, "password_char");
	exec_search = strcmp(config_get(config, "exec_search", "false"), "true") == 0;
	bool hide_scroll = strcmp(config_get(config, "hide_scroll", "false"), "true") == 0;
	matching = config_get_mnemonic(config, "matching", "contains", 2, "contains", "fuzzy");
	insensitive = strcmp(config_get(config, "insensitive", "false"), "true") == 0;
	parse_search = strcmp(config_get(config, "parse_search", "false"), "true") == 0;
	modes = map_init_void();

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_realize(window);
	gtk_widget_set_name(window, "window");
	gtk_window_set_default_size(GTK_WINDOW(window), width, height);
	gtk_window_resize(GTK_WINDOW(window), width, height);
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
	if(!normal_window) {
		GdkDisplay* disp = gdk_display_get_default();
		struct wl_display* wl = gdk_wayland_display_get_wl_display(disp);
		struct wl_registry* registry = wl_display_get_registry(wl);
		struct wl_registry_listener listener = {
			.global = add_interface,
			.global_remove = nop
		};
		wl_registry_add_listener(registry, &listener, NULL);
		wl_display_roundtrip(wl);
		GdkWindow* gdk_win = gtk_widget_get_window(window);
		gdk_wayland_window_set_use_custom_surface(gdk_win);
		struct wl_surface* wl_surface = gdk_wayland_window_get_wl_surface(gdk_win);
		struct zwlr_layer_surface_v1* surface = zwlr_layer_shell_v1_get_layer_surface(shell, wl_surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "wofi");
		struct zwlr_layer_surface_v1_listener* surface_listener = malloc(sizeof(struct zwlr_layer_surface_v1_listener));
		surface_listener->configure = config_surface;
		surface_listener->closed = nop;
		zwlr_layer_surface_v1_add_listener(surface, surface_listener, NULL);
		wl_surface_commit(wl_surface);
		wl_display_roundtrip(wl);
	}

	outer_box = gtk_box_new(outer_orientation, 0);
	gtk_widget_set_name(outer_box, "outer-box");
	gtk_container_add(GTK_CONTAINER(window), outer_box);
	entry = gtk_search_entry_new();
	gtk_widget_set_name(entry, "input");
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), prompt);
	gtk_container_add(GTK_CONTAINER(outer_box), entry);
	if(password_char != NULL) {
		gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
		gtk_entry_set_invisible_char(GTK_ENTRY(entry), password_char[0]);
	}

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_set_name(scroll, "scroll");
	gtk_container_add(GTK_CONTAINER(outer_box), scroll);
	gtk_widget_set_size_request(scroll, width, height);
	if(hide_scroll) {
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
	}

	inner_box = gtk_flow_box_new();
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(inner_box), 1);
	gtk_orientable_set_orientation(GTK_ORIENTABLE(inner_box), orientation);
	gtk_widget_set_halign(inner_box, halign);
	gtk_widget_set_valign(inner_box, valign);

	gtk_widget_set_name(inner_box, "inner-box");
	gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(inner_box), FALSE);

	GtkWidget* wrapper_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_set_homogeneous(GTK_BOX(wrapper_box), TRUE);
	gtk_container_add(GTK_CONTAINER(wrapper_box), inner_box);
	gtk_container_add(GTK_CONTAINER(scroll), wrapper_box);

	switch(matching) {
	case MATCHING_MODE_CONTAINS:
		gtk_flow_box_set_filter_func(GTK_FLOW_BOX(inner_box), do_filter, NULL, NULL);
		break;
	case MATCHING_MODE_FUZZY:
		gtk_flow_box_set_sort_func(GTK_FLOW_BOX(inner_box), do_sort, NULL, NULL);
		break;
	}

	g_signal_connect(entry, "changed", G_CALLBACK(get_input), NULL);
	g_signal_connect(entry, "search-changed", G_CALLBACK(get_search), NULL);
	g_signal_connect(inner_box, "child-activated", G_CALLBACK(activate_item), NULL);
	g_signal_connect(inner_box, "selected-children-changed", G_CALLBACK(select_item), NULL);
	g_signal_connect(entry, "activate", G_CALLBACK(activate_search), NULL);
	g_signal_connect(window, "key-press-event", G_CALLBACK(key_press), NULL);

	pthread_t thread;
	pthread_create(&thread, NULL, start_thread, NULL);
	gtk_widget_grab_focus(scroll);
	gtk_window_set_title(GTK_WINDOW(window), prompt);
	gtk_widget_show_all(window);
}
