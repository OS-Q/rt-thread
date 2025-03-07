/**************************************************************************//**
*
* @copyright (C) 2020 Nuvoton Technology Corp. All rights reserved.
*
* SPDX-License-Identifier: Apache-2.0
*
* Change Logs:
* Date            Author           Notes
* 2021-7-15       Wayne            First version
*
******************************************************************************/

#include <rtconfig.h>

#if defined(BSP_USING_PDMA)

#include <rtdevice.h>
#include <rtthread.h>
#include <drv_pdma.h>
#include <nu_bitutil.h>
#include "drv_sys.h"

/* Private define ---------------------------------------------------------------*/
// RT_DEV_NAME_PREFIX pdma

#ifndef NU_PDMA_MEMFUN_ACTOR_MAX
    #define NU_PDMA_MEMFUN_ACTOR_MAX (4)
#endif

/* To select the first PDMA base */
#if !defined(USE_MA35D1_SUBM)
    #define DEF_PDMA_BASE_START   PDMA0_BASE
#else
    #define DEF_PDMA_BASE_START   PDMA2_BASE
#endif

enum
{
    PDMA_START = -1,
#if defined(BSP_USING_PDMA0)
    PDMA0_IDX,
#endif
#if defined(BSP_USING_PDMA1)
    PDMA1_IDX,
#endif
#if defined(BSP_USING_PDMA2)
    PDMA2_IDX,
#endif
#if defined(BSP_USING_PDMA3)
    PDMA3_IDX,
#endif
    PDMA_CNT
};

#define NU_PDMA_SG_TBL_MAXSIZE         (NU_PDMA_SG_LIMITED_DISTANCE/sizeof(DSCT_T))

#define NU_PDMA_CH_MAX (PDMA_CNT*PDMA_CH_MAX)     /* Specify maximum channels of PDMA */
#define NU_PDMA_CH_Pos (0)                        /* Specify first channel number of PDMA */
#define NU_PDMA_CH_Msk (PDMA_CH_Msk << NU_PDMA_CH_Pos)
#define NU_PDMA_GET_BASE(ch)   (PDMA_T *)((((ch)/PDMA_CH_MAX)*0x10000UL) + DEF_PDMA_BASE_START)
#define NU_PDMA_GET_MOD_IDX(ch)   ((ch)/PDMA_CH_MAX)
#define NU_PDMA_GET_MOD_CHIDX(ch)   ((ch)%PDMA_CH_MAX)

/* Private typedef --------------------------------------------------------------*/
struct nu_pdma_periph_ctl
{
    uint32_t     m_u32Peripheral;
    nu_pdma_memctrl_t  m_eMemCtl;
};
typedef struct nu_pdma_periph_ctl nu_pdma_periph_ctl_t;

struct nu_pdma_chn
{
    struct nu_pdma_chn_cb  m_sCB_Event;
    struct nu_pdma_chn_cb  m_sCB_Trigger;
    struct nu_pdma_chn_cb  m_sCB_Disable;

    nu_pdma_desc_t        *m_ppsSgtbl;
    uint32_t               m_u32WantedSGTblNum;

    uint32_t               m_u32EventFilter;
    uint32_t               m_u32IdleTimeout_us;
    nu_pdma_periph_ctl_t   m_spPeripCtl;
};
typedef struct nu_pdma_chn nu_pdma_chn_t;

struct nu_pdma_memfun_actor
{
    int         m_i32ChannID;
    uint32_t    m_u32Result;
    rt_sem_t    m_psSemMemFun;
} ;
typedef struct nu_pdma_memfun_actor *nu_pdma_memfun_actor_t;

/* Private functions ------------------------------------------------------------*/
static int nu_pdma_peripheral_set(uint32_t u32PeriphType);
static void nu_pdma_init(void);
static void nu_pdma_channel_enable(int i32ChannID);
static void nu_pdma_channel_disable(int i32ChannID);
static void nu_pdma_channel_reset(int i32ChannID);
static rt_err_t nu_pdma_timeout_set(int i32ChannID, int i32Timeout_us);
static void nu_pdma_periph_ctrl_fill(int i32ChannID, int i32CtlPoolIdx);
static rt_ssize_t nu_pdma_memfun(void *dest, void *src, uint32_t u32DataWidth, unsigned int u32TransferCnt, nu_pdma_memctrl_t eMemCtl);
static void nu_pdma_memfun_cb(void *pvUserData, uint32_t u32Events);
static void nu_pdma_memfun_actor_init(void);
static int nu_pdma_memfun_employ(void);
static int nu_pdma_non_transfer_count_get(int32_t i32ChannID);

/* Public functions -------------------------------------------------------------*/


/* Private variables ------------------------------------------------------------*/
static volatile int nu_pdma_inited = 0;
static volatile uint32_t nu_pdma_chn_mask_arr[PDMA_CNT] = {0};
static nu_pdma_chn_t nu_pdma_chn_arr[NU_PDMA_CH_MAX];
static volatile uint32_t nu_pdma_memfun_actor_mask = 0;
static volatile uint32_t nu_pdma_memfun_actor_maxnum = 0;
static rt_sem_t nu_pdma_memfun_actor_pool_sem = RT_NULL;
static rt_mutex_t nu_pdma_memfun_actor_pool_lock = RT_NULL;
static void nu_pdma_isr(int vector, void *pvdata);

const static struct nu_module nu_pdma_arr[] =
{
#if defined(BSP_USING_PDMA0)
    {
        .name = "pdma0",
        .m_pvBase = (void *)PDMA0,
        .u32RstId = PDMA0_RST,
        .eIRQn = PDMA0_IRQn
    },
#endif
#if defined(BSP_USING_PDMA1)
    {
        .name = "pdma1",
        .m_pvBase = (void *)PDMA1,
        .u32RstId = PDMA1_RST,
        .eIRQn = PDMA1_IRQn
    },
#endif
#if defined(BSP_USING_PDMA2)
    {
        .name = "pdma2",
        .m_pvBase = (void *)PDMA2,
        .u32RstId = PDMA2_RST,
        .eIRQn = PDMA2_IRQn
    },
#endif
#if defined(BSP_USING_PDMA3)
    {
        .name = "pdma3",
        .m_pvBase = (void *)PDMA3,
        .u32RstId = PDMA3_RST,
        .eIRQn = PDMA3_IRQn
    }
#endif
};

static const nu_pdma_periph_ctl_t g_nu_pdma_peripheral_ctl_pool[ ] =
{
    // M2M
    { PDMA_MEM, eMemCtl_SrcInc_DstInc },

    // M2P
    { PDMA_UART0_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART1_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART2_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART3_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART4_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART5_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART6_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART7_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART8_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART9_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART10_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART11_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART12_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART13_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART14_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART15_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_UART16_TX, eMemCtl_SrcInc_DstFix },

    { PDMA_QSPI0_TX, eMemCtl_SrcInc_DstFix },
    { PDMA_QSPI1_TX, eMemCtl_SrcInc_DstFix },

    { PDMA_SPI0_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_SPI1_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_SPI2_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_SPI3_TX,  eMemCtl_SrcInc_DstFix },

    { PDMA_I2C0_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_I2C1_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_I2C2_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_I2C3_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_I2C4_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_I2C5_TX,  eMemCtl_SrcInc_DstFix },

    { PDMA_I2S0_TX,  eMemCtl_SrcInc_DstFix },
    { PDMA_I2S1_TX,  eMemCtl_SrcInc_DstFix },

    // P2M
    { PDMA_UART0_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART1_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART2_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART3_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART4_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART5_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART6_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART7_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART8_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART9_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART10_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART11_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART12_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART13_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART14_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART15_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_UART16_RX, eMemCtl_SrcFix_DstInc },

    { PDMA_QSPI0_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_QSPI1_RX, eMemCtl_SrcFix_DstInc },

    { PDMA_SPI0_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_SPI1_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_SPI2_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_SPI3_RX, eMemCtl_SrcFix_DstInc },

    { PDMA_I2C0_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_I2C1_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_I2C2_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_I2C3_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_I2C4_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_I2C5_RX, eMemCtl_SrcFix_DstInc },

    { PDMA_I2S0_RX, eMemCtl_SrcFix_DstInc },
    { PDMA_I2S1_RX, eMemCtl_SrcFix_DstInc },

};
#define NU_PERIPHERAL_SIZE ( sizeof(g_nu_pdma_peripheral_ctl_pool) / sizeof(g_nu_pdma_peripheral_ctl_pool[0]) )

static struct nu_pdma_memfun_actor nu_pdma_memfun_actor_arr[NU_PDMA_MEMFUN_ACTOR_MAX];

static int nu_pdma_check_is_nonallocated(uint32_t u32ChnId)
{
    uint32_t mod_idx = NU_PDMA_GET_MOD_IDX(u32ChnId);
    RT_ASSERT(mod_idx < PDMA_CNT);
    return !(nu_pdma_chn_mask_arr[mod_idx] & (1 << NU_PDMA_GET_MOD_CHIDX(u32ChnId)));
}

static int nu_pdma_peripheral_set(uint32_t u32PeriphType)
{
    int idx = 0;

    while (idx < NU_PERIPHERAL_SIZE)
    {
        if (g_nu_pdma_peripheral_ctl_pool[idx].m_u32Peripheral == u32PeriphType)
            return idx;
        idx++;
    }

    // Not such peripheral
    return -1;
}

static void nu_pdma_periph_ctrl_fill(int i32ChannID, int i32CtlPoolIdx)
{
    nu_pdma_chn_t *psPdmaChann = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos];
    psPdmaChann->m_spPeripCtl.m_u32Peripheral = g_nu_pdma_peripheral_ctl_pool[i32CtlPoolIdx].m_u32Peripheral;
    psPdmaChann->m_spPeripCtl.m_eMemCtl = g_nu_pdma_peripheral_ctl_pool[i32CtlPoolIdx].m_eMemCtl;
}

/**
 * Hardware PDMA Initialization
 */
static void nu_pdma_init(void)
{
    int i;

    if (nu_pdma_inited)
        return;

    rt_memset(nu_pdma_chn_arr, 0x00, NU_PDMA_CH_MAX * sizeof(nu_pdma_chn_t));

    for (i = (PDMA_START + 1); i < PDMA_CNT; i++)
    {
        nu_pdma_chn_mask_arr[i] = ~(NU_PDMA_CH_Msk);

        nu_sys_ip_reset(nu_pdma_arr[i].u32RstId);

        /* Initialize PDMA setting */
        PDMA_Open((PDMA_T *)nu_pdma_arr[i].m_pvBase, PDMA_CH_Msk);

        PDMA_Close((PDMA_T *)nu_pdma_arr[i].m_pvBase);

        /* Register PDMA ISR */
        rt_hw_interrupt_install(nu_pdma_arr[i].eIRQn, nu_pdma_isr, nu_pdma_arr[i].m_pvBase, nu_pdma_arr[i].name);
        rt_hw_interrupt_umask(nu_pdma_arr[i].eIRQn);
    }

    nu_pdma_inited = 1;
}

static inline void nu_pdma_channel_enable(int i32ChannID)
{
    PDMA_T *PDMA = NU_PDMA_GET_BASE(i32ChannID);
    int u32ModChannId = NU_PDMA_GET_MOD_CHIDX(i32ChannID);

    /* Clean descriptor table control register. */
    PDMA->DSCT[u32ModChannId].CTL = 0UL;

    /* Enable the channel */
    PDMA->CHCTL |= (1 << u32ModChannId);
}

static inline void nu_pdma_channel_disable(int i32ChannID)
{
    PDMA_T *PDMA = NU_PDMA_GET_BASE(i32ChannID);
    PDMA->CHCTL &= ~(1 << NU_PDMA_GET_MOD_CHIDX(i32ChannID));
}

static inline void nu_pdma_channel_reset(int i32ChannID)
{
    PDMA_T *PDMA = NU_PDMA_GET_BASE(i32ChannID);
    int u32ModChannId = NU_PDMA_GET_MOD_CHIDX(i32ChannID);

    PDMA->CHRST = (1 << u32ModChannId);

    /* Wait for cleared channel CHCTL. */
    while ((PDMA->CHCTL & (1 << u32ModChannId)));
}

static rt_err_t nu_pdma_timeout_set(int i32ChannID, int i32Timeout_us)
{
    rt_err_t ret = RT_EINVAL;
    PDMA_T *PDMA = NULL;
    uint32_t u32ModChannId;

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_timeout_set;

    PDMA = NU_PDMA_GET_BASE(i32ChannID);

    u32ModChannId = NU_PDMA_GET_MOD_CHIDX(i32ChannID);

    nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_u32IdleTimeout_us = i32Timeout_us;

    if (i32Timeout_us)
    {
        uint32_t u32ToClk_Max   = 1000000ul / (CLK_GetSYSCLK1Freq() / (1 << 8));
        uint32_t u32Divider     = (i32Timeout_us / u32ToClk_Max) / (1 << 16);
        uint32_t u32TOutCnt     = (i32Timeout_us / u32ToClk_Max) % (1 << 16);

        PDMA_DisableTimeout(PDMA,  1 << u32ModChannId);
        PDMA_EnableInt(PDMA, u32ModChannId, PDMA_INT_TIMEOUT);    // Interrupt type

        if (u32Divider > 7)
        {
            u32Divider = 7;
            u32TOutCnt = (1 << 16) - 1;
        }

        if (u32ModChannId < 8)
            PDMA->TOUTPSC = (PDMA->TOUTPSC & ~(0x7ul << (PDMA_TOUTPSC_TOUTPSC1_Pos * u32ModChannId))) | (u32Divider << (PDMA_TOUTPSC_TOUTPSC1_Pos * u32ModChannId));
        else
            PDMA->TOUTPSC1 = (PDMA->TOUTPSC1 & ~(0x7ul << (PDMA_TOUTPSC_TOUTPSC1_Pos * u32ModChannId))) | (u32Divider << (PDMA_TOUTPSC_TOUTPSC1_Pos * u32ModChannId));

        //rt_kprintf("[%d]HCLK=%d, u32Divider=%d,  u32TOutCnt=%d\n", i32Timeout_us, CLK_GetSYSCLK1Freq(), u32Divider, u32TOutCnt );

        PDMA_SetTimeOut(PDMA,  u32ModChannId, 1, u32TOutCnt);

        ret = RT_EOK;
    }
    else
    {
        PDMA_DisableInt(PDMA, u32ModChannId, PDMA_INT_TIMEOUT);    // Interrupt type
        PDMA_DisableTimeout(PDMA,  1 << u32ModChannId);
    }

exit_nu_pdma_timeout_set:

    return -(ret);
}

void nu_pdma_channel_terminate(int i32ChannID)
{
    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_pdma_channel_terminate;

    /* Disable timeout function of specified channel. */
    nu_pdma_timeout_set(i32ChannID, 0);

    /* Reset specified channel. */
    nu_pdma_channel_reset(i32ChannID);

    /* Enable specified channel after reset. */
    nu_pdma_channel_enable(i32ChannID);

exit_pdma_channel_terminate:

    return;
}

int nu_pdma_channel_allocate(int32_t i32PeripType)
{
    int ChnId, i32PeripCtlIdx, j;

    nu_pdma_init();

    if ((i32PeripCtlIdx = nu_pdma_peripheral_set(i32PeripType)) < 0)
        goto exit_nu_pdma_channel_allocate;

    for (j = (PDMA_START + 1); j < PDMA_CNT; j++)
    {
        /* Find the position of first '0' in nu_pdma_chn_mask_arr[j]. */
        ChnId = nu_cto(nu_pdma_chn_mask_arr[j]);
        if (ChnId < PDMA_CH_MAX)
        {
            nu_pdma_chn_mask_arr[j] |= (1 << ChnId);
            ChnId += (j * PDMA_CH_MAX);
            rt_memset(nu_pdma_chn_arr + ChnId - NU_PDMA_CH_Pos, 0x00, sizeof(nu_pdma_chn_t));

            /* Set idx number of g_nu_pdma_peripheral_ctl_pool */
            nu_pdma_periph_ctrl_fill(ChnId, i32PeripCtlIdx);

            /* Reset channel */
            nu_pdma_channel_terminate(ChnId);

            return ChnId;
        }
    }

exit_nu_pdma_channel_allocate:
    // No channel available
    return -(RT_ERROR);
}

rt_err_t nu_pdma_channel_free(int i32ChannID)
{
    rt_err_t ret = RT_EINVAL;

    if (! nu_pdma_inited)
        goto exit_nu_pdma_channel_free;

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_channel_free;

    if ((i32ChannID < NU_PDMA_CH_MAX) && (i32ChannID >= NU_PDMA_CH_Pos))
    {
        nu_pdma_chn_mask_arr[NU_PDMA_GET_MOD_IDX(i32ChannID)] &= ~(1 << NU_PDMA_GET_MOD_CHIDX(i32ChannID));
        nu_pdma_channel_disable(i32ChannID);
        ret =  RT_EOK;
    }
exit_nu_pdma_channel_free:

    return -(ret);
}

rt_err_t nu_pdma_filtering_set(int i32ChannID, uint32_t u32EventFilter)
{
    rt_err_t ret = RT_EINVAL;
    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_filtering_set;

    nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_u32EventFilter = u32EventFilter;

    ret = RT_EOK;

exit_nu_pdma_filtering_set:

    return -(ret) ;
}

uint32_t nu_pdma_filtering_get(int i32ChannID)
{
    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_filtering_get;

    return nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_u32EventFilter;

exit_nu_pdma_filtering_get:

    return 0;
}

rt_err_t nu_pdma_callback_register(int i32ChannID, nu_pdma_chn_cb_t psChnCb)
{
    rt_err_t ret = RT_EINVAL;
    nu_pdma_chn_cb_t psChnCb_Current = RT_NULL;

    RT_ASSERT(psChnCb != RT_NULL);

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_callback_register;

    switch (psChnCb->m_eCBType)
    {
    case eCBType_Event:
        psChnCb_Current = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_sCB_Event;
        break;
    case eCBType_Trigger:
        psChnCb_Current = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_sCB_Trigger;
        break;
    case eCBType_Disable:
        psChnCb_Current = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_sCB_Disable;
        break;
    default:
        goto exit_nu_pdma_callback_register;
    }

    psChnCb_Current->m_pfnCBHandler = psChnCb->m_pfnCBHandler;
    psChnCb_Current->m_pvUserData = psChnCb->m_pvUserData;

    ret = RT_EOK;

exit_nu_pdma_callback_register:

    return -(ret) ;
}

nu_pdma_cb_handler_t nu_pdma_callback_hijack(int i32ChannID, nu_pdma_cbtype_t eCBType, nu_pdma_chn_cb_t psChnCb_Hijack)
{
    nu_pdma_chn_cb_t psChnCb_Current = RT_NULL;
    struct nu_pdma_chn_cb sChnCB_Tmp;

    RT_ASSERT(psChnCb_Hijack != NULL);

    sChnCB_Tmp.m_pfnCBHandler = RT_NULL;

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_callback_hijack;

    switch (eCBType)
    {
    case eCBType_Event:
        psChnCb_Current = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_sCB_Event;
        break;
    case eCBType_Trigger:
        psChnCb_Current = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_sCB_Trigger;
        break;
    case eCBType_Disable:
        psChnCb_Current = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_sCB_Disable;
        break;
    default:
        goto exit_nu_pdma_callback_hijack;
    }

    /* Backup */
    sChnCB_Tmp.m_pfnCBHandler = psChnCb_Current->m_pfnCBHandler;
    sChnCB_Tmp.m_pvUserData = psChnCb_Current->m_pvUserData;

    /* Update */
    psChnCb_Current->m_pfnCBHandler = psChnCb_Hijack->m_pfnCBHandler;
    psChnCb_Current->m_pvUserData = psChnCb_Hijack->m_pvUserData;

    /* Restore */
    psChnCb_Hijack->m_pfnCBHandler = sChnCB_Tmp.m_pfnCBHandler;
    psChnCb_Hijack->m_pvUserData = sChnCB_Tmp.m_pvUserData;

exit_nu_pdma_callback_hijack:

    return sChnCB_Tmp.m_pfnCBHandler;
}

static int nu_pdma_non_transfer_count_get(int32_t i32ChannID)
{
    PDMA_T *PDMA = NU_PDMA_GET_BASE(i32ChannID);
    return ((PDMA->DSCT[NU_PDMA_GET_MOD_CHIDX(i32ChannID)].CTL & PDMA_DSCT_CTL_TXCNT_Msk) >> PDMA_DSCT_CTL_TXCNT_Pos) + 1;
}

int nu_pdma_transferred_byte_get(int32_t i32ChannID, int32_t i32TriggerByteLen)
{
    int i32BitWidth = 0;
    int cur_txcnt = 0;
    PDMA_T *PDMA;

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_transferred_byte_get;

    PDMA = NU_PDMA_GET_BASE(i32ChannID);

    if ((PDMA->DSCT[NU_PDMA_GET_MOD_CHIDX(i32ChannID)].CTL & PDMA_DSCT_CTL_OPMODE_Msk) != PDMA_OP_SCATTER)
    {
        i32BitWidth = PDMA->DSCT[NU_PDMA_GET_MOD_CHIDX(i32ChannID)].CTL & PDMA_DSCT_CTL_TXWIDTH_Msk;
        i32BitWidth = (i32BitWidth == PDMA_WIDTH_8) ? 1 : (i32BitWidth == PDMA_WIDTH_16) ? 2 : (i32BitWidth == PDMA_WIDTH_32) ? 4 : 0;

        cur_txcnt = nu_pdma_non_transfer_count_get(i32ChannID);

        // rt_kprintf("\n[%s] %d %d %02x\n", __func__, i32ChannID, cur_txcnt, (PDMA->DSCT[NU_PDMA_GET_MOD_CHIDX(i32ChannID)].CTL & PDMA_DSCT_CTL_OPMODE_Msk) );

        return (i32TriggerByteLen - (cur_txcnt) * i32BitWidth);
    }

    // rt_kprintf("\n@@@@ %d %02x @@@@\n", i32ChannID, PDMA->DSCT[NU_PDMA_GET_MOD_CHIDX(i32ChannID)].CTL & PDMA_DSCT_CTL_OPMODE_Msk);

    return 0;

exit_nu_pdma_transferred_byte_get:

    return -1;
}

nu_pdma_desc_t nu_pdma_get_channel_desc(int32_t i32ChannID)
{
    PDMA_T *PDMA;

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_get_srcaddr;

    PDMA = NU_PDMA_GET_BASE(i32ChannID);

    return &PDMA->DSCT[NU_PDMA_GET_MOD_CHIDX(i32ChannID)];

exit_nu_pdma_get_srcaddr:

    return RT_NULL;
}

nu_pdma_memctrl_t nu_pdma_channel_memctrl_get(int i32ChannID)
{
    nu_pdma_memctrl_t eMemCtrl = eMemCtl_Undefined;

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_channel_memctrl_get;

    eMemCtrl = nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_spPeripCtl.m_eMemCtl;

exit_nu_pdma_channel_memctrl_get:

    return eMemCtrl;
}

rt_err_t nu_pdma_channel_memctrl_set(int i32ChannID, nu_pdma_memctrl_t eMemCtrl)
{
    rt_err_t ret = RT_EINVAL;
    nu_pdma_chn_t *psPdmaChann = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos];

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_channel_memctrl_set;
    else if ((eMemCtrl < eMemCtl_SrcFix_DstFix) || (eMemCtrl > eMemCtl_SrcInc_DstInc))
        goto exit_nu_pdma_channel_memctrl_set;

    /* PDMA_MEM/SAR_FIX/BURST mode is not supported. */
    if ((psPdmaChann->m_spPeripCtl.m_u32Peripheral == PDMA_MEM) &&
            ((eMemCtrl == eMemCtl_SrcFix_DstInc) || (eMemCtrl == eMemCtl_SrcFix_DstFix)))
        goto exit_nu_pdma_channel_memctrl_set;

    nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_spPeripCtl.m_eMemCtl = eMemCtrl;

    ret = RT_EOK;

exit_nu_pdma_channel_memctrl_set:

    return -(ret);
}

static void nu_pdma_channel_memctrl_fill(nu_pdma_memctrl_t eMemCtl, uint32_t *pu32SrcCtl, uint32_t *pu32DstCtl)
{
    switch ((int)eMemCtl)
    {
    case eMemCtl_SrcFix_DstFix:
        *pu32SrcCtl = PDMA_SAR_FIX;
        *pu32DstCtl = PDMA_DAR_FIX;
        break;
    case eMemCtl_SrcFix_DstInc:
        *pu32SrcCtl = PDMA_SAR_FIX;
        *pu32DstCtl = PDMA_DAR_INC;
        break;
    case eMemCtl_SrcInc_DstFix:
        *pu32SrcCtl = PDMA_SAR_INC;
        *pu32DstCtl = PDMA_DAR_FIX;
        break;
    case eMemCtl_SrcInc_DstInc:
        *pu32SrcCtl = PDMA_SAR_INC;
        *pu32DstCtl = PDMA_DAR_INC;
        break;
    default:
        break;
    }
}

/* This is for Scatter-gather DMA. */
rt_err_t nu_pdma_desc_setup(int i32ChannID, nu_pdma_desc_t dma_desc, uint32_t u32DataWidth, uint32_t u32AddrSrc,
                            uint32_t u32AddrDst, int32_t i32TransferCnt, nu_pdma_desc_t next, uint32_t u32BeSilent)
{
    nu_pdma_periph_ctl_t *psPeriphCtl = NULL;

    uint32_t u32SrcCtl = 0;
    uint32_t u32DstCtl = 0;

    rt_err_t ret = RT_EINVAL;

    if (!dma_desc)
        goto exit_nu_pdma_desc_setup;
    else if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_desc_setup;
    else if (!(u32DataWidth == 8 || u32DataWidth == 16 || u32DataWidth == 32))
        goto exit_nu_pdma_desc_setup;
    else if ((u32AddrSrc % (u32DataWidth / 8)) || (u32AddrDst % (u32DataWidth / 8)))
        goto exit_nu_pdma_desc_setup;
    else if (i32TransferCnt > NU_PDMA_MAX_TXCNT)
        goto exit_nu_pdma_desc_setup;

    psPeriphCtl = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_spPeripCtl;

    nu_pdma_channel_memctrl_fill(psPeriphCtl->m_eMemCtl, &u32SrcCtl, &u32DstCtl);

    dma_desc->CTL = ((i32TransferCnt - 1) << PDMA_DSCT_CTL_TXCNT_Pos) |
                    ((u32DataWidth == 8) ? PDMA_WIDTH_8 : (u32DataWidth == 16) ? PDMA_WIDTH_16 : PDMA_WIDTH_32) |
                    u32SrcCtl |
                    u32DstCtl |
                    PDMA_OP_BASIC;

    dma_desc->SA = u32AddrSrc;
    dma_desc->DA = u32AddrDst;
    dma_desc->NEXT = 0;  /* Terminating node by default. */

    if (psPeriphCtl->m_u32Peripheral == PDMA_MEM)
    {
        /* For M2M transfer */
        dma_desc->CTL |= (PDMA_REQ_BURST | PDMA_BURST_32);
    }
    else
    {
        /* For P2M and M2P transfer */
        dma_desc->CTL |= (PDMA_REQ_SINGLE);
    }

    if (next)
    {
        /* Link to Next and modify to scatter-gather DMA mode. */
        dma_desc->CTL = (dma_desc->CTL & ~PDMA_DSCT_CTL_OPMODE_Msk) | PDMA_OP_SCATTER;
        dma_desc->NEXT = (uint32_t)next;

    }

    /* Be silent */
    if (u32BeSilent)
        dma_desc->CTL |= PDMA_DSCT_CTL_TBINTDIS_Msk;

    ret = RT_EOK;

exit_nu_pdma_desc_setup:

    return -(ret);
}

rt_err_t nu_pdma_sgtbls_allocate(nu_pdma_desc_t *ppsSgtbls, int num)
{
    int i;

    RT_ASSERT(ppsSgtbls != NULL);
    RT_ASSERT(num > 0);

    for (i = 0; i < num; i++)
    {
        ppsSgtbls[i] = (nu_pdma_desc_t) rt_malloc_align(RT_ALIGN(sizeof(DSCT_T), 64), 64);
        RT_ASSERT(ppsSgtbls[i] != RT_NULL);
        rt_memset((void *)ppsSgtbls[i], 0, RT_ALIGN(sizeof(DSCT_T), 64));
    }

    return RT_EOK;
}

void nu_pdma_sgtbls_free(nu_pdma_desc_t *ppsSgtbls, int num)
{
    int i;

    RT_ASSERT(ppsSgtbls != NULL);
    RT_ASSERT(num > 0);

    for (i = 0; i < num; i++)
    {
        rt_free_align(ppsSgtbls[i]);
    }
}

static void _nu_pdma_transfer(int i32ChannID, uint32_t u32Peripheral, nu_pdma_desc_t head, uint32_t u32IdleTimeout_us)
{
    PDMA_T *PDMA = NU_PDMA_GET_BASE(i32ChannID);
    nu_pdma_chn_t *psPdmaChann = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos];

#if !defined(USE_MA35D1_SUBM)
    /* Writeback data in dcache to memory before transferring. */
    {
        static uint32_t bNonCacheAlignedWarning = 1;
        nu_pdma_desc_t next = head;
        int CACHE_LINE_SIZE = nu_cpu_dcache_line_size();
        while (next != RT_NULL)
        {
            uint32_t u32TxCnt     = ((next->CTL & PDMA_DSCT_CTL_TXCNT_Msk) >> PDMA_DSCT_CTL_TXCNT_Pos) + 1;
            uint32_t u32DataWidth = (1 << ((next->CTL & PDMA_DSCT_CTL_TXWIDTH_Msk) >> PDMA_DSCT_CTL_TXWIDTH_Pos));
            uint32_t u32SrcCtl    = (next->CTL & PDMA_DSCT_CTL_SAINC_Msk);
            uint32_t u32DstCtl    = (next->CTL & PDMA_DSCT_CTL_DAINC_Msk);
            uint32_t u32FlushLen  = u32TxCnt * u32DataWidth;

#if 0
            rt_kprintf("[%s] i32ChannID=%d\n", __func__, i32ChannID);
            rt_kprintf("[%s] PDMA=0x%08x\n", __func__, (uint32_t)PDMA);
            rt_kprintf("[%s] u32TxCnt=%d\n", __func__, u32TxCnt);
            rt_kprintf("[%s] u32DataWidth=%d\n", __func__, u32DataWidth);
            rt_kprintf("[%s] u32SrcCtl=0x%08x\n", __func__, u32SrcCtl);
            rt_kprintf("[%s] u32DstCtl=0x%08x\n", __func__, u32DstCtl);
            rt_kprintf("[%s] u32FlushLen=%d\n", __func__, u32FlushLen);
            rt_kprintf("[%s] DA=%08x\n", __func__, next->DA);
            rt_kprintf("[%s] SA=%08x\n", __func__, next->SA);
#endif

            /* Flush Src buffer into memory. */
            if ((u32SrcCtl == PDMA_SAR_INC)) // for M2P, M2M
                rt_hw_cpu_dcache_clean_and_invalidate((void *)next->SA, u32FlushLen);

            /* Flush Dst buffer into memory. */
            if ((u32DstCtl == PDMA_DAR_INC)) // for P2M, M2M
                rt_hw_cpu_dcache_clean_and_invalidate((void *)next->DA, u32FlushLen);

            /* Flush descriptor into memory */
            rt_hw_cpu_dcache_clean_and_invalidate((void *)next, sizeof(DSCT_T));

            if (bNonCacheAlignedWarning)
            {
                if ((u32FlushLen & (CACHE_LINE_SIZE - 1)) ||
                        (next->SA & (CACHE_LINE_SIZE - 1)) ||
                        (next->DA & (CACHE_LINE_SIZE - 1)) ||
                        ((rt_uint32_t)next & (CACHE_LINE_SIZE - 1)))
                {
                    /*
                        Race-condition avoidance between DMA-transferring and DCache write-back:
                        Source, destination, DMA descriptor address and length should be aligned at len(CACHE_LINE_SIZE)
                    */
                    bNonCacheAlignedWarning = 0;
                    //rt_kprintf("[PDMA-W]\n");
                }
            }

            next = (nu_pdma_desc_t)next->NEXT;

            if (next == head) break;
        }
    }
#endif

    nu_pdma_desc_t psDesc = nu_pdma_get_channel_desc(i32ChannID);

    PDMA_DisableTimeout(PDMA,  1 << NU_PDMA_GET_MOD_CHIDX(i32ChannID));

    /* Set scatter-gather mode and head */
    /* Take care the head structure, you should make sure cache-coherence. */
    PDMA_SetTransferMode(PDMA,
                        NU_PDMA_GET_MOD_CHIDX(i32ChannID),
                        u32Peripheral,
                        (head->NEXT != 0) ? 1 : 0,
                        (uint32_t)head);

    /* PDMA fetchs description on-demand if SG enabled. We check it valid in here. */
    if ( (u32Peripheral != PDMA_MEM) &&
        (head->NEXT != 0) &&
        (head->DA != psDesc->DA) )
    {
        RT_ASSERT(0);
    }

    PDMA_EnableInt(PDMA, NU_PDMA_GET_MOD_CHIDX(i32ChannID), PDMA_INT_TRANS_DONE);

    nu_pdma_timeout_set(i32ChannID, u32IdleTimeout_us);

    /* If peripheral is M2M, trigger it. */
    if (u32Peripheral == PDMA_MEM)
    {
        PDMA_Trigger(PDMA, NU_PDMA_GET_MOD_CHIDX(i32ChannID));
    }
    else if (psPdmaChann->m_sCB_Trigger.m_pfnCBHandler)
    {
        psPdmaChann->m_sCB_Trigger.m_pfnCBHandler(psPdmaChann->m_sCB_Trigger.m_pvUserData, psPdmaChann->m_sCB_Trigger.m_u32Reserved);
    }
}

static void _nu_pdma_free_sgtbls(nu_pdma_chn_t *psPdmaChann)
{
    if (psPdmaChann->m_ppsSgtbl)
    {
        nu_pdma_sgtbls_free(psPdmaChann->m_ppsSgtbl, psPdmaChann->m_u32WantedSGTblNum);
        rt_free_align((void *)psPdmaChann->m_ppsSgtbl);
        psPdmaChann->m_ppsSgtbl = RT_NULL;
        psPdmaChann->m_u32WantedSGTblNum = 0;
    }
}

static rt_err_t _nu_pdma_transfer_chain(int i32ChannID, uint32_t u32DataWidth, uint32_t u32AddrSrc, uint32_t u32AddrDst, uint32_t u32TransferCnt, uint32_t u32IdleTimeout_us)
{
    int i = 0;
    rt_err_t ret = RT_ERROR;
    nu_pdma_periph_ctl_t *psPeriphCtl = NULL;
    nu_pdma_chn_t *psPdmaChann = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos];

    nu_pdma_memctrl_t eMemCtl = nu_pdma_channel_memctrl_get(i32ChannID);

    rt_uint32_t u32Offset = 0;
    rt_uint32_t u32TxCnt = 0;

    psPeriphCtl = &psPdmaChann->m_spPeripCtl;

    if (psPdmaChann->m_u32WantedSGTblNum != (u32TransferCnt / NU_PDMA_MAX_TXCNT + 1))
    {
        if (psPdmaChann->m_u32WantedSGTblNum > 0)
            _nu_pdma_free_sgtbls(psPdmaChann);

        psPdmaChann->m_u32WantedSGTblNum = u32TransferCnt / NU_PDMA_MAX_TXCNT + 1;

        psPdmaChann->m_ppsSgtbl = (nu_pdma_desc_t *)rt_malloc_align(sizeof(nu_pdma_desc_t) * psPdmaChann->m_u32WantedSGTblNum, 4);
        if (!psPdmaChann->m_ppsSgtbl)
            goto exit__nu_pdma_transfer_chain;

        ret = nu_pdma_sgtbls_allocate(psPdmaChann->m_ppsSgtbl, psPdmaChann->m_u32WantedSGTblNum);
        if (ret != RT_EOK)
            goto exit__nu_pdma_transfer_chain;
    }

    for (i = 0; i < psPdmaChann->m_u32WantedSGTblNum; i++)
    {
        u32TxCnt = (u32TransferCnt > NU_PDMA_MAX_TXCNT) ? NU_PDMA_MAX_TXCNT : u32TransferCnt;

        ret = nu_pdma_desc_setup(i32ChannID,
                                 psPdmaChann->m_ppsSgtbl[i],
                                 u32DataWidth,
                                 (eMemCtl & 0x2ul) ? u32AddrSrc + u32Offset : u32AddrSrc, /* Src address is Inc or not. */
                                 (eMemCtl & 0x1ul) ? u32AddrDst + u32Offset : u32AddrDst, /* Dst address is Inc or not. */
                                 u32TxCnt,
                                 ((i + 1) == psPdmaChann->m_u32WantedSGTblNum) ? RT_NULL : psPdmaChann->m_ppsSgtbl[i + 1],
                                 ((i + 1) == psPdmaChann->m_u32WantedSGTblNum) ? 0 : 1); // Silent, w/o TD interrupt

        if (ret != RT_EOK)
            goto exit__nu_pdma_transfer_chain;

        u32TransferCnt -= u32TxCnt;
        u32Offset += (u32TxCnt * u32DataWidth / 8);
    }

    _nu_pdma_transfer(i32ChannID, psPeriphCtl->m_u32Peripheral, psPdmaChann->m_ppsSgtbl[0], u32IdleTimeout_us);

    ret = RT_EOK;

    return ret;

exit__nu_pdma_transfer_chain:

    _nu_pdma_free_sgtbls(psPdmaChann);

    return -(ret);
}

rt_err_t nu_pdma_transfer(int i32ChannID, uint32_t u32DataWidth, uint32_t u32AddrSrc, uint32_t u32AddrDst, uint32_t u32TransferCnt, uint32_t u32IdleTimeout_us)
{
    rt_err_t ret = RT_EINVAL;
    PDMA_T *PDMA = NU_PDMA_GET_BASE(i32ChannID);
    nu_pdma_desc_t head;
    nu_pdma_chn_t *psPdmaChann;

    nu_pdma_periph_ctl_t *psPeriphCtl = NULL;

    if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_transfer;
    else if (!u32TransferCnt)
        goto exit_nu_pdma_transfer;
    else if (u32TransferCnt > NU_PDMA_MAX_TXCNT)
        return _nu_pdma_transfer_chain(i32ChannID, u32DataWidth, u32AddrSrc, u32AddrDst, u32TransferCnt, u32IdleTimeout_us);

    psPdmaChann = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos];
    psPeriphCtl = &psPdmaChann->m_spPeripCtl;

    head = &PDMA->DSCT[NU_PDMA_GET_MOD_CHIDX(i32ChannID)];

    ret = nu_pdma_desc_setup(i32ChannID,
                             head,
                             u32DataWidth,
                             u32AddrSrc,
                             u32AddrDst,
                             u32TransferCnt,
                             RT_NULL,
                             0);
    if (ret != RT_EOK)
        goto exit_nu_pdma_transfer;

    _nu_pdma_transfer(i32ChannID, psPeriphCtl->m_u32Peripheral, head, u32IdleTimeout_us);

    ret = RT_EOK;

exit_nu_pdma_transfer:

    return -(ret);
}

rt_err_t nu_pdma_sg_transfer(int i32ChannID, nu_pdma_desc_t head, uint32_t u32IdleTimeout_us)
{
    rt_err_t ret = RT_EINVAL;
    nu_pdma_periph_ctl_t *psPeriphCtl = NULL;

    if (!head)
        goto exit_nu_pdma_sg_transfer;
    else if (nu_pdma_check_is_nonallocated(i32ChannID))
        goto exit_nu_pdma_sg_transfer;

    psPeriphCtl = &nu_pdma_chn_arr[i32ChannID - NU_PDMA_CH_Pos].m_spPeripCtl;

    _nu_pdma_transfer(i32ChannID, psPeriphCtl->m_u32Peripheral, head, u32IdleTimeout_us);

    ret = RT_EOK;

exit_nu_pdma_sg_transfer:

    return -(ret);
}

static void nu_pdma_isr(int vector, void *pvdata)
{
    int i;
    PDMA_T *PDMA = (void *)pvdata;

    uint32_t intsts = PDMA_GET_INT_STATUS(PDMA);
    uint32_t abtsts = PDMA_GET_ABORT_STS(PDMA);
    uint32_t tdsts  = PDMA_GET_TD_STS(PDMA);
    uint32_t unalignsts  = PDMA_GET_ALIGN_STS(PDMA);
    uint32_t reqto  = intsts & PDMA_INTSTS_REQTOFn_Msk;
    uint32_t reqto_ch = (reqto >> PDMA_INTSTS_REQTOFn_Pos);

    int allch_sts = (reqto_ch | tdsts | abtsts | unalignsts);

    // Abort
    if (intsts & PDMA_INTSTS_ABTIF_Msk)
    {
        // Clear all Abort flags
        PDMA_CLR_ABORT_FLAG(PDMA, abtsts);
    }

    // Transfer done
    if (intsts & PDMA_INTSTS_TDIF_Msk)
    {
        // Clear all transfer done flags
        PDMA_CLR_TD_FLAG(PDMA, tdsts);
    }

    // Unaligned
    if (intsts & PDMA_INTSTS_ALIGNF_Msk)
    {
        // Clear all Unaligned flags
        PDMA_CLR_ALIGN_FLAG(PDMA, unalignsts);
    }

    // Timeout
    if (reqto)
    {
        // Clear all Timeout flags
        PDMA->INTSTS = reqto;
    }

    // Find the position of first '1' in allch_sts.
    while ((i = nu_ctz(allch_sts)) < PDMA_CH_MAX)
    {
        int module_id = ((uint32_t)PDMA - DEF_PDMA_BASE_START) / 0x10000UL;
        int j = i + (module_id * PDMA_CH_MAX);
        int ch_mask = (1 << i);

        if (nu_pdma_chn_mask_arr[module_id] & ch_mask)
        {
            int ch_event = 0;
            nu_pdma_chn_t *dma_chn = nu_pdma_chn_arr + j - NU_PDMA_CH_Pos;

            if (dma_chn->m_sCB_Event.m_pfnCBHandler)
            {
                if (abtsts & ch_mask)
                {
                    ch_event |= NU_PDMA_EVENT_ABORT;
                }

                if (tdsts & ch_mask)
                {
                    ch_event |= NU_PDMA_EVENT_TRANSFER_DONE;
                }

                if (unalignsts & ch_mask)
                {
                    ch_event |= NU_PDMA_EVENT_ALIGNMENT;
                }

                if (reqto_ch & ch_mask)
                {
                    PDMA_DisableTimeout(PDMA,  ch_mask);
                    ch_event |= NU_PDMA_EVENT_TIMEOUT;
                }

                if (dma_chn->m_sCB_Disable.m_pfnCBHandler)
                    dma_chn->m_sCB_Disable.m_pfnCBHandler(dma_chn->m_sCB_Disable.m_pvUserData, dma_chn->m_sCB_Disable.m_u32Reserved);

                if ((dma_chn->m_u32EventFilter & ch_event) && dma_chn->m_sCB_Event.m_pfnCBHandler)
                    dma_chn->m_sCB_Event.m_pfnCBHandler(dma_chn->m_sCB_Event.m_pvUserData, ch_event);

                if (reqto_ch & ch_mask)
                    nu_pdma_timeout_set(j, nu_pdma_chn_arr[j - NU_PDMA_CH_Pos].m_u32IdleTimeout_us);

            }//if(dma_chn->handler)

        } //if (nu_pdma_chn_mask & ch_mask)

        // Clear the served bit.
        allch_sts &= ~ch_mask;

    } //while
}

static void nu_pdma_memfun_actor_init(void)
{
    int i = 0 ;
    nu_pdma_init();
    for (i = 0; i < NU_PDMA_MEMFUN_ACTOR_MAX; i++)
    {
        rt_memset(&nu_pdma_memfun_actor_arr[i], 0, sizeof(struct nu_pdma_memfun_actor));
        if (-(RT_ERROR) != (nu_pdma_memfun_actor_arr[i].m_i32ChannID = nu_pdma_channel_allocate(PDMA_MEM)))
        {
            nu_pdma_memfun_actor_arr[i].m_psSemMemFun = rt_sem_create("memactor_sem", 0, RT_IPC_FLAG_FIFO);
            RT_ASSERT(nu_pdma_memfun_actor_arr[i].m_psSemMemFun != RT_NULL);
        }
        else
            break;
    }
    if (i)
    {
        nu_pdma_memfun_actor_maxnum = i;
        nu_pdma_memfun_actor_mask = ~(((1 << i) - 1));

        nu_pdma_memfun_actor_pool_sem = rt_sem_create("mempool_sem", nu_pdma_memfun_actor_maxnum, RT_IPC_FLAG_FIFO);
        RT_ASSERT(nu_pdma_memfun_actor_pool_sem != RT_NULL);

        nu_pdma_memfun_actor_pool_lock = rt_mutex_create("mempool_lock", RT_IPC_FLAG_PRIO);
        RT_ASSERT(nu_pdma_memfun_actor_pool_lock != RT_NULL);
    }
}

static void nu_pdma_memfun_cb(void *pvUserData, uint32_t u32Events)
{
    rt_err_t result = RT_EOK;

    nu_pdma_memfun_actor_t psMemFunActor = (nu_pdma_memfun_actor_t)pvUserData;
    psMemFunActor->m_u32Result = u32Events;

    result = rt_sem_release(psMemFunActor->m_psSemMemFun);
    RT_ASSERT(result == RT_EOK);
}

static int nu_pdma_memfun_employ(void)
{
    int idx = -1 ;
    rt_err_t result = RT_EOK;

    /* Headhunter */
    if (nu_pdma_memfun_actor_pool_sem &&
            ((result = rt_sem_take(nu_pdma_memfun_actor_pool_sem, RT_WAITING_FOREVER)) == RT_EOK))
    {
        RT_ASSERT(result == RT_EOK);

        result = rt_mutex_take(nu_pdma_memfun_actor_pool_lock, RT_WAITING_FOREVER);
        RT_ASSERT(result == RT_EOK);

        /* Find the position of first '0' in nu_pdma_memfun_actor_mask. */
        idx = nu_cto(nu_pdma_memfun_actor_mask);
        if (idx != 32)
        {
            nu_pdma_memfun_actor_mask |= (1 << idx);
        }
        else
        {
            idx = -1;
        }
        result = rt_mutex_release(nu_pdma_memfun_actor_pool_lock);
        RT_ASSERT(result == RT_EOK);
    }

    return idx;
}

static rt_ssize_t nu_pdma_memfun(void *dest, void *src, uint32_t u32DataWidth, unsigned int u32TransferCnt, nu_pdma_memctrl_t eMemCtl)
{
    nu_pdma_memfun_actor_t psMemFunActor = NULL;
    struct nu_pdma_chn_cb sChnCB;
    rt_err_t result = RT_ERROR;

    int idx;
    rt_size_t ret = 0;

    /* Employ actor */
    while ((idx = nu_pdma_memfun_employ()) < 0);

    psMemFunActor = &nu_pdma_memfun_actor_arr[idx];

    /* Set PDMA memory control to eMemCtl. */
    nu_pdma_channel_memctrl_set(psMemFunActor->m_i32ChannID, eMemCtl);

    /* Register ISR callback function */
    sChnCB.m_eCBType = eCBType_Event;
    sChnCB.m_pfnCBHandler = nu_pdma_memfun_cb;
    sChnCB.m_pvUserData = (void *)psMemFunActor;

    nu_pdma_filtering_set(psMemFunActor->m_i32ChannID, NU_PDMA_EVENT_ABORT | NU_PDMA_EVENT_TRANSFER_DONE);
    nu_pdma_callback_register(psMemFunActor->m_i32ChannID, &sChnCB);

    psMemFunActor->m_u32Result = 0;

    /* Trigger it */
    nu_pdma_transfer(psMemFunActor->m_i32ChannID,
                     u32DataWidth,
                     (uint32_t)src,
                     (uint32_t)dest,
                     u32TransferCnt,
                     0);

    /* Wait it done. */
    result = rt_sem_take(psMemFunActor->m_psSemMemFun, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    /* Give result if get NU_PDMA_EVENT_TRANSFER_DONE.*/
    if (psMemFunActor->m_u32Result & NU_PDMA_EVENT_TRANSFER_DONE)
    {
        ret +=  u32TransferCnt;
    }
    else
    {
        ret += (u32TransferCnt - nu_pdma_non_transfer_count_get(psMemFunActor->m_i32ChannID));
    }

    /* Terminate it if get ABORT event */
    if (psMemFunActor->m_u32Result & NU_PDMA_EVENT_ABORT)
    {
        nu_pdma_channel_terminate(psMemFunActor->m_i32ChannID);
    }

    result = rt_mutex_take(nu_pdma_memfun_actor_pool_lock, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    nu_pdma_memfun_actor_mask &= ~(1 << idx);

    result = rt_mutex_release(nu_pdma_memfun_actor_pool_lock);
    RT_ASSERT(result == RT_EOK);

    /* Fire actor */
    result = rt_sem_release(nu_pdma_memfun_actor_pool_sem);
    RT_ASSERT(result == RT_EOK);

    return ret;
}

rt_size_t nu_pdma_mempush(void *dest, void *src, uint32_t data_width, unsigned int transfer_count)
{
    if (data_width == 8 || data_width == 16 || data_width == 32)
        return nu_pdma_memfun(dest, src, data_width, transfer_count, eMemCtl_SrcInc_DstFix);
    return 0;
}

void *nu_pdma_memcpy(void *dest, void *src, unsigned int count)
{
    int i = 0;
    uint32_t u32Offset = 0;
    uint32_t u32Remaining = count;

    for (i = 4; (i > 0) && (u32Remaining > 0) ; i >>= 1)
    {
        uint32_t u32src   = (uint32_t)src + u32Offset;
        uint32_t u32dest  = (uint32_t)dest + u32Offset;

        if (((u32src % i) == (u32dest % i)) &&
                ((u32src % i) == 0) &&
                (RT_ALIGN_DOWN(u32Remaining, i) >= i))
        {
            uint32_t u32TXCnt = u32Remaining / i;
            if (u32TXCnt != nu_pdma_memfun((void *)u32dest, (void *)u32src, i * 8, u32TXCnt, eMemCtl_SrcInc_DstInc))
                goto exit_nu_pdma_memcpy;

            u32Offset += (u32TXCnt * i);
            u32Remaining -= (u32TXCnt * i);
        }
    }

    if (count == u32Offset)
        return dest;

exit_nu_pdma_memcpy:

    return NULL;
}

/**
 * PDMA memfun actor initialization
 */
int rt_hw_pdma_memfun_init(void)
{
    nu_pdma_memfun_actor_init();
    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_pdma_memfun_init);
#endif // #if defined(BSP_USING_PDMA)
