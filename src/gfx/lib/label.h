#ifndef LABEL_H
#define LABEL_H

#include <std/std_base.h>
#include <stdint.h>
#include "rect.h"
#include "ca_layer.h"
#include "color.h"

__BEGIN_DECLS

typedef struct label {
	//common
	Rect frame;
	char needs_redraw;
	ca_layer* layer;
	struct view* superview;

	char* text;
	Color text_color;
} Label;

Label* create_label(Rect frame, char* text);
void label_teardown(Label* label);

__END_DECLS

#endif
