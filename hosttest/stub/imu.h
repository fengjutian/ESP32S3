#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t imu_read_accel(float *ax, float *ay, float *az);
