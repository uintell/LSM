#include "explore.h"
#include <math.h>
#include <stdio.h>
int history_nearest(const double *times, int capacity, int cursor, int length, double timestamp) {
    if (!times || capacity < 1 || length < 1 || length > capacity || cursor < 0 ||
        cursor >= capacity || !isfinite(timestamp))
        return -1;
    int oldest = (cursor - length + capacity) % capacity,
        newest = (cursor + capacity - 1) % capacity;
    if (timestamp < times[oldest] || timestamp > times[newest])
        return -1;
    int low = 0, high = length;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (times[(oldest + mid) % capacity] < timestamp)
            low = mid + 1;
        else
            high = mid;
    }
    int right = (oldest + (low < length ? low : length - 1)) % capacity;
    if (low > 0) {
        int left = (oldest + low - 1) % capacity;
        if (timestamp - times[left] <= times[right] - timestamp)
            return left;
    }
    return right;
}
float history_visible_peak(const float *values, int stride, const double *times, int capacity,
                           int cursor, int length, double window) {
    if (!values || !times || stride < 1 || capacity < 1 || cursor < 0 || cursor >= capacity ||
        length < 1 || length > capacity || window <= 0)
        return 1;
    double newest = times[(cursor + capacity - 1) % capacity];
    float peak = 1;
    for (int i = 0; i < length; i++) {
        int index = (cursor - length + i + capacity) % capacity;
        float v = values[index * stride];
        if (newest - times[index] <= window && isfinite(v) && v > peak)
            peak = v;
    }
    return peak;
}
int history_next_window(int current) {
    return current == 60 ? 300 : current == 300 ? 900 : 60;
}
int panel_sidebar_width(int window_width, int requested) {
    if (window_width < 1000)
        return 0;
    int max = window_width - 440;
    if (max > 520)
        max = 520;
    if (requested < 220)
        requested = 220;
    if (requested > max)
        requested = max;
    return requested;
}
int explore_tests(void) {
    /* Ring wraps at index 3: chronological data are 20,30,40,50,60,70. */
    double times[6] = {50, 60, 70, 20, 30, 40};
    float values[6] = {5, 6, 7, 900, 3, 4};
    int ok = history_nearest(times, 6, 3, 6, 30) == 4 && history_nearest(times, 6, 3, 6, 54) == 0 &&
             history_nearest(times, 6, 3, 6, 56) == 1;
    ok = ok && history_nearest(times, 6, 3, 6, 19) == -1 &&
         history_nearest(times, 6, 3, 6, 71) == -1 && history_nearest(times, 6, 3, 0, 50) == -1;
    ok = ok && history_visible_peak(values, 1, times, 6, 3, 6, 30) == 7 &&
         history_visible_peak(values, 1, times, 6, 3, 6, 60) == 900;
    double irregular[5] = {1, 2.7, 8, 8.2, 13};
    ok = ok && history_nearest(irregular, 5, 0, 5, 3) == 1 &&
         history_nearest(irregular, 5, 0, 5, 8.15) == 3 &&
         history_nearest(irregular, 5, 0, 5, .5) == -1 &&
         history_nearest(irregular, 5, 0, 5, NAN) == -1;
    ok = ok && history_next_window(60) == 300 && history_next_window(300) == 900 &&
         history_next_window(900) == 60;
    ok = ok && panel_sidebar_width(900, 500) == 0 && panel_sidebar_width(1440, 100) == 220 &&
         panel_sidebar_width(1000, 900) == 520;
    printf("%s: wrapped/irregular history lookup, blank ranges, visible scaling and panel bounds\n",
           ok ? "PASS" : "FAIL");
    return ok;
}
