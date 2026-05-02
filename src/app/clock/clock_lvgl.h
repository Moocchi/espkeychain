#ifndef CLOCK_LVGL_H
#define CLOCK_LVGL_H

#include "../core/app_config.h"

void my_disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map);
void runRealtimeClock();

#endif
