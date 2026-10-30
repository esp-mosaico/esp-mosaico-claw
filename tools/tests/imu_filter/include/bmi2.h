#pragma once
#include <stdint.h>
struct bmi2_sens_axes_data { int16_t x, y, z; };
struct bmi2_sens_data {
    struct bmi2_sens_axes_data acc;
    struct bmi2_sens_axes_data gyr;
};
