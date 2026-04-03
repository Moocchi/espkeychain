#include "lvgl_clock.h"
#include <math.h>

/* Objek jarum dan pusat */
static lv_obj_t * hour_line = NULL;
static lv_obj_t * min_line = NULL;
static lv_obj_t * sec_line = NULL;
static lv_obj_t * center_dot = NULL;
static lv_obj_t * date_label = NULL;

/* 
 * Array titik untuk jarum (harus static agar nilainya tetap ada di memori di luar scope fungsi) 
 */
static lv_point_precise_t hour_pts[2];
static lv_point_precise_t min_pts[2];
static lv_point_precise_t sec_pts[2];

/* Fungsi pembantu untuk konversi Math derajat ke radian */
static float get_rad(float angle) {
    return angle * 3.14159f / 180.0f;
}

/* Fungsi memutar jarum dan mengupdate nilai */
void set_clock_time(int hour, int minute, int second, const char* date_str) {
    /* 
     * Mengambil nilai tengah layar secara dinamis! 
     * Kesalahan posisi sebelumnya karena layar simulator ternyata ukurannya bukan 240x240, 
     * melainkan lebih besar, tapi titik pusat koordinat manual kaku di 120.
     */
    int cx = lv_obj_get_width(lv_screen_active()) / 2;
    int cy = lv_obj_get_height(lv_screen_active()) / 2;
    
    /* Fallback aman kalau ukuran layar belum terupdate/0 pada detik pertama init */
    if (cx <= 0) cx = 120;
    if (cy <= 0) cy = 120;

    /* Sudut 0 menit/jam berada di angka 12 (atas).
       Karena hitungan sin/cos 0 derajat berawal dari arah kanan (jam 3), kita kurangi 90 derajat */
    float hour_angle = ((hour % 12) * 30.0f + (minute * 0.5f)) - 90.0f;
    float min_angle = (minute * 6.0f + (second * 0.1f)) - 90.0f;
    float sec_angle = (second * 6.0f) - 90.0f;

    /* Panjang dari masing-masing jarum */
    int hour_len = 50;
    int min_len = 75;
    int sec_len = 95;

    /* Titik pangkal semua jarum kita taruh persis di koordinat center dinamis tadi */
    hour_pts[0].x = cx; hour_pts[0].y = cy;
    min_pts[0].x = cx;  min_pts[0].y = cy;
    sec_pts[0].x = cx;  sec_pts[0].y = cy;

    /* Perhitungan titik ujung jarum menggunakan rumus sin/cos dengan titik pusat yang presisi di tengah */
    hour_pts[1].x = cx + (int)(hour_len * cos(get_rad(hour_angle)));
    hour_pts[1].y = cy + (int)(hour_len * sin(get_rad(hour_angle)));

    min_pts[1].x = cx + (int)(min_len * cos(get_rad(min_angle)));
    min_pts[1].y = cy + (int)(min_len * sin(get_rad(min_angle)));

    sec_pts[1].x = cx + (int)(sec_len * cos(get_rad(sec_angle)));
    sec_pts[1].y = cy + (int)(sec_len * sin(get_rad(sec_angle)));

    /* Update posisi garis jarum */
    if (hour_line) lv_line_set_points(hour_line, hour_pts, 2);
    if (min_line) lv_line_set_points(min_line, min_pts, 2);
    if (sec_line) lv_line_set_points(sec_line, sec_pts, 2);
    
    /* Center pivot tetap di atas menutupi semua ujung pangkal jarum */
    if (center_dot) lv_obj_move_foreground(center_dot);

    /* Update teks tanggal di bawah */
    if (date_label && date_str) {
        lv_label_set_text(date_label, date_str);
    }
}

void lvgl_live_preview_init(void)
{
    static bool is_init_done = false;
    if(is_init_done) return;
    is_init_done = true;

    /* Latar belakang layar jam: hitam total */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    /* 1. LAYER BAWAH: Garis Jarum Menit (Minute) */
    min_line = lv_line_create(lv_screen_active());
    lv_obj_set_style_line_width(min_line, 4, 0);  // Sedang
    lv_obj_set_style_line_color(min_line, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(min_line, true, 0);
    lv_obj_set_style_pad_all(min_line, 0, 0);
    lv_obj_set_pos(min_line, 0, 0);

    /* 2. LAYER TENGAH KE BAWAH: Garis Jarum Jam (Hour) */
    hour_line = lv_line_create(lv_screen_active());
    lv_obj_set_style_line_width(hour_line, 6, 0); // Tebal
    lv_obj_set_style_line_color(hour_line, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(hour_line, true, 0);
    lv_obj_set_style_pad_all(hour_line, 0, 0);
    lv_obj_set_pos(hour_line, 0, 0); 

    /* 3. LAYER TENGAH: Label Tanggal (Format Ddd, DD Mmm YYYY) */
    date_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, -60); 

    /* 4. LAYER ATAS: Garis Jarum Detik (Second) */
    sec_line = lv_line_create(lv_screen_active());
    lv_obj_set_style_line_width(sec_line, 2, 0);  // Tipis
    lv_obj_set_style_line_color(sec_line, lv_color_hex(0x2196F3), 0); // Biru terang
    lv_obj_set_style_line_rounded(sec_line, true, 0);
    lv_obj_set_style_pad_all(sec_line, 0, 0);
    lv_obj_set_pos(sec_line, 0, 0);

    /* 5. LAYER PALING ATAS: Titik tengah penahan (Pivot) */
    center_dot = lv_obj_create(lv_screen_active());
    lv_obj_set_size(center_dot, 10, 10);
    lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, 0); // Jadi lingkaran asli
    lv_obj_set_style_bg_color(center_dot, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_border_width(center_dot, 2, 0);
    lv_obj_set_style_border_color(center_dot, lv_color_black(), 0); /* Ring hitam pinggir! */
    lv_obj_align(center_dot, LV_ALIGN_CENTER, 0, 0); /* Pastikan titik di tengah sempurna */
}

void hide_lvgl_clock(void) {
    if (hour_line) lv_obj_add_flag(hour_line, LV_OBJ_FLAG_HIDDEN);
    if (min_line) lv_obj_add_flag(min_line, LV_OBJ_FLAG_HIDDEN);
    if (sec_line) lv_obj_add_flag(sec_line, LV_OBJ_FLAG_HIDDEN);
    if (center_dot) lv_obj_add_flag(center_dot, LV_OBJ_FLAG_HIDDEN);
    if (date_label) lv_obj_add_flag(date_label, LV_OBJ_FLAG_HIDDEN);
}

void show_lvgl_clock(void) {
    lvgl_live_preview_init();
    if (hour_line) lv_obj_remove_flag(hour_line, LV_OBJ_FLAG_HIDDEN);
    if (min_line) lv_obj_remove_flag(min_line, LV_OBJ_FLAG_HIDDEN);
    if (sec_line) lv_obj_remove_flag(sec_line, LV_OBJ_FLAG_HIDDEN);
    if (center_dot) lv_obj_remove_flag(center_dot, LV_OBJ_FLAG_HIDDEN);
    if (date_label) lv_obj_remove_flag(date_label, LV_OBJ_FLAG_HIDDEN);
}
