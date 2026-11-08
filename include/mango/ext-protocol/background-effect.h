#ifndef __EXT_PROTOCOL_BACKGROUND_EFFECT_H__
#define __EXT_PROTOCOL_BACKGROUND_EFFECT_H__ 1

#include <wayland-server-core.h>

void background_effect_init(struct wl_display *display);
void background_effect_sync_all(void);

#endif
