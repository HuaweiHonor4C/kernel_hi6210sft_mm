/*
 * arch/arm/mach-k3v2/balong_power.c
 *
 * Copyright (C) 2011 Hisilicon Co. Ltd.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "hi6xxx-power.h"
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/kernel.h> /* string operation */
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include "drv_global.h"
/*#include <linux/hisi/reset.h>*/
#include "soc_modem_sctrl_interface.h"
#include "soc_ao_sctrl_interface.h"
#include <linux/hisi/hi6xxx-iomap.h>                   /* For IO_ADDRESS access */
#include <linux/io.h>
#include "../reset/reset_sub_ccore.h"


extern int simulate_irq_c_reset(int is_normal); /* MODEM reset alone has no H file  */
extern BSP_VOID BSP_ICC_ResetFun(void);/*reset icc*/
extern int ccorereset_tonotify(DRV_RESET_CB_MOMENT_E eparam, ereset_module emodule);
extern int ccorereset_doreset(void);

extern sreset_mgr_assistant_ccore   g_reset_assistant ;


static struct balong_power_plat_data* balong_driver_plat_data = NULL;
static void __iomem *hi6xxx_power_mstrl      = NULL; /*for modem sc register*/

static struct workqueue_struct *workqueue;
static struct work_struct modem_reset_work;
static const char* const modem_state_str[] = {
    "MODEM_STATE_OFF",
    "MODEM_STATE_READY",
    "MODEM_STATE_INVALID",
};
static int is_modem_normal_reset = 0; /*0: normal, 1: exception */

/*To make modem poweroff called only once when there are two rilds.*/
static int modem_power_off_flag = 0;

/*****************************************************************************
 函 数 名  : balong_power_reset_work
 功能描述  : 调用C核单独复位函数，执行单独复位操作
 输入参数  : struct work_struct *work
 输出参数  : 无
 返 回 值  : static void
 调用函数  :
 被调函数  :

 修改历史      :
  1.日    期   : 2013年5月25日
    作    者   : 陈易超
    修改内容   : 新生成函数

*****************************************************************************/
static void balong_power_reset_work(struct work_struct *work)
{
    /*modem single reset function to be added */
    static int modem = 0;
    simulate_irq_c_reset(is_modem_normal_reset);
    pr_err("Balong_power : reset %d times.\n", modem);
    modem++;
}

static void modem_wdt_disable(void)
{
    pr_info("Balong_power : modem_wdt_disable.\n");
    /*关闭C核狗的时钟*/
    if((get_domain_access_status(ACCESS_DOMAIN_MODEM_SC) == 1)&&(NULL != hi6xxx_power_mstrl) )
    {
        writel(BIT(SOC_MODEM_SCTRL_MDMSC_AO_CLKDIS_ao_peri_clkdis_watchdog0_pclk_START),
                  SOC_MODEM_SCTRL_MDMSC_AO_CLKDIS_ADDR(hi6xxx_power_mstrl));
    }
}

/*****************************************************************************
 函 数 名  : modem_reset_set
 功能描述  : 设备节点写函数
 输入参数  : struct device *dev
             struct device_attribute *attr
             const char *buf
             size_t count
 输出参数  : 无
 返 回 值  : static ssize_t
 调用函数  :
 被调函数  :

 修改历史      :
  1.日    期   : 2013年5月25日
    作    者   : 陈易超
    修改内容   : 新生成函数

*****************************************************************************/
static ssize_t modem_reset_set(struct device *dev,
                 struct device_attribute *attr,
                 const char *buf, size_t count)
{
    int state;
    char* endp;
    dev_info(dev, "Power set to %s\n", buf);
    state = simple_strtol(buf, &endp, 10); /*10 means read as dec*/
    pr_err("endp = 0x%x\n", *(int*)endp);

    if (*buf == '\0' || *buf == *endp)/* return 0 means match failed */
        return count;
    if (state == BALONG_MODEM_RESET) {
        is_modem_normal_reset = 1;
        queue_work(workqueue, &modem_reset_work);
        pr_err("Balong_power : send uevent modem_reset to workqueue !!!!\n");
        return count;
    } else if(state == BALONG_MODEM_NORMAL_RESET) {
        is_modem_normal_reset = 0;
        queue_work(workqueue, &modem_reset_work);
        pr_err("Balong_power : send uevent modem_reset to workqueue !!!!\n");
        return count;
    } else if(state == POWER_SET_OFF) {
        /*To make modem poweroff called only once when there are two rilds.*/
        if(modem_power_off_flag ) {
            pr_info("Balong_power: modem power off has been called! \n");
            return count;
        }
        modem_power_off_flag = 1;
        pr_info("Balong_power:  Modem POWER OFF... \n");

        /*发送消息给MCU*/
        g_reset_assistant.smailboxmsg_mcu.iproctec = BSP_RESET_MAILBOX_MSG_PROTECT;
        g_reset_assistant.smailboxmsg_mcu.idata = BSP_RESET_MCU_MSG_CCORE_RESET_BEFORE;
        printk(KERN_INFO"RESET LOG: Send mail to mcu \n");
        ccorereset_tonotify(MDRV_RESET_CB_BEFORE, BSP_RESET_MODULE_MCU);

        /*等待收到MCU回复消息*/
        if (0 != down_timeout(&(g_reset_assistant.sem_wait_mcu_msg),
            msecs_to_jiffies(RESET_WAIT_TIMEOUT_MCU_MAILMSG)))
        {
            pr_err("Balong_power : Wait mailbox msg from mcu timeout !!!\n");
        }

        /*Reset icc;*/
        BSP_ICC_ResetFun();

        modem_wdt_disable();

        /*Reset c core*/
        ccorereset_doreset();

        return count;
    } else {
        pr_err("Balong_power : invalid code to balong power !!!!\n");
        return count;
    }
}

/*****************************************************************************
 函 数 名  : modem_state_get
 功能描述  : get modem state
 输入参数  : void
 输出参数  : 无
 返 回 值  : <0     failed
             >=0    success
 调用函数  :
 被调函数  :

 修改历史      :
  1.日    期   : 2013年5月25日
    作    者   : 陈易超
    修改内容   : 新生成函数

*****************************************************************************/
static int modem_state_get(void)
{
    if (!balong_driver_plat_data){
        pr_err("Balong_power %s:%d not init.\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }
    return (int)balong_driver_plat_data->modem_state;
}

/*****************************************************************************
 函 数 名  : modem_state_set
 功能描述  : 提供ttf设置modem状态的API
 输入参数  : unsigned int state
 输出参数  : 无
 返 回 值  : <0     failed,invalid state
             =0     success
 调用函数  :
 被调函数  :

 修改历史      :
  1.日    期   : 2013年5月25日
    作    者   : 陈易超
    修改内容   : 新生成函数

*****************************************************************************/
int modem_state_set(unsigned int state)
{
    if (!balong_driver_plat_data){
        pr_err("Balong_power %s:%d not init.\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }
    if (state >= MODEM_INVALID){
        pr_err("Balong_power %s:%d invalid state 0x%x.\n", __FUNCTION__, __LINE__, state);
        return -EINVAL;
    }
    balong_driver_plat_data->modem_state = state;
    return 0;
}

int mdrv_set_modem_state(unsigned int state)
{
    return modem_state_set(state);
}
/*****************************************************************************
 函 数 名  : balong_power_get
 功能描述  : 设备节点modem_state 的读函数
 输入参数  : struct device *dev
             struct device_attribute *attr
             char *buf
 输出参数  : 无
 返 回 值  : static ssize_t
 调用函数  :
 被调函数  :

 修改历史      :
  1.日    期   : 2013年5月25日
    作    者   : 陈易超
    修改内容   : 新生成函数

*****************************************************************************/
static ssize_t balong_power_get(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    int state;
    ssize_t len;
    state = modem_state_get();
    if (state < 0) {
        pr_err("Balong_power %s:%d not init.\n", __FUNCTION__, __LINE__);
        return 0;
    }
    if (balong_driver_plat_data->modem_state >= MODEM_INVALID){
        pr_err("Balong_power : %s:%d Invalid state 0x%x now is set.\n", __FUNCTION__, __LINE__, state);
        return 0;
    }
    len = sprintf(buf, "%s\n", modem_state_str[balong_driver_plat_data->modem_state]);
    pr_err("Current state is [%d]\n", state);
    return len;
}
static DEVICE_ATTR(state, ( S_IWUSR | S_IWGRP), NULL, modem_reset_set);
static DEVICE_ATTR(modem_state, S_IRUGO , balong_power_get, NULL);

static int balong_power_probe(struct platform_device *pdev)
{
    int status = -1;

    dev_dbg(&pdev->dev, "balong_power_probe\n");
    hi6xxx_power_mstrl = ioremap(SOC_MODEM_SCTRL_BASE_ADDR,SZ_4K);
    if (NULL == hi6xxx_power_mstrl)
    {
         dev_err(&pdev->dev, "hi6xxx_power_mstrl remap error\n");;
    }

    balong_driver_plat_data = pdev->dev.platform_data;

    /* sysfs entries for IO control */
    status = device_create_file(&(pdev->dev), &dev_attr_state);
    if (status) {
        dev_err(&pdev->dev, "Failed to create sysfs entry\n");
        return status;
    }
    status = device_create_file(&(pdev->dev), &dev_attr_modem_state);
    if (status) {
        dev_err(&pdev->dev, "Failed to create sysfs entry\n");
        goto error_single;
    }
    /* Initialize works */
    workqueue = create_singlethread_workqueue("balong_power_workqueue");
    if (!workqueue) {
        dev_err(&pdev->dev, "Create workqueue failed\n");
        status = -1;
        goto error;
    }

    INIT_WORK(&modem_reset_work, balong_power_reset_work);
    return 0;
error_single:
    device_remove_file(&(pdev->dev), &dev_attr_state);
    return status;
error:
    device_remove_file(&(pdev->dev), &dev_attr_state);
    device_remove_file(&(pdev->dev), &dev_attr_modem_state);
    return status;
}

static int balong_power_remove(struct platform_device *pdev)
{
    dev_dbg(&pdev->dev, "balong_power_remove\n");
    destroy_workqueue(workqueue);
    device_remove_file(&(pdev->dev), &dev_attr_state);
    device_remove_file(&(pdev->dev), &dev_attr_modem_state);

    return 0;
}

static struct platform_driver balong_power_driver = {
    .probe = balong_power_probe,
    .remove = balong_power_remove,
    .driver = {
        .name ="balong_power",
        .owner = THIS_MODULE,
    },
};
static int __init balong_power_init(void)
{
    int ret = 0;

    ret = platform_driver_register(&balong_power_driver);
    if (ret) {
        pr_err("%s:init failed!\n", __func__);
    }

    return ret;
}

static void __exit balong_power_exit(void)
{
    platform_driver_unregister(&balong_power_driver);
}

static struct balong_power_plat_data  balong_plat_data = {
    .modem_state = MODEM_NOT_READY,
};

static struct platform_device balong_power_device = {
	.name		= "balong_power",
	.id = -1,
	.dev = {
		.platform_data = &balong_plat_data,
	},
};
static int __init balong_power_dev_init(void)
{
    int ret = 0;
    ret = platform_device_register(&balong_power_device);
    return ret;
}

arch_initcall(balong_power_dev_init);
module_init(balong_power_init);
module_exit(balong_power_exit);
MODULE_LICENSE("GPL");
