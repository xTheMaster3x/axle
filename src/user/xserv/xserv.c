#include "xserv.h"
#include <kernel/drivers/mouse/mouse.h>
#include <stddef.h>
#include <std/math.h>
#include <std/panic.h>
#include <std/std.h>
#include <gfx/lib/gfx.h>
#include <gfx/lib/shader.h>
#include <kernel/util/syscall/sysfuncs.h>
#include <kernel/util/multitasking/tasks/task.h>
#include <kernel/drivers/rtc/clock.h>
#include <kernel/drivers/vesa/vesa.h>
#include <tests/gfx_test.h>
#include <kernel/drivers/kb/kb.h>

//has the screen been modified this refresh?
static char dirtied = 0;
static volatile Window* active_window;

ca_layer* layer_snapshot(ca_layer* src, Rect frame) {
	//clip frame
	rect_min_x(frame) = MAX(0, rect_min_x(frame));
	rect_min_y(frame) = MAX(0, rect_min_y(frame));
	if (rect_max_x(frame) >= src->size.width) {
		double overhang = rect_max_x(frame) - src->size.width;
		frame.size.width -= overhang;
	}
	if (rect_max_y(frame) >= src->size.height) {
		double overhang = rect_max_y(frame) - src->size.height;
		frame.size.height -= overhang;
	}

	ca_layer* snapshot = create_layer(frame.size);

	//pointer to current row of snapshot to write to
	uint8_t* snapshot_row = snapshot->raw;
	//pointer to start of row currently writing to snapshot
	uint8_t* row_start = src->raw + (rect_min_y(frame) * src->size.width * gfx_bpp()) + (rect_min_x(frame) * gfx_bpp());

	//copy row by row
	for (int i = 0; i < frame.size.height; i++) {
		memcpy(snapshot_row, row_start, frame.size.width * gfx_bpp());

		snapshot_row += (snapshot->size.width * gfx_bpp());
		row_start += (src->size.width * gfx_bpp());
	}

	return snapshot;
}

Window* containing_window_int(Screen* screen, View* v) {
	//find root window
	View* view = v;
	while (view->superview) {
		view = view->superview;
	}

	//if (screen->window->title_view == view || screen->window->content_view == view) return screen->window

	//traverse view hierarchy, find window which has view as its title or content view
	for (int32_t i = 0; i < screen->window->subviews->size; i++) {
		Window* window = (Window*)array_m_lookup(screen->window->subviews, i);

		//if user passed a Window, check against that
		if (window == (Window*)view) return window;

		if (window->title_view == view || window->content_view == view) return window;
		for (int32_t j = 0; j < window->subviews->size; j++) {
			Window* subwindow = (Window*)array_m_lookup(window->subviews, j);
			if (subwindow->title_view == view || subwindow->content_view == view) return subwindow;
		}
	}
	return screen->window;
}

Rect convert_rect(Rect outer, Rect inner) {
	Rect ret;
	ret.origin.x = inner.origin.x + outer.origin.x;
	ret.origin.y = inner.origin.y + outer.origin.y;
	ret.size.width = MIN(inner.size.width, outer.size.width);
	ret.size.height = MIN(inner.size.height, outer.size.height);
	return ret;
}

Rect convert_frame(View* view, Rect frame) {
	if (!view) return frame;

	Rect ret = convert_rect(view->frame, frame);
	return ret;
}

Rect absolute_frame(Screen* screen, View* view) {
	Rect ret = view->frame;
	//find root view
	View* v = view;
	while (v->superview) {
		v = v->superview;
		ret = convert_frame(v, ret);
	}

	//find containing window
	Window* win = containing_window_int(screen, v);
	ASSERT(win, "couldn't find container window!");

	return convert_rect(win->frame, ret);
}

void draw_bmp(ca_layer* dest, Bmp* bmp) {
	if (!bmp) return;

	blit_layer(dest, bmp->layer, bmp->frame); 

	bmp->needs_redraw = 0;
}

void draw_label(ca_layer* dest, Label* label) {
	if (!label) return;

	View* superview = label->superview;
	Rect frame = label->frame;
	frame.origin = point_zero();

	Color background_color = color_white();
	//try to match text bounding box to superview's background color
	if (superview) {
		background_color = superview->background_color;
	}
	draw_rect(label->layer, rect_make(point_zero(), label->frame.size), background_color, THICKNESS_FILLED);

	//actually render text
	int idx = 0;
	char* str = label->text;
	int x = 0;
	int y = 0;
	while (str[idx]) {
		//go to next line if necessary
		if ((x + CHAR_WIDTH + CHAR_PADDING_W) > frame.size.width || str[idx] == '\n') {
			x = 0;

			//quit if going to next line would exceed view bounds
			if ((y + CHAR_WIDTH + CHAR_PADDING_H) > frame.size.height) break;

			y += CHAR_HEIGHT + CHAR_PADDING_H;
		}

		draw_char(label->layer, str[idx], x, y, label->text_color);

		x += CHAR_WIDTH + CHAR_PADDING_W;

		idx++;
	}

	blit_layer(dest, label->layer, label->frame.origin);

	label->needs_redraw = 0;
}

void draw_view(Screen* screen, View* view) {
	if (!view) return;
	if (!containing_window_int(screen, view)->needs_redraw) {
		return;
	}

	//inform subviews that we're being redrawn
	dirtied = 1;

	//fill view with its background color
	draw_rect(view->layer, rect_make(point_zero(), view->frame.size), view->background_color, THICKNESS_FILLED);

	//draw any labels this view has
	for (int i = 0; i < view->labels->size; i++) {
		Label* label = (Label*)array_m_lookup(view->labels, i);
		draw_label(view->layer, label);
	}

	//draw any bmps this view has
	for (int i = 0; i < view->bmps->size; i++) {
		Bmp* bmp = (Bmp*)array_m_lookup(view->bmps, i);
		if (bmp) {
			draw_bmp(view->layer, bmp);
		}
	}

	/*
	//draw shaders last
	for (unsigned i = 0; i < view->shaders->size; i++) {
		Shader* s = (Shader*)array_m_lookup(view->shaders, i);
		draw_shader(screen, s);
	}
	*/

	//draw each subview of this view
	for (int i = 0; i < view->subviews->size; i++) {
		View* subview = (View*)array_m_lookup(view->subviews, i);
		draw_view(screen, subview);
	}
	view->needs_redraw = 0;
}

bool draw_window(Screen* screen, Window* window) {
	if (!window->needs_redraw) return false;

	dirtied = 1;

	//paint window
	draw_rect(window->layer, rect_make(point_zero(), window->frame.size), window->border_color, window->border_width);

	//only draw a title bar if title_view exists
	if (window->title_view) {
		//update title label of window
		Label* title_label = (Label*)array_m_lookup(window->title_view->labels, 0);
		title_label->text = window->title;
		draw_view(screen, window->title_view);
	}

	//only draw the content view if content_view exists
	if (window->content_view) {
		draw_view(screen, window->content_view);

		//draw dividing border between window border and other content
		if (window->border_width) {
			//inner border
			draw_rect(window->content_view->layer, rect_make(point_zero(), window->content_view->frame.size), color_gray(), window->border_width);
		}
	}

	//composite views of this window into layer
	if (window->title_view) {
		blit_layer(window->layer, window->title_view->layer, window->title_view->frame.origin);
	}
	if (window->content_view) {
		blit_layer(window->layer, window->content_view->layer, window->content_view->frame.origin);
	}

	//draw window border
	draw_rect(window->layer, rect_make(point_zero(), window->frame.size), color_black(), 1);

	window->needs_redraw = 0;

	return true;
}

void add_taskbar(Screen* screen) {
	View* content = screen->window->content_view;
	Size taskbar_size = size_make(content->frame.size.width, content->frame.size.height * 0.045);
	Rect border_r = rect_make(point_make(0, 0), size_make(taskbar_size.width, 5));
	taskbar_size.height -= border_r.size.height;

	Coordinate taskbar_origin = point_make(0, content->frame.size.height - taskbar_size.height + border_r.size.height);
	View* taskbar_view = create_view(rect_make(taskbar_origin, taskbar_size));
	taskbar_view->background_color = color_make(245, 120, 80);
	add_subview(content, taskbar_view);

	//add top 'border' to taskbar
	//TODO add window border API
	View* border = create_view(border_r);
	border->background_color = color_make(200, 80, 245);
	add_subview(taskbar_view, border);

	//inner border seperating top and bottom of taskbar
	View* inner_border = create_view(rect_make(point_make(0, border_r.size.height), size_make(content->frame.size.width, 1)));
	inner_border->background_color = color_make(50, 50, 50);
	add_subview(taskbar_view, inner_border);

	Rect usable = rect_make(point_make(0, border_r.origin.y + border_r.size.height), size_make(taskbar_view->frame.size.width, taskbar_view->frame.size.height - border_r.size.height));
	Coordinate name_label_origin = point_make(taskbar_view->frame.size.width * 0.925, usable.origin.y + (usable.size.height / 2) - (CHAR_HEIGHT / 2));
	Rect label_rect = rect_make(name_label_origin, size_make(taskbar_size.width - name_label_origin.x, taskbar_size.height));
	Label* name_label = create_label(label_rect, "axle OS");
	add_sublabel(taskbar_view, name_label);
}

void add_status_bar(Screen* screen) {
	Rect status_bar_r = rect_make(point_make(0, 0), size_make(screen->window->content_view->frame.size.width, screen->window->frame.size.height * 0.03));
	View* status_bar = create_view(status_bar_r);
	status_bar->background_color = color_make(150, 150, 150);
	add_subview(screen->window->content_view, status_bar);

	//border
	View* border = create_view(rect_make(point_make(0, status_bar_r.size.height - 1), size_make(status_bar_r.size.width, 1)));
	border->background_color = color_purple();
	add_subview(status_bar, border);
}

void draw_desktop(Screen* screen) {
	//paint root desktop
	draw_window(screen, screen->window);
	blit_layer(screen->vmem, screen->window->layer, point_zero());

	//paint every child window
	for (int i = 0; i < screen->window->subviews->size; i++) {
		Window* win = (Window*)(array_m_lookup(screen->window->subviews, i));
		draw_window(screen, win);
			//composite child window onto root window
			blit_layer(screen->vmem, win->layer, win->frame.origin);
		//}
	}
}

void desktop_setup(Screen* screen) {
	//set up background image
	Bmp* background = load_bmp(screen->window->content_view->frame, "background.bmp");
	if (background) {
		add_bmp(screen->window->content_view, background);
	}
	add_status_bar(screen);
	add_taskbar(screen);
}

void draw_cursor(Screen* screen) {
	//actual cursor bitmap
	static Bmp* cursor = 0;
	static bool tried_loading_cursor = false;
	static int max_per_frame_movement = 5;

	if (!tried_loading_cursor) {
		cursor = load_bmp(rect_make(point_zero(), size_make(12, 18)), "cursor.bmp");
		tried_loading_cursor = true;
	}

	//update cursor position
	cursor->frame.origin = mouse_point();

	//drawing cursor shouldn't change dirtied flag
	//save dirtied flag, draw cursor, and restore it
	char prev_dirtied = dirtied;

	//we do not call add_bmp on the cursor
	//we draw it manually to ensure it is always above all other content
	if (cursor) {
		draw_bmp(screen->vmem, cursor);
	}
	else {
		//couldn't load cursor, use backup
		draw_rect(screen->vmem, rect_make(mouse_point(), size_make(10, 12)), color_blue(), THICKNESS_FILLED);
	}

	dirtied = prev_dirtied;
}

char xserv_draw(Screen* screen) {
	screen->finished_drawing = 0;

	dirtied = 0;
	draw_desktop(screen);
	draw_cursor(screen);

	screen->finished_drawing = 1;

	return (char)dirtied;
}

static Window* window_containing_point(Screen* screen, Coordinate p) {
	//traverse window hierarchy, starting with the topmost window
	for (int i = screen->window->subviews->size - 1; i >= 0; i--) {
		Window* w = (Window*)array_m_lookup(screen->window->subviews, i);
		//TODO implement rect_intersects
		if (p.x >= rect_min_x(w->frame)  && p.y >= rect_min_y(w->frame) && p.x - rect_min_x(w->frame) <= w->frame.size.width && p.y - rect_min_y(w->frame) <= w->frame.size.height) {
			return w;
		}
	}
	//wasn't in any subwindows
	//point must be within root window
	return screen->window;
}

static void set_active_window(Screen* screen, Window* grabbed_window) {
	active_window = grabbed_window;
	for (int i = 0; i < screen->window->subviews->size; i++) {
		Window* win = array_m_lookup(screen->window->subviews, i);
		Color color;
		if (win == active_window) {
			color = color_make(120, 245, 80);
		}
		else {
			color = color_make(50, 122, 40);
		}
		set_background_color(win->title_view, color);
		mark_needs_redraw((View*)win);
	}

}

static void process_mouse_events(Screen* screen) {
	static Window* grabbed_window = NULL;
	static Coordinate last_mouse_pos = { -1, -1 };

	//get mouse events
	uint8_t events = mouse_events();
	Coordinate p = mouse_point();

	//0th bit is left mouse button
	bool left = events & 0x1;
	if (left) {
		if (!grabbed_window) {
			//find the window that got this click
			Window* owner = window_containing_point(screen, p);
			grabbed_window = owner;

			//don't move root window! :p
			if (grabbed_window != screen->window && grabbed_window->layer->alpha > 0.0) {
				set_active_window(screen, grabbed_window);

				//bring this window to forefont
				array_m_remove(screen->window->subviews, array_m_index(screen->window->subviews, (type_t)active_window));
				array_m_insert(screen->window->subviews, (type_t)active_window);
			}
		}
		if (last_mouse_pos.x != -1 && grabbed_window != screen->window) {
			//move this window by the difference between current mouse position and last mouse position
			Rect new_frame = grabbed_window->frame;
			new_frame.origin.x -= (last_mouse_pos.x - p.x);
			new_frame.origin.y -= (last_mouse_pos.y - p.y);
			set_frame((View*)grabbed_window, new_frame);
		}
	}
	else {
		if (grabbed_window) {
			//click event ended, release window
			grabbed_window = NULL;
		}
	}
	last_mouse_pos = p;
}

void xserv_quit(Screen* screen) {
	switch_to_text();
	gfx_teardown(screen);
	resign_first_responder();
	_kill();
}

static Label* fps;
void xserv_refresh(Screen* screen) {
	//if (!screen->finished_drawing) return;

	//check if there are any keys pending
	if (haskey()) {
		char ch;
		if ((ch = kgetch())) {
			if (ch == 'q') {
				//quit xserv
				xserv_quit(screen);
			}
			else if (ch == 'r') {
				//force everything to refresh
				screen->window->needs_redraw = 1;
				for (int i = 0; i < screen->window->subviews->size; i++) {
					Window* w = array_m_lookup(screen->window->subviews, i);
					w->needs_redraw = 1;
				}
			}
			else if (ch == 'a') {
				//toggle alpha of topmost window between 0.5 and 1.0
				Window* topmost = array_m_lookup(screen->window->subviews, screen->window->subviews->size - 1);
				float new = 0.5;
				if (topmost->layer->alpha == new) {
					new = 1.0;
				}
				set_alpha((View*)topmost, new);
			}
		}
	}

	double time_start = time();
	xserv_draw(screen);
	double frame_time = (time() - time_start) / 1000.0;
	double fps_conv = 1 / frame_time;

	//draw rect to indicate whether the screen was dirtied this frame
	//red indicates dirtied, green indicates clean
	Rect dirtied_indicator = rect_make(point_make(0, screen->window->size.height - 25), size_make(25, 25));
	draw_rect(screen->window->layer, dirtied_indicator, (dirtied ? color_red() : color_green()), THICKNESS_FILLED);

	//update frame time tracker
	char buf[32];
	itoa(fps_conv, (char*)&buf);
	strcat(buf, " FPS");
	//itoa(frame_time * 1000, &buf);
	//strcat(buf, " ms/frame");
	fps->text = buf;
	draw_label(screen->window->layer, fps);

	//handle mouse events
	process_mouse_events(screen);

	write_screen(screen);

	dirtied = 0;
}

void xserv_pause() {
	switch_to_text();
}

void xserv_resume() {
	switch_to_vesa(0x118, false);
}

void xserv_temp_stop(uint32_t pause_length) {
	xserv_pause();
	sleep(pause_length);
	xserv_resume();
}

void xserv_init_late() {
	//switch to VESA for x serv
	Screen* screen = switch_to_vesa(0x118, true);
	desktop_setup(screen);

	//add FPS tracker
	//don't call add_sublabel on fps because it's drawn manually
	//(drawn manually so we can update text with accurate frame draw time)
	fps = create_label(rect_make(point_make(3, 3), size_make(60, 20)), "FPS counter");
	fps->text_color = color_black();

	test_xserv(screen);

	while (1) {
		xserv_refresh(screen);
		mouse_event_wait();
	}

	_kill();
}

void xserv_init() {
	become_first_responder();
	xserv_init_late();
}
