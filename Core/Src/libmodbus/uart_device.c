#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "semphr.h"
#include "queue.h"

#include <stdio.h>
#include <string.h>

#include "usart.h"
#include "uart_device.h"

#define UART_RECV_BUF      128

/*
 * 硬件绑定配置
 *
 * 以后从UART2切换到UART3，只需要改成：
 * #define MODBUS_UART_HANDLE huart3
 *
 * 同时确保CubeMX已经生成UART3的初始化和中断代码。
 */
#define MODBUS_UART_HANDLE huart2
#define MODBUS_DEVICE_NAME "modbus"

typedef struct UART_Data {
    UART_HandleTypeDef *huart;

    uint8_t recv_char;

    /* UART发送完成通知 */
    SemaphoreHandle_t xSendComplete;
	/* UART发送锁 */
	SemaphoreHandle_t xSendMutex;     

    /* UART接收字节队列 */
    QueueHandle_t xRecvQueue;
} UART_Data;


/* 函数声明 */
static int uart_init(
    struct UART_Device *pDev,
    int baud,
    char parity,
    int data_bit,
    int stop_bit);

static int uart_send(
    struct UART_Device *pDev,
    uint8_t *datas,
    uint32_t len,
    int timeout);

static int uart_recv(
    struct UART_Device *pDev,
    uint8_t *data,
    int timeout);

static int uart_flush(struct UART_Device *pDev);


/*
 * Modbus串口的底层数据。
 */
static UART_Data g_modbus_uart_data = {
    .huart = &MODBUS_UART_HANDLE,
    .recv_char = 0,
    .xSendComplete = NULL,
    .xRecvQueue = NULL,
};


/* Modbus UART设备 */
static struct UART_Device g_modbus_uart_device = {
    .name = MODBUS_DEVICE_NAME,
    .Init = uart_init,
    .Send = uart_send,
    .RecvByte = uart_recv,
    .Flush = uart_flush,
    .priv_data = &g_modbus_uart_data,
};


/*
 * UART设备表。
 *
 * 以后增加其他UART设备时，只需要继续往这里添加。
 */
static struct UART_Device *g_uart_devices[] = {
    &g_modbus_uart_device,
};

#define UART_DEVICE_COUNT \
    (sizeof(g_uart_devices) / sizeof(g_uart_devices[0]))


/*
 * 根据HAL传进来的UART句柄查找UART_Data。
 *
 * HAL的回调只有UART_HandleTypeDef参数，因此需要通过设备表反查
 * 它属于哪个UART_Device。
 */
static UART_Data *uart_find_data_by_handle(
    UART_HandleTypeDef *huart)
{
    size_t i;

    if (huart == NULL) {
        return NULL;
    }

    for (i = 0; i < UART_DEVICE_COUNT; i++) {
        struct UART_Device *device;
        UART_Data *data;

        device = g_uart_devices[i];

        if (device == NULL) {
            continue;
        }

        data = (UART_Data *)device->priv_data;

        if ((data != NULL) && (data->huart == huart)) {
            return data;
        }
    }

    return NULL;
}


static int uart_init(
    struct UART_Device *pDev,
    int baud,
    char parity,
    int data_bit,
    int stop_bit)
{
    UART_Data *pData;

    /*
     * 当前HAL串口参数还是由CubeMX生成的usart.c配置。
     * 这些参数暂时没有使用。
     */
    (void)baud;
    (void)parity;
    (void)data_bit;
    (void)stop_bit;

    if (pDev == NULL) {
        return -1;
    }

    pData = (UART_Data *)pDev->priv_data;

    if ((pData == NULL) || (pData->huart == NULL)) {
        return -1;
    }

	if (pData->xSendMutex == NULL) {
		pData->xSendMutex = xSemaphoreCreateMutex();

		if (pData->xSendMutex == NULL) {
			return -1;
		}
	}

	if (pData->xSendComplete == NULL) {
		pData->xSendComplete = xSemaphoreCreateBinary();

		if (pData->xSendComplete == NULL) {
			return -1;
		}
	}

    if (pData->xRecvQueue == NULL) {
        pData->xRecvQueue = xQueueCreate(
            UART_RECV_BUF,
            sizeof(uint8_t));

        if (pData->xRecvQueue == NULL) {
            return -1;
        }
    }

    /* 启动第一个字节的中断接收 */
    if (HAL_UART_Receive_IT(
            pData->huart,
            &pData->recv_char,
            1) != HAL_OK) {
        return -1;
    }

    return 0;
}


static int uart_send(
    struct UART_Device *pDev,
    uint8_t *datas,
    uint32_t len,
    int timeout)
{
    UART_Data *pData;
    int result = -1;

    if ((pDev == NULL) ||
        (datas == NULL) ||
        (len == 0)) {
        return -1;
    }

    pData = (UART_Data *)pDev->priv_data;

    if ((pData == NULL) ||
        (pData->huart == NULL) ||
        (pData->xSendMutex == NULL) ||
        (pData->xSendComplete == NULL)) {
        return -1;
    }

    if (xSemaphoreTake(
            pData->xSendMutex,
            timeout) != pdTRUE) {
        return -1;
    }

    /* 清除旧的完成通知 */
    (void)xSemaphoreTake(
        pData->xSendComplete,
        0);

    if (HAL_UART_Transmit_IT(
            pData->huart,
            datas,
            len) != HAL_OK) {
        goto exit;
    }

    if (xSemaphoreTake(
            pData->xSendComplete,
            timeout) != pdTRUE) {

        /*
         * 等待超时后中止硬件发送，防止旧发送的回调
         * 干扰下一次发送。
         */
        (void)HAL_UART_AbortTransmit(pData->huart);

        /* 清除可能残留的完成通知 */
        (void)xSemaphoreTake(
            pData->xSendComplete,
            0);

        goto exit;
    }

    result = 0;

exit:
    xSemaphoreGive(pData->xSendMutex);
    return result;
}


/*
 * 所有UART共用这个HAL发送完成回调。
 *
 * 不再判断：
 *     if (huart == &huart2)
 *
 * 而是根据huart自动查找设备。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    UART_Data *pData;
    BaseType_t task_woken = pdFALSE;

    pData = uart_find_data_by_handle(huart);

    if (pData == NULL) {
        return;
    }

    xSemaphoreGiveFromISR(
        pData->xSendComplete,
        &task_woken);

    portYIELD_FROM_ISR(task_woken);
}


static int uart_recv(struct UART_Device *pDev,uint8_t *data,int timeout)
{
    UART_Data *pData;

    if ((pDev == NULL) || (data == NULL)) {
        return -1;
    }

    pData = (UART_Data *)pDev->priv_data;

    if ((pData == NULL) ||
        (pData->xRecvQueue == NULL)) {
        return -1;
    }

    if (xQueueReceive(
            pData->xRecvQueue,
            data,
            timeout) == pdPASS) {
        return 0;
    }

    return -1;
}


/*
 * 所有UART共用这个HAL接收完成回调。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    UART_Data *pData;
    BaseType_t task_woken = pdFALSE;

    pData = uart_find_data_by_handle(huart);

    if ((pData == NULL) ||
        (pData->xRecvQueue == NULL)) {
        return;
    }

    /* 把接收到的字节放入对应设备的接收队列 */
    (void)xQueueSendFromISR(
        pData->xRecvQueue,
        &pData->recv_char,
        &task_woken);

    /* 重新启动下一个字节的中断接收 */
    (void)HAL_UART_Receive_IT(
        pData->huart,
        &pData->recv_char,
        1);

    portYIELD_FROM_ISR(task_woken);
}


static int uart_flush(struct UART_Device *pDev)
{
    UART_Data *pData;
    uint8_t data;
    int count = 0;

    if (pDev == NULL) {
        return -1;
    }

    pData = (UART_Data *)pDev->priv_data;

    if ((pData == NULL) ||
        (pData->xRecvQueue == NULL)) {
        return -1;
    }

    /* 清空接收队列 */
    while (xQueueReceive(
               pData->xRecvQueue,
               &data,
               0) == pdPASS) {
        count++;
    }

    return count;
}


struct UART_Device *GetUARTDevice(char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; i < UART_DEVICE_COUNT; i++) {
        if ((g_uart_devices[i] != NULL) &&
            (strcmp(name, g_uart_devices[i]->name) == 0)) {
            return g_uart_devices[i];
        }
    }

    return NULL;
}