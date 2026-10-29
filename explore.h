#ifndef LSM_EXPLORE_H
#define LSM_EXPLORE_H
/* History positions use measured monotonic timestamps, never sample numbers. */
int history_nearest(const double *times, int capacity, int cursor, int length, double timestamp);
float history_visible_peak(const float *values, int stride, const double *times, int capacity,
                           int cursor, int length, double window);
int history_next_window(int current);
int panel_sidebar_width(int window_width, int requested);
int explore_tests(void);
#endif
