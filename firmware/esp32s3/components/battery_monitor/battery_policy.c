#include "battery_policy.h"

#include <stddef.h>

typedef struct {
    uint16_t millivolts;
    uint8_t percent;
} curve_point_t;

/* Typical 1S LiPo open-circuit curve, descending voltage. */
static const curve_point_t lipo_curve[] = {
    {4200U, 100U}, {4150U, 95U}, {4110U, 90U}, {4080U, 85U},
    {4020U, 80U},  {3980U, 75U}, {3950U, 70U}, {3910U, 65U},
    {3870U, 60U},  {3850U, 55U}, {3840U, 50U}, {3820U, 45U},
    {3800U, 40U},  {3790U, 35U}, {3770U, 30U}, {3750U, 25U},
    {3730U, 20U},  {3710U, 15U}, {3690U, 10U}, {3610U, 5U},
    {3270U, 0U},
};

float battery_percent_from_mv(uint32_t millivolts)
{
    const size_t count = sizeof(lipo_curve) / sizeof(lipo_curve[0]);
    if (millivolts >= lipo_curve[0].millivolts) {
        return 100.0f;
    }
    if (millivolts <= lipo_curve[count - 1U].millivolts) {
        return 0.0f;
    }
    for (size_t index = 1U; index < count; index++) {
        const curve_point_t upper = lipo_curve[index - 1U];
        const curve_point_t lower = lipo_curve[index];
        if (millivolts >= lower.millivolts) {
            const float span_mv =
                (float)(upper.millivolts - lower.millivolts);
            const float span_pct = (float)(upper.percent - lower.percent);
            return (float)lower.percent +
                   ((float)(millivolts - lower.millivolts) * span_pct) /
                       span_mv;
        }
    }
    return 0.0f;
}

battery_level_t battery_classify(const battery_policy_t *policy,
                                 bool valid,
                                 float percent,
                                 battery_level_t previous)
{
    if (policy == NULL || !valid) {
        return BATTERY_LEVEL_UNKNOWN;
    }
    const float critical_exit =
        (float)policy->critical_pct + (float)policy->hysteresis_pct;
    const float low_exit =
        (float)policy->low_pct + (float)policy->hysteresis_pct;

    if (percent < (float)policy->critical_pct ||
        (previous == BATTERY_LEVEL_CRITICAL && percent < critical_exit)) {
        return BATTERY_LEVEL_CRITICAL;
    }
    if (percent < (float)policy->low_pct ||
        ((previous == BATTERY_LEVEL_LOW ||
          previous == BATTERY_LEVEL_CRITICAL) &&
         percent < low_exit)) {
        return BATTERY_LEVEL_LOW;
    }
    return BATTERY_LEVEL_NORMAL;
}

const char *battery_level_name(battery_level_t level)
{
    switch (level) {
    case BATTERY_LEVEL_NORMAL:
        return "normal";
    case BATTERY_LEVEL_LOW:
        return "low";
    case BATTERY_LEVEL_CRITICAL:
        return "critical";
    case BATTERY_LEVEL_UNKNOWN:
    default:
        return "unknown";
    }
}
