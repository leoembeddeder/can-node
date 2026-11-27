/* SPDX-License-Identifier: MIT */
#include "signals.h"

#include "can_db.h"

/*
 * What the conversions below assume about the DBC, one relation per assertion (the pattern
 * app/gen/can_db.h recommends). The arithmetic is written for exactly these factors and
 * offsets; a DBC that says otherwise stops the build here.
 */

/* PackCurrent: physical = raw * 1/100 A, no offset, a signed 16-bit raw value. 1 raw unit is
 * 10 mA. */
#define MA_PER_CURRENT_BIT 10
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_FACTOR_NUM == 1, "PackCurrent: 0.01 A/bit");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_FACTOR_DEN == 100, "PackCurrent: 0.01 A/bit");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_OFFSET_NUM == 0, "PackCurrent: no offset");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MIN == INT16_MIN, "PackCurrent: int16_t");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MAX == INT16_MAX, "PackCurrent: int16_t");
_Static_assert((BMS_CURRENT_SENSOR_MAX_MA / MA_PER_CURRENT_BIT) <= INT16_MAX,
               "PackCurrent: every plausible current fits without saturation");

/* PackVoltage: physical = raw * 1/1000 V, no offset: 1 raw unit is 1 mV. */
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_FACTOR_NUM == 1, "PackVoltage: 1 mV/bit");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_FACTOR_DEN == 1000, "PackVoltage: 1 mV/bit");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_OFFSET_NUM == 0, "PackVoltage: no offset");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_RAW_MAX == UINT16_MAX, "PackVoltage: 16 bits");
_Static_assert(BMS_PACK_SENSOR_MAX_MV <= CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_RAW_MAX,
               "PackVoltage: every plausible pack voltage fits without saturation");

/* PackState: the value table has the numbers of bms_state_t. */
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_STATE_STARTUP == (uint32_t)BMS_STATE_STARTUP,
               "PackState: STARTUP");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_STATE_NORMAL == (uint32_t)BMS_STATE_NORMAL,
               "PackState: NORMAL");
_Static_assert(CAN_DB_BMS_PACK_STATUS_PACK_STATE_FAULT == (uint32_t)BMS_STATE_FAULT,
               "PackState: FAULT");

/* The eight CellNVoltage signals: physical = raw * 1/1000 V + 1 V, raw 0..4095, 4095 = SNA.
 * 1 raw unit is 1 mV, raw 0 is 1000 mV, and the largest value that is not SNA is 4094. */
#define CELL_OFFSET_MV   1000U
#define CELL_RAW_LARGEST 4094U
_Static_assert(CELL_RAW_LARGEST == (SIGNALS_CELL_SNA - 1U), "CellNVoltage: one below SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_NUM == 1, "Cell1: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_DEN == 1000, "Cell1: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_NUM == 1, "Cell1: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_DEN == 1, "Cell1: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell1: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell1: SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_FACTOR_NUM == 1, "Cell2: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_FACTOR_DEN == 1000, "Cell2: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_OFFSET_NUM == 1, "Cell2: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_OFFSET_DEN == 1, "Cell2: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell2: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL2_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell2: SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_FACTOR_NUM == 1, "Cell3: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_FACTOR_DEN == 1000, "Cell3: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_OFFSET_NUM == 1, "Cell3: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_OFFSET_DEN == 1, "Cell3: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell3: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL3_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell3: SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_FACTOR_NUM == 1, "Cell4: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_FACTOR_DEN == 1000, "Cell4: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_OFFSET_NUM == 1, "Cell4: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_OFFSET_DEN == 1, "Cell4: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell4: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL4_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell4: SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_FACTOR_NUM == 1, "Cell5: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_FACTOR_DEN == 1000, "Cell5: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_OFFSET_NUM == 1, "Cell5: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_OFFSET_DEN == 1, "Cell5: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell5: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL5_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell5: SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_FACTOR_NUM == 1, "Cell6: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_FACTOR_DEN == 1000, "Cell6: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_OFFSET_NUM == 1, "Cell6: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_OFFSET_DEN == 1, "Cell6: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell6: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL6_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell6: SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_FACTOR_NUM == 1, "Cell7: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_FACTOR_DEN == 1000, "Cell7: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_OFFSET_NUM == 1, "Cell7: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_OFFSET_DEN == 1, "Cell7: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell7: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL7_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell7: SNA");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_FACTOR_NUM == 1, "Cell8: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_FACTOR_DEN == 1000, "Cell8: 1 mV/bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_OFFSET_NUM == 1, "Cell8: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_OFFSET_DEN == 1, "Cell8: from 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_RAW_MAX == SIGNALS_CELL_SNA, "Cell8: 12 b");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES2_CELL8_VOLTAGE_SNA == SIGNALS_CELL_SNA, "Cell8: SNA");
_Static_assert(BMS_CELL_SENSOR_MAX_MV <= (CELL_OFFSET_MV + CELL_RAW_LARGEST),
               "CellNVoltage: every plausible cell above 1 V fits without saturation");

/* The four TempN signals: physical = raw * 1 degC - 40 degC, raw 0..255, 255 = SNA. A raw
 * unit is 10 tenths of a degree; raw 0 is -40 degC, and the largest value that is not SNA is
 * 254 (214 degC). */
#define DC_PER_TEMPERATURE_BIT  10
#define TEMPERATURE_RAW_OF_0C   40
#define TEMPERATURE_RAW_LARGEST 254U
_Static_assert(TEMPERATURE_RAW_LARGEST == (SIGNALS_TEMPERATURE_SNA - 1U), "TempN: one below SNA");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_FACTOR_NUM == 1, "Temp1: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_FACTOR_DEN == 1, "Temp1: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_OFFSET_NUM == -40, "Temp1: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_OFFSET_DEN == 1, "Temp1: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_RAW_MAX == SIGNALS_TEMPERATURE_SNA, "Temp1: 8 bits");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP1_SNA == SIGNALS_TEMPERATURE_SNA, "Temp1: SNA");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP2_FACTOR_NUM == 1, "Temp2: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP2_FACTOR_DEN == 1, "Temp2: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP2_OFFSET_NUM == -40, "Temp2: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP2_OFFSET_DEN == 1, "Temp2: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP2_RAW_MAX == SIGNALS_TEMPERATURE_SNA, "Temp2: 8 bits");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP2_SNA == SIGNALS_TEMPERATURE_SNA, "Temp2: SNA");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP3_FACTOR_NUM == 1, "Temp3: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP3_FACTOR_DEN == 1, "Temp3: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP3_OFFSET_NUM == -40, "Temp3: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP3_OFFSET_DEN == 1, "Temp3: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP3_RAW_MAX == SIGNALS_TEMPERATURE_SNA, "Temp3: 8 bits");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP3_SNA == SIGNALS_TEMPERATURE_SNA, "Temp3: SNA");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_FACTOR_NUM == 1, "Temp4: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_FACTOR_DEN == 1, "Temp4: 1 degC/bit");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_OFFSET_NUM == -40, "Temp4: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_OFFSET_DEN == 1, "Temp4: from -40 degC");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_RAW_MAX == SIGNALS_TEMPERATURE_SNA, "Temp4: 8 bits");
_Static_assert(CAN_DB_BMS_TEMPERATURES_TEMP4_SNA == SIGNALS_TEMPERATURE_SNA, "Temp4: SNA");
_Static_assert((BMS_TEMP_SENSOR_MIN_DC / DC_PER_TEMPERATURE_BIT) >= -TEMPERATURE_RAW_OF_0C,
               "TempN: every plausible temperature fits without saturation (minimum)");
_Static_assert(((BMS_TEMP_SENSOR_MAX_DC / DC_PER_TEMPERATURE_BIT) + TEMPERATURE_RAW_OF_0C) <=
                   (int32_t)TEMPERATURE_RAW_LARGEST,
               "TempN: every plausible temperature fits without saturation (maximum)");

/* TaskCpuShare: physical = raw * 1/100 %, so 100 % is raw 10000, which the signal holds. */
#define CPU_SHARE_FULL 10000U
_Static_assert(CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_FACTOR_NUM == 1, "TaskCpuShare: 0.01 %/bit");
_Static_assert(CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_FACTOR_DEN == 100, "TaskCpuShare: 0.01 %/bit");
_Static_assert(CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_OFFSET_NUM == 0, "TaskCpuShare: no offset");
_Static_assert(CPU_SHARE_FULL <= CAN_DB_BMS_TASK_STATS_TASK_CPU_SHARE_RAW_MAX,
               "TaskCpuShare: 100 % fits");

/* round(value / divisor) for a divisor above 0, a half away from zero. In 64 bits, so that no
 * value of a 32-bit argument can overflow on the way. */
static int64_t divide_rounded(int64_t value, int64_t divisor)
{
    const int64_t half = divisor / 2;
    int64_t result;

    if (value >= 0) {
        result = (value + half) / divisor;
    } else {
        result = -((half - value) / divisor);
    }

    return result;
}

int16_t signals_pack_current(int32_t current_ma)
{
    const int64_t raw = divide_rounded((int64_t)current_ma, MA_PER_CURRENT_BIT);
    int16_t result;

    if (raw > (int64_t)CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MAX) {
        result = CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MAX;
    } else if (raw < (int64_t)CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MIN) {
        result = CAN_DB_BMS_PACK_STATUS_PACK_CURRENT_RAW_MIN;
    } else {
        result = (int16_t)raw;
    }

    return result;
}

uint16_t signals_pack_voltage(uint32_t pack_mv)
{
    return (uint16_t)signals_saturate(pack_mv, CAN_DB_BMS_PACK_STATUS_PACK_VOLTAGE_RAW_MAX);
}

uint16_t signals_cell_voltage(uint16_t cell_mv, bool valid)
{
    uint32_t raw = SIGNALS_CELL_SNA;

    if (valid) {
        /* Below the offset the signal has nothing: the lowest value it can carry stands in. */
        const uint32_t above_offset =
            ((uint32_t)cell_mv > CELL_OFFSET_MV) ? ((uint32_t)cell_mv - CELL_OFFSET_MV) : 0U;

        raw = signals_saturate(above_offset, CELL_RAW_LARGEST);
    }

    return (uint16_t)raw;
}

uint8_t signals_temperature(int16_t temp_dc, bool valid)
{
    uint32_t raw = SIGNALS_TEMPERATURE_SNA;

    if (valid) {
        const int64_t shifted =
            divide_rounded((int64_t)temp_dc, DC_PER_TEMPERATURE_BIT) + TEMPERATURE_RAW_OF_0C;

        if (shifted < 0) {
            raw = 0U;
        } else if (shifted > (int64_t)TEMPERATURE_RAW_LARGEST) {
            raw = TEMPERATURE_RAW_LARGEST;
        } else {
            raw = (uint32_t)shifted;
        }
    }

    return (uint8_t)raw;
}

uint8_t signals_pack_state(bms_state_t state)
{
    uint8_t raw;

    switch (state) {
    case BMS_STATE_STARTUP:
        raw = (uint8_t)CAN_DB_BMS_PACK_STATUS_PACK_STATE_STARTUP;
        break;
    case BMS_STATE_NORMAL:
        raw = (uint8_t)CAN_DB_BMS_PACK_STATUS_PACK_STATE_NORMAL;
        break;
    default:
        raw = (uint8_t)CAN_DB_BMS_PACK_STATUS_PACK_STATE_FAULT;
        break;
    }

    return raw;
}

uint16_t signals_cpu_share(uint64_t task_us, uint64_t total_us)
{
    uint64_t share = 0U;

    if (task_us >= total_us) {
        /* All of the time, or none while there is no time yet. */
        if (total_us != 0U) {
            share = CPU_SHARE_FULL;
        }
    } else {
        uint64_t part = task_us;
        uint64_t whole = total_us;

        /* part * 10000 + whole / 2 has to fit 64 bits. That needs a run of 58 years; should it
         * ever come, both are halved until it fits, which changes the share by less than the
         * rounding does. */
        while (part > ((UINT64_MAX - (whole / 2U)) / CPU_SHARE_FULL)) {
            part /= 2U;
            whole /= 2U;
        }

        share = ((part * CPU_SHARE_FULL) + (whole / 2U)) / whole;
    }

    return (uint16_t)share;
}

uint32_t signals_saturate(uint32_t value, uint32_t max)
{
    return (value > max) ? max : value;
}
