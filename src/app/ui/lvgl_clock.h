#ifndef LVGL_CLOCK_H
#define LVGL_CLOCK_H

#include <lvgl.h>

/* Fungsi untuk melakukan inisialisasi gaya, warna, dan membuat objek jam */
void lvgl_live_preview_init(void);

/* Fungsi untuk memperbarui jam menggunakan animasi sinus/kosinus dan jarum */
void set_clock_time(int hour, int minute, int second, const char* date_str);

/* Fungsi untuk menyembunyikan jam agar tidak tumpang tindih dengan menu lain */
void hide_lvgl_clock(void);

/* Fungsi untuk memunculkan kembali jam setelah sempat disembunyikan */
void show_lvgl_clock(void);

#endif // LVGL_CLOCK_H
