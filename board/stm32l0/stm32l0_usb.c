/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#include "stm32l0_usb.h"
#include "stm32.h"
#include "stm32l0_gpio.h"
#include "delay.h"
#include "../../board.h"
#include "../../comm.h"
#include "../../usb.h"
#include "config.h"

#ifndef NULL
#define NULL                                        0
#endif

#define USB_DM                                      A11
#define USB_DP                                      A12

#pragma pack(push, 1)

typedef struct {
    uint16_t ADDR_TX;
    uint16_t COUNT_TX;
    uint16_t ADDR_RX;
    uint16_t COUNT_RX;
} USB_BUFFER_DESCRIPTOR;

#define USB_BUFFER_DESCRIPTORS                      ((USB_BUFFER_DESCRIPTOR*) USB_PMAADDR)

#pragma pack(pop)

static inline uint16_t* ep_reg_data(int num)
{
    return (uint16_t*)(USB_BASE + USB_EP_NUM(num) * 4);
}

static inline EP* ep_data(COMM* comm, int num)
{
    return (num & USB_EP_IN) ? &(comm->drv.in[USB_EP_NUM(num)]) : &(comm->drv.out[USB_EP_NUM(num)]);
}

static inline void ep_toggle_bits(int num, int mask, int value)
{
    __disable_irq();
    *ep_reg_data(num) = ((*ep_reg_data(num)) & USB_EPREG_MASK) | (((*ep_reg_data(num)) & mask) ^ value);
    __enable_irq();
}

static inline void memcpy16(void* dst, void* src, unsigned int size)
{
    int i;
    size = (size + 1) / 2;
    for (i = 0; i < size; ++i)
        ((uint16_t*)dst)[i] = ((uint16_t*)src)[i];
}

/*
#include "../sys.h"
#include "../usb.h"
#include "../../userspace/direct.h"
#include "../../userspace/irq.h"
#include "../../userspace/block.h"
#include "stm32_gpio.h"
#include "stm32_power.h"
#include <string.h>
#include "../../userspace/lib/stdlib.h"
#if (SYS_INFO) || (USB_DEBUG_ERRORS)
#include "../../userspace/lib/stdio.h"
#endif
#if (MONOLITH_USB)
#include "stm32_core_private.h"
#endif


static inline void stm32_usb_tx(SHARED_USB_DRV* drv, int num)
{
    EP* ep = drv->usb.in[USB_EP_NUM(num)];

    int size = ep->size - ep->processed;
    if (size > ep->mps)
        size = ep->mps;
    USB_BUFFER_DESCRIPTORS[num].COUNT_TX = size;
    memcpy16((void*)(USB_BUFFER_DESCRIPTORS[num].ADDR_TX + USB_PMAADDR), ep->ptr +  ep->processed, size);
    ep_toggle_bits(num, USB_EPTX_STAT, USB_EP_TX_VALID);
    ep->processed += size;
}

bool stm32_usb_ep_flush(SHARED_USB_DRV* drv, int num)
{
    if (USB_EP_NUM(num) >= USB_EP_COUNT_MAX)
    {
        error(ERROR_INVALID_PARAMS);
        return false;
    }
    EP* ep = ep_data(drv, num);
    if (ep == NULL)
    {
        error(ERROR_NOT_CONFIGURED);
        return false;
    }
    if (ep->block != INVALID_HANDLE)
    {
        if (num & USB_EP_IN)
            ep_toggle_bits(num, USB_EPTX_STAT, USB_EP_TX_NAK);
        else
            ep_toggle_bits(num, USB_EPRX_STAT, USB_EP_RX_NAK);
        block_send(ep->block, ep->process);
        ep->block = INVALID_HANDLE;
    }
    ep->io_active = false;
    return true;
}

void stm32_usb_ep_set_stall(SHARED_USB_DRV* drv, int num)
{
    if (!stm32_usb_ep_flush(drv, num))
        return;
    if (USB_EP_IN & num)
        ep_toggle_bits(num, USB_EPTX_STAT, USB_EP_TX_STALL);
    else
        ep_toggle_bits(num, USB_EPRX_STAT, USB_EP_RX_STALL);
}

void stm32_usb_ep_clear_stall(SHARED_USB_DRV* drv, int num)
{
    if (!stm32_usb_ep_flush(drv, num))
        return;
    if (USB_EP_IN & num)
        ep_toggle_bits(num, USB_EPTX_STAT, USB_EP_TX_NAK);
    else
        ep_toggle_bits(num, USB_EPRX_STAT, USB_EP_RX_NAK);
}


bool stm32_usb_ep_is_stall(int num)
{
    if (USB_EP_NUM(num) >= USB_EP_COUNT_MAX)
    {
        error(ERROR_INVALID_PARAMS);
        return false;
    }
    if (USB_EP_IN & num)
        return ((*ep_reg_data(num)) & USB_EPTX_STAT) == USB_EP_TX_STALL;
    else
        return ((*ep_reg_data(num)) & USB_EPRX_STAT) == USB_EP_RX_STALL;
}

USB_SPEED stm32_usb_get_speed(SHARED_USB_DRV* drv)
{
    //according to datasheet STM32L0 doesn't support low speed mode...
    return USB_FULL_SPEED;
}

static inline void stm32_usb_reset(SHARED_USB_DRV* drv)
{
    USB->CNTR |= USB_CNTR_SUSPM;

    //enable function
    USB->DADDR = USB_DADDR_EF;

    IPC ipc;
    ipc.process = drv->usb.device;
    ipc.param1 = stm32_usb_get_speed(drv);
    ipc.cmd = USB_RESET;
    ipc_ipost(&ipc);
}

static inline void stm32_usb_suspend(SHARED_USB_DRV* drv)
{
    IPC ipc;
    USB->CNTR &= ~USB_CNTR_SUSPM;
    ipc.process = drv->usb.device;
    ipc.cmd = USB_SUSPEND;
    ipc_ipost(&ipc);
}

static inline void stm32_usb_wakeup(SHARED_USB_DRV* drv)
{
    IPC ipc;
    USB->CNTR |= USB_CNTR_SUSPM;
    ipc.process = drv->usb.device;
    ipc.cmd = USB_WAKEUP;
    ipc_ipost(&ipc);
}

static inline void stm32_usb_ctr(SHARED_USB_DRV* drv)
{
    uint8_t num;
    IPC ipc;
    num = USB->ISTR & USB_ISTR_EP_ID;
    uint16_t size;

    //got SETUP
    if (num == 0 && (*ep_reg_data(num)) & USB_EP_SETUP)
    {
        ipc.cmd = USB_SETUP;
        ipc.process = drv->usb.device;
        memcpy16(&ipc.param1, (void*)(USB_BUFFER_DESCRIPTORS[0].ADDR_RX + USB_PMAADDR), 4);
        memcpy16(&ipc.param2, (void*)(USB_BUFFER_DESCRIPTORS[0].ADDR_RX + USB_PMAADDR + 4), 4);
        ipc_ipost(&ipc);
        *ep_reg_data(num) = (*ep_reg_data(num)) & USB_EPREG_MASK & ~USB_EP_CTR_RX;
        return;
    }

    if ((*ep_reg_data(num)) & USB_EP_CTR_RX)
    {
        size = USB_BUFFER_DESCRIPTORS[num].COUNT_RX & 0x3ff;
        memcpy16(drv->usb.out[num]->ptr + drv->usb.out[num]->processed, (void*)(USB_BUFFER_DESCRIPTORS[0].ADDR_RX + USB_PMAADDR), size);
        *ep_reg_data(num) = (*ep_reg_data(num)) & USB_EPREG_MASK & ~USB_EP_CTR_RX;
        drv->usb.out[num]->processed += size;

        if (drv->usb.out[num]->processed >= drv->usb.out[num]->size)
        {
            ipc.process = drv->usb.out[num]->process;
            ipc.cmd = IPC_READ_COMPLETE;
            ipc.param1 = HAL_HANDLE(HAL_USB, num);
            ipc.param2 = drv->usb.out[num]->block;
            ipc.param3 = drv->usb.out[num]->processed;

            if (drv->usb.out[num]->block != INVALID_HANDLE)
            {
                block_isend_ipc(drv->usb.out[num]->block, drv->usb.out[num]->process, &ipc);
                drv->usb.out[num]->block = INVALID_HANDLE;
            }
            else
                ipc_ipost(&ipc);
            drv->usb.out[num]->io_active = false;
        }
        else
            ep_toggle_bits(num, USB_EPRX_STAT, USB_EP_RX_VALID);
        return;

    }

    if ((*ep_reg_data(num)) & USB_EP_CTR_TX)
    {
        //handle STATUS in for set address
        if (drv->usb.addr && USB_BUFFER_DESCRIPTORS[num].COUNT_TX == 0)
        {
            USB->DADDR = USB_DADDR_EF | drv->usb.addr;
            drv->usb.addr = 0;
        }

        if (drv->usb.in[num]->processed >= drv->usb.in[num]->size)
        {
            ipc.process = drv->usb.in[num]->process;
            ipc.cmd = IPC_WRITE_COMPLETE;
            ipc.param1 = HAL_HANDLE(HAL_USB, USB_EP_IN | num);
            ipc.param2 = drv->usb.in[num]->block;
            ipc.param3 = drv->usb.in[num]->processed;
            if (drv->usb.in[num]->block != INVALID_HANDLE)
            {
                block_isend_ipc(drv->usb.in[num]->block, drv->usb.in[num]->process, &ipc);
                drv->usb.in[num]->block = INVALID_HANDLE;
            }
            else
                ipc_ipost(&ipc);
            drv->usb.in[num]->io_active = false;
        }
        else
        {
            ipc.process = process_iget_current();
            ipc.cmd = STM32_USB_FIFO_TX;
            ipc.param1 = HAL_HANDLE(HAL_USB, USB_EP_IN | num);
            ipc_ipost(&ipc);
        }
        *ep_reg_data(num) = (*ep_reg_data(num)) & USB_EPREG_MASK & ~USB_EP_CTR_TX;
    }
}

static inline void stm32_usb_open_ep(SHARED_USB_DRV* drv, HANDLE process, int num, USB_EP_OPEN* ep_open)
{
    if (USB_EP_NUM(num) >=  USB_EP_COUNT_MAX)
    {
        error(ERROR_INVALID_PARAMS);
        return;
    }
    if (ep_data(drv, num) != NULL)
    {
        error(ERROR_ALREADY_CONFIGURED);
        return;
    }

    //find free addr in FIFO
    unsigned int fifo, i;
    fifo = 0;
    for (i = 0; i < USB_EP_COUNT_MAX; ++i)
    {
        if (drv->usb.in[USB_EP_NUM(num)])
            fifo += drv->usb.in[USB_EP_NUM(num)]->mps;
        if (drv->usb.out[USB_EP_NUM(num)])
            fifo += drv->usb.out[USB_EP_NUM(num)]->mps;
    }
    fifo += sizeof(USB_BUFFER_DESCRIPTOR) * USB_EP_COUNT_MAX;

    EP* ep = malloc(sizeof(EP));
    if (ep == NULL)
        return;
    num & USB_EP_IN ? (drv->usb.in[USB_EP_NUM(num)] = ep) : (drv->usb.out[USB_EP_NUM(num)] = ep);
    ep->block = INVALID_HANDLE;
    ep->mps = 0;
    ep->io_active = false;

    uint16_t ctl = USB_EP_NUM(num);
    //setup ep type
    switch (ep_open->type)
    {
    case USB_EP_CONTROL:
        ctl |= 1 << 9;
        break;
    case USB_EP_BULK:
        ctl |= 0 << 9;
        break;
    case USB_EP_INTERRUPT:
        ctl |= 3 << 9;
        break;
    case USB_EP_ISOCHRON:
        ctl |= 2 << 9;
        break;
    }

    //setup FIFO
    if (num & USB_EP_IN)
    {
        USB_BUFFER_DESCRIPTORS[USB_EP_NUM(num)].ADDR_TX = fifo;
        USB_BUFFER_DESCRIPTORS[USB_EP_NUM(num)].COUNT_TX = 0;
    }
    else
    {
        USB_BUFFER_DESCRIPTORS[USB_EP_NUM(num)].ADDR_RX = fifo;
        if (ep_open->size <= 62)
            USB_BUFFER_DESCRIPTORS[USB_EP_NUM(num)].COUNT_RX = ((ep_open->size + 1) >> 1) << 10;
        else
            USB_BUFFER_DESCRIPTORS[USB_EP_NUM(num)].COUNT_RX = ((((ep_open->size + 3) >> 2) - 1) << 10) | (1 << 15);
    }

    ep->mps = ep_open->size;
    ep->process = process;

    *ep_reg_data(num) = ctl;
    //set NAK, clear DTOG
    if (num & USB_EP_IN)
        ep_toggle_bits(num, USB_EPTX_STAT | USB_EP_DTOG_TX, USB_EP_TX_NAK);
    else
        ep_toggle_bits(num, USB_EPRX_STAT | USB_EP_DTOG_RX, USB_EP_RX_NAK);
}

void stm32_usb_open(SHARED_USB_DRV* drv, unsigned int handle, HANDLE process)
{
    union {
        USB_EP_OPEN ep_open;
        USB_OPEN uo;
    } u;

    if (handle == USB_HANDLE_DEVICE)
    {
        if (direct_read(process, (void*)&u.uo, sizeof(USB_OPEN)))
           stm32_usb_open_device(drv, &u.uo);
    }
    else
    {
        if (direct_read(process, (void*)&u.ep_open, sizeof(USB_EP_OPEN)))
            stm32_usb_open_ep(drv, process, handle, &u.ep_open);
    }
}

static inline void stm32_usb_close_ep(SHARED_USB_DRV* drv, int num)
{
    if (!stm32_usb_ep_flush(drv, num))
        return;
    if (num & USB_EP_IN)
        ep_toggle_bits(num, USB_EPTX_STAT, USB_EP_TX_DIS);
    else
        ep_toggle_bits(num, USB_EPRX_STAT, USB_EP_RX_DIS);

    EP* ep = ep_data(drv, num);
    free(ep);
    num & USB_EP_IN ? (drv->usb.in[USB_EP_NUM(num)] = NULL) : (drv->usb.out[USB_EP_NUM(num)] = NULL);
}

static inline void stm32_usb_close_device(SHARED_USB_DRV* drv)
{
    //disable pullup
    USB->BCDR &= ~USB_BCDR_DPPU;

    int i;
    //disable interrupts
    NVIC_DisableIRQ(USB_IRQn);
    irq_unregister(USB_IRQn);

    //close all endpoints
    for (i = 0; i < USB_EP_COUNT_MAX; ++i)
    {
        if (drv->usb.out[i] != NULL)
            stm32_usb_close_ep(drv, i);
        if (drv->usb.in[i] != NULL)
            stm32_usb_close_ep(drv, USB_EP_IN | i);
    }
    drv->usb.device = INVALID_HANDLE;

    //power down, disable all interrupts
    USB->DADDR = 0;
    USB->CNTR = USB_CNTR_PDWN | USB_CNTR_FRES;

    //disable clock
    RCC->APB1ENR &= ~RCC_APB1ENR_USBEN;

    //disable pins
    ack_gpio(drv, GPIO_DISABLE_PIN, USB_DM, 0, 0);
    ack_gpio(drv, GPIO_DISABLE_PIN, USB_DP, 0, 0);
}

static inline void stm32_usb_close(SHARED_USB_DRV* drv, unsigned int handle)
{
    if (handle == USB_HANDLE_DEVICE)
        stm32_usb_close_device(drv);
    else
        stm32_usb_close_ep(drv, handle);
}

static inline void stm32_usb_set_address(SHARED_USB_DRV* drv, int addr)
{
    //address will be set after STATUS IN packet
    if (addr)
        drv->usb.addr = addr;
    else
        USB->DADDR = USB_DADDR_EF;
}

static inline void stm32_usb_read(SHARED_USB_DRV* drv, unsigned int num, HANDLE block, unsigned int size, HANDLE process)
{
    if (USB_EP_NUM(num) >= USB_EP_COUNT_MAX)
    {
        block_send(block, process);
        ipc_post_error(process, ERROR_INVALID_PARAMS);
        return;
    }
    EP* ep = drv->usb.out[USB_EP_NUM(num)];
    if (ep == NULL)
    {
        block_send(block, process);
        ipc_post_error(process, ERROR_NOT_CONFIGURED);
        return;
    }
    if (ep->io_active)
    {
        block_send(block, process);
        ipc_post_error(process, ERROR_IN_PROGRESS);
        return;
    }
    //no blocks for ZLP
    ep->size = size;
    if (ep->size)
    {
        ep->block = block;
        if ((ep->ptr = block_open(ep->block)) == NULL)
        {
            block_send(block, process);
            ipc_post_error(process, get_last_error());
            return;
        }
    }
    ep->processed = 0;
    ep->io_active = true;
    ep_toggle_bits(num, USB_EPRX_STAT, USB_EP_RX_VALID);
}

static inline void stm32_usb_write(SHARED_USB_DRV* drv, unsigned int num, HANDLE block, unsigned int size, HANDLE process)
{
    if (USB_EP_NUM(num) >= USB_EP_COUNT_MAX)
    {
        block_send(block, process);
        ipc_post_error(process, ERROR_INVALID_PARAMS);
        return;
    }
    EP* ep = drv->usb.in[USB_EP_NUM(num)];
    if (ep == NULL)
    {
        block_send(block, process);
        ipc_post_error(process, ERROR_NOT_CONFIGURED);
        return;
    }
    if (ep->io_active)
    {
        block_send(block, process);
        ipc_post_error(process, ERROR_IN_PROGRESS);
        return;
    }
    ep->size = size;
    //no blocks for ZLP
    if (ep->size)
    {
        ep->block = block;
        if ((ep->ptr = block_open(ep->block)) == NULL)
        {
            block_send(block, process);
            ipc_post_error(process, get_last_error());
            return;
        }
    }
    ep->processed = 0;
    ep->io_active = true;

    stm32_usb_tx(drv, num);
}

*/


void board_usb_init(COMM* comm)
{
    int i;
    comm->drv.addr = 0;
    comm->drv.suspend = false;
    for (i = 0; i < USB_EP_COUNT_MAX; ++i)
    {
        comm->drv.out[i].ptr = comm->drv.in[i].ptr = NULL;
        comm->drv.out[i].size = comm->drv.in[i].size = 0;
        comm->drv.out[i].mps = comm->drv.in[i].mps = 0;
        comm->drv.out[i].io_active = comm->drv.in[i].io_active = false;
    }
    //enable DM/DP
    gpio_enable_pin(USB_DM, GPIO_MODE_AF | GPIO_OT_PUSH_PULL | GPIO_SPEED_HIGH, AF0);
    gpio_enable_pin(USB_DP, GPIO_MODE_AF | GPIO_OT_PUSH_PULL | GPIO_SPEED_HIGH, AF0);

    //enable clock
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;
}

bool board_usb_start(COMM* comm)
{
    int i;

    //power up and wait tStartup
    USB->CNTR &= ~USB_CNTR_PDWN;
    delay_us(1);
    USB->CNTR &= ~USB_CNTR_FRES;

    //clear any spurious pending interrupts
    USB->ISTR = 0;

    //buffer descriptor table at top
    USB->BTABLE = 0;

    for (i = 0; i < USB_EP_COUNT_MAX; ++i)
    {
        USB_BUFFER_DESCRIPTORS[i].ADDR_TX = 0;
        USB_BUFFER_DESCRIPTORS[i].COUNT_TX = 0;
        USB_BUFFER_DESCRIPTORS[i].ADDR_RX = 0;
        USB_BUFFER_DESCRIPTORS[i].COUNT_RX = 0;
    }

    //pullup device
    USB->BCDR |= USB_BCDR_DPPU;
    return true;
}

void board_usb_stop(COMM* comm)
{
    //disable pullup
    USB->BCDR &= ~USB_BCDR_DPPU;

    int i;
    //close all endpoints
    for (i = 0; i < USB_EP_COUNT_MAX; ++i)
    {
/*        if (comm->out[i] != NULL)
            stm32_usb_close_ep(drv, i);
        if (drv->usb.in[i] != NULL)
            stm32_usb_close_ep(drv, USB_EP_IN | i);*/
    }

    //power down, disable all interrupts
    USB->DADDR = 0;
    USB->CNTR = USB_CNTR_PDWN | USB_CNTR_FRES;
}

static inline void usb_reset(COMM* comm)
{
    comm->drv.suspend = false;
    //enable function
    USB->DADDR = USB_DADDR_EF;

    //TODO: call device reset
    printf("USB reset\n\r");
}

static inline void usb_suspend(COMM* comm)
{
    comm->drv.suspend = true;
    //TODO: call device suspend
    printf("USB suspend\n\r");
}

static inline void usb_wakeup(COMM* comm)
{
    comm->drv.suspend = false;
    //TODO: call device suspend
    printf("USB wakeup\n\r");
}

void board_usb_request(COMM* comm)
{
    //TODO: wdt

    uint16_t sta = USB->ISTR;

    //transfer complete. Most often called
    if (sta & USB_ISTR_CTR)
    {
        printf("USB CTR\n\r");
//        usb_ctr(drv);
        return;
    }
    //rarely called
    if (sta & USB_ISTR_RESET)
    {
        usb_reset(comm);
        USB->ISTR &= ~USB_ISTR_RESET;
        return;
    }
    if ((sta & USB_ISTR_SUSP) && (!comm->drv.suspend))
    {
        usb_suspend(comm);
        USB->ISTR &= ~USB_ISTR_SUSP;
        return;
    }
    if (sta & USB_ISTR_WKUP)
    {
        usb_wakeup(comm);
        USB->ISTR &= ~USB_ISTR_WKUP;
        return;
    }
#if (USB_DEBUG_ERRORS)
    if (sta & USB_ISTR_ERR)
    {
#if (DFU_DEBUG)
        printf("USB hardware error\n\r");
#endif
        USB->ISTR &= ~USB_ISTR_ERR;
        return;
    }
    if (sta & USB_ISTR_PMAOVR)
    {
#if (DFU_DEBUG)
        printf("USB overflow\n\r");
#endif
        USB->ISTR &= ~USB_ISTR_PMAOVR;
        return;
    }
#endif
}

