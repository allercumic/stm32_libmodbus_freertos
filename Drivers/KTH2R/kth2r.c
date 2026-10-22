#include "kth2r.h"
#include "modbus.h"
#include <stddef.h>

#define KTH2R_DEVICE	   "modbus"
#define KTH2R_BAUDRATE     9600
#define KTH2R_SLAVE_ID     1
#define KTH2R_DATA_REG     0x0300
#define KTH2R_REG_COUNT    2

static modbus_t *kth2r_ctx = NULL;

int KTH2R_Init(void)
{
    kth2r_ctx = modbus_new_st_rtu(
        KTH2R_DEVICE,
        KTH2R_BAUDRATE,
        'N',
        8,
        1
    );

    if (kth2r_ctx == NULL) {
        return -1;
    }

    if (modbus_set_slave(kth2r_ctx, KTH2R_SLAVE_ID) == -1) {
        KTH2R_DeInit();
        return -1;
    }

    if (modbus_connect(kth2r_ctx) == -1) {
        KTH2R_DeInit();
        return -1;
    }

    return 0;
}

int KTH2R_Read(KTH2R_Data_t *data)
{
    uint16_t reg[KTH2R_REG_COUNT];

    if (kth2r_ctx == NULL || data == NULL) {
        return -1;
    }

    if (modbus_read_registers(
            kth2r_ctx,
            KTH2R_DATA_REG,
            KTH2R_REG_COUNT,
            reg) != KTH2R_REG_COUNT) {
        return -1;
    }

    data->temperature_x10 = (int16_t)reg[0];
    data->humidity_x10 = reg[1];

    return 0;
}

void KTH2R_DeInit(void)
{
    if (kth2r_ctx != NULL) {
        modbus_close(kth2r_ctx);
        modbus_free(kth2r_ctx);
        kth2r_ctx = NULL;
    }
}