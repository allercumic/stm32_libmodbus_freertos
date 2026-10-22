#ifndef KTH2R_H
#define KTH2R_H

#include <stdint.h>

typedef struct
{
    int16_t temperature_x10;  /* 温度，单位 0.1°C */
    uint16_t humidity_x10;    /* 湿度，单位 0.1%RH */
} KTH2R_Data_t;

int KTH2R_Init(void);
int KTH2R_Read(KTH2R_Data_t *data);
void KTH2R_DeInit(void);

#endif