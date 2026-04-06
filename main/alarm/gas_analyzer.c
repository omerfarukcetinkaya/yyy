/**
 * @file gas_analyzer.c
 * @brief Multi-sensor gas classification logic.
 *
 * Thresholds are chosen from sensor datasheets and occupational exposure limits:
 *   CO  : OSHA PEL 50 ppm TWA, IDLH 1200 ppm
 *   NH3 : OSHA PEL 50 ppm TWA, IDLH 300 ppm
 *   Temp: >70°C considered a fire risk indicator
 *   Lux : >8000 lux suggests a flame nearby in an indoor environment
 *
 * The evaluation uses the highest-severity rule that matches all conditions.
 * Rules are checked from most to least severe — first match wins.
 */
#include "gas_analyzer.h"

/* ── Thresholds ───────────────────────────────────────────────────────────── */

/* TOXIC_GAS: NH3 critically high (above IDLH) or extreme CO */
#define THR_NH3_TOXIC          250.0f   /* ppm */
#define THR_CO_TOXIC           500.0f   /* ppm */

/* FIRE: CO + temperature, or sudden bright flash */
#define THR_CO_FIRE             80.0f   /* ppm */
#define THR_TEMP_FIRE           65.0f   /* °C  */
#define THR_LUX_FIRE          8000.0f   /* lux */

/* CABLE_BURNING: VOC/NH3 spike with low-moderate CO (smouldering insulation) */
#define THR_NH3_CABLE           60.0f   /* ppm */
#define THR_CO_CABLE_MAX        50.0f   /* ppm, NOT a full fire yet */

/* DANGEROUS: CO or NH3 above OSHA PEL TWA */
#define THR_CO_DANGEROUS        50.0f   /* ppm */
#define THR_NH3_DANGEROUS       50.0f   /* ppm */

/* POOR_AIR: slightly elevated */
#define THR_CO_POOR             20.0f   /* ppm */
#define THR_NH3_POOR            25.0f   /* ppm */

/* ── Classification ───────────────────────────────────────────────────────── */

gas_status_t gas_analyzer_classify(const gas_inputs_t *in)
{
    if (!in) return GAS_STATUS_NORMAL;

    /* TOXIC_GAS — NH3 above IDLH or CO extreme */
    if (in->nh3_ppm >= THR_NH3_TOXIC || in->co_ppm >= THR_CO_TOXIC) {
        return GAS_STATUS_TOXIC_GAS;
    }

    /* FIRE — high CO + high temp, OR strong light burst */
    if ((in->co_ppm >= THR_CO_FIRE && in->temp_c >= THR_TEMP_FIRE)
        || in->lux >= THR_LUX_FIRE) {
        return GAS_STATUS_FIRE;
    }

    /* CABLE_BURNING — high VOC/NH3 with low-moderate CO (smouldering) */
    if (in->nh3_ppm >= THR_NH3_CABLE && in->co_ppm < THR_CO_CABLE_MAX) {
        return GAS_STATUS_CABLE_BURNING;
    }

    /* DANGEROUS — CO or NH3 above OSHA PEL */
    if (in->co_ppm >= THR_CO_DANGEROUS || in->nh3_ppm >= THR_NH3_DANGEROUS) {
        return GAS_STATUS_DANGEROUS;
    }

    /* POOR_AIR — slightly elevated */
    if (in->co_ppm >= THR_CO_POOR || in->nh3_ppm >= THR_NH3_POOR) {
        return GAS_STATUS_POOR_AIR;
    }

    return GAS_STATUS_NORMAL;
}

const char *gas_analyzer_status_str(gas_status_t status)
{
    switch (status) {
        case GAS_STATUS_NORMAL:        return "NORMAL";
        case GAS_STATUS_POOR_AIR:      return "POOR_AIR";
        case GAS_STATUS_DANGEROUS:     return "DANGEROUS";
        case GAS_STATUS_CABLE_BURNING: return "CABLE_BURN";
        case GAS_STATUS_FIRE:          return "FIRE";
        case GAS_STATUS_TOXIC_GAS:     return "TOXIC";
        default:                       return "UNKNOWN";
    }
}
