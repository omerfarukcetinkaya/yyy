/**
 * @file gas_analyzer.h
 * @brief Multi-sensor gas classification for fire/hazard detection.
 *
 * Classifies the environment using:
 *   - co_ppm    : CO from MQ7
 *   - nh3_ppm   : NH3/VOC from MQ137
 *   - temp_c    : temperature from AHT20
 *   - lux       : ambient light from VEML7700
 *
 * Classification categories (ordered by severity):
 *   NORMAL        - all readings within safe limits
 *   POOR_AIR      - slightly elevated gas levels
 *   DANGEROUS     - CO or NH3 above safe threshold
 *   CABLE_BURNING - high VOC/NH3 + moderate CO (PVC/insulation burning)
 *   FIRE          - high CO + high temp, or strong light burst
 *   TOXIC_GAS     - very high NH3 / multiple sensors critical
 */
#pragma once
#include <stdint.h>

typedef enum {
    GAS_STATUS_NORMAL        = 0,
    GAS_STATUS_POOR_AIR      = 1,
    GAS_STATUS_DANGEROUS     = 2,
    GAS_STATUS_CABLE_BURNING = 3,
    GAS_STATUS_FIRE          = 4,
    GAS_STATUS_TOXIC_GAS     = 5,
} gas_status_t;

typedef struct {
    float co_ppm;
    float nh3_ppm;
    float temp_c;
    float lux;
} gas_inputs_t;

/**
 * @brief Classify the environment from sensor readings.
 * @param in   Sensor snapshot.
 * @return     gas_status_t (highest severity that matches).
 */
gas_status_t gas_analyzer_classify(const gas_inputs_t *in);

/**
 * @brief Return a short human-readable string for a gas_status_t value.
 */
const char *gas_analyzer_status_str(gas_status_t status);
