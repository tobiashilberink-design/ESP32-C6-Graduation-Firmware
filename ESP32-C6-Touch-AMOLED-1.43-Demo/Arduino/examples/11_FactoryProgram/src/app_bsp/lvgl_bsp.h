#pragma once

#include "lvgl.h"
#include "../port_bsp/display_bsp.h"


void Lvgl_PortInit(DisplayPort &display);
bool Lvgl_lock(int timeout_ms);
void Lvgl_unlock(void);