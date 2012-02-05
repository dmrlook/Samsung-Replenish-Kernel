/*
 *  H2W device detection driver.
 *
 * Copyright (C) 2008 SAMSUNG Corporation.
 * Copyright (C) 2008 Google, Inc.
 *
 * Authors: 
 *  Laurence Chen <Laurence_Chen@htc.com>
 *  Nick Pelly <npelly@google.com>
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

/*  For detecting SAMSUNG 2 Wire devices, such as wired headset.

    Logically, the H2W driver is always present, and H2W state (hi->state)
    indicates what is currently plugged into the H2W interface.

    When the headset is plugged in, CABLE_IN1 is pulled low. When the headset
    button is pressed, CABLE_IN2 is pulled low. These two lines are shared with
    the TX and RX (respectively) of UART3 - used for serial debugging.

    This headset driver keeps the CPLD configured as UART3 for as long as
    possible, so that we can do serial FIQ debugging even when the kernel is
    locked and this driver no longer runs. So it only configures the CPLD to
    GPIO while the headset is plugged in, and for 10ms during detection work.

    Unfortunately we can't leave the CPLD as UART3 while a headset is plugged
    in, UART3 is pullup on TX but the headset is pull-down, causing a 55 mA
    drain on bigfoot.

    The headset detection work involves setting CPLD to GPIO, and then pulling
    CABLE_IN1 high with a stronger pullup than usual. A H2W headset will still
    pull this line low, whereas other attachments such as a serial console
    would get pulled up by this stronger pullup.

    Headset insertion/removal causes UEvent's to be sent, and
    /sys/class/switch/h2w/state to be updated.

    Button presses are interpreted as input event (KEY_MEDIA). Button presses
    are ignored if the headset is plugged in, so the buttons on 11 pin -> 3.5mm
    jack adapters do not work until a headset is plugged into the adapter. This
    is to avoid serial RX traffic causing spurious button press events.

    We tend to check the status of CABLE_IN1 a few more times than strictly
    necessary during headset detection, to avoid spurious headset insertion
    events caused by serial debugger TX traffic.
*/

#include <linux/module.h>
#include <linux/sysdev.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/switch.h>
#include <linux/input.h>
#include <linux/debugfs.h>
#include <asm/gpio.h>
#include <asm/atomic.h>
#include <mach/board.h>
#include <mach/vreg.h>
#include <mach/pmic.h>
#include <linux/slab.h>
#include <linux/gpio.h>

#include <mach/hardware.h>

#define DET_CHECK_TIME_MS	200 // 200ms

/* PROJECT SPECIFICATION START
*/
#if defined(CONFIG_MACH_ROOKIE) || defined(CONFIG_MACH_GIO) || defined(CONFIG_MACH_ESCAPE)
#define GPIO_DETECT_HEADSET 		39
#define GPIO_EAR_LDO_EN     93
#define FEATURE_MAX8899_PMIC_SENDEND
#endif

#if defined(CONFIG_MACH_ROOKIE) || defined(CONFIG_MACH_GIO)
#define GPIO_POPUP_SW_EN	78 // only rookie uses popup_switch
#endif

#if !defined(CONFIG_MACH_ROOKIE)
#define ADC_CHECK // 3P,4P detect & remote (+,-) detecti
#endif

/* PROJECT SPECIFICATION END
*/

#ifdef ADC_CHECK

#include "./proc_comm.h"
#define SMEM_PROC_COMM_GET_ADC			PCOM_OEM_SAMSUNG_GET_ADC
// SMEM_PROC_COMM_GET_ADC data 1

enum{
	SMEM_PROC_COMM_GET_ADC_BATTERY = 0x0,
	SMEM_PROC_COMM_GET_ADC_TEMP,
	SMEM_PROC_COMM_GET_ADC_VF,
	SMEM_PROC_COMM_GET_ADC_ALL, 
	SMEM_PROC_COMM_GET_ADC_EAR_ADC,		// 3.5PI_ADC
};

static int get_ear_adc(void)
{
	int ret = 0;
	int data1 = SMEM_PROC_COMM_GET_ADC_EAR_ADC;	
	int data2 = 0;

	ret = msm_proc_comm(SMEM_PROC_COMM_GET_ADC, &data1, &data2);
	if( ret < 0)
	{
		printk(KERN_ERR "[H2W] %s :%d fail %d\n", __func__, data1, ret);
		return ret;
	}
	return data1;
}
#endif


static u64 pressed_jiffies;
static u64 irq_jiffies;
#define SHORTKEY_MS			120
#define SHORTKEY_JIFFIES	((HZ / 10) * (SHORTKEY_MS / 100)) + (HZ / 100) * ((SHORTKEY_MS % 100) / 10)


#ifndef FEATURE_MAX8899_PMIC_SENDEND
#define GPIO_SEND_END 		1
#endif

#define CONFIG_DEBUG_H2W

#ifdef CONFIG_DEBUG_H2W
#define H2W_DBG(fmt, arg...) printk(KERN_INFO "[H2W] %s " fmt "\r\n", __func__, ## arg)
#else
#define H2W_DBG(fmt, arg...) do {} while (0)
#endif

static struct workqueue_struct *g_detection_work_queue;
static void detection_work(struct work_struct *work);
static DECLARE_WORK(g_detection_work, detection_work);

// for DFMS test
static struct workqueue_struct *g_earbutton_work_queue;
static void earbutton_work(struct work_struct *work);
static DECLARE_WORK(g_earbutton_work, earbutton_work);
static unsigned int earbutton_pressed = 0;

static void button_pressed(void);
static void button_released(void);

#define NO_DEVICE		(0)
#define BIT_HEADSET			(1 << 0)
#define BIT_HEADSET_NO_MIC	(1 << 1)

enum {
	H2W_NO_DEVICE	= 0,
	H2W_NORMAL_HEADSET = 1, 
};


enum {
	BUTTON_RELEASE	= 0,
	BUTTON_PRESS	= 1,
};

struct h2w_info {
	struct switch_dev sdev;
	struct switch_dev button_sdev;	// PBA test

	struct input_dev *input;
	struct mutex mutex_lock;

	atomic_t btn_state;
	int ignore_btn;

	unsigned int irq;
	unsigned int irq_btn;
	int btn_11pin_35mm_flag;

	struct hrtimer timer;
	ktime_t debounce_time;
	int headset_state;

	struct hrtimer btn_timer;
	ktime_t btn_debounce_time;
	int button_state;
		
	struct hrtimer irq_delay_timer;
	ktime_t irq_delay_time;	
	int btn_irq_available;

	int pressed_btn;

	unsigned int use_irq : 1;
};
static struct h2w_info *hi;

static ssize_t h2w_print_name(struct switch_dev *sdev, char *buf)
{
	switch (switch_get_state(&hi->sdev)) {
	case NO_DEVICE:
		return sprintf(buf, "No Device\n");
	case BIT_HEADSET:
	case BIT_HEADSET_NO_MIC:
		return sprintf(buf, "H2W_NORMAL_HEADSET\n");
	}
	return -EINVAL;
}
static ssize_t h2w_set_state(struct switch_dev *sdev, char *buf)
{
	int state = *buf - 48;
	switch(state){
	case H2W_NO_DEVICE:
		switch_set_state(&hi->sdev, H2W_NO_DEVICE);
		return 0;
	case H2W_NORMAL_HEADSET:
		switch_set_state(&hi->sdev, H2W_NORMAL_HEADSET);
		return 0;
	default:
		break;
	}
	return -EINVAL;
}


static ssize_t button_print_name(struct switch_dev *sdev, char *buf)
{
	switch (switch_get_state(&hi->button_sdev)) {
	case 0:
		return sprintf(buf, "No Input\n");
	case 1:
		return sprintf(buf, "Button Pressed\n");
	}
	return -EINVAL;
}

static void remove_headset(void)
{
	unsigned long irq_flags;

	H2W_DBG("");


#ifdef GPIO_POPUP_SW_EN
	gpio_set_value(GPIO_POPUP_SW_EN, 0);	//popup_sw_en off
#endif	
	gpio_set_value(GPIO_EAR_LDO_EN, 0);	//popup_sw_en off

//	msleep(200);

	mutex_lock(&hi->mutex_lock);
	switch_set_state(&hi->sdev, switch_get_state(&hi->sdev) & ~(BIT_HEADSET | BIT_HEADSET_NO_MIC));
	mutex_unlock(&hi->mutex_lock);
#ifndef FEATURE_MAX8899_PMIC_SENDEND
	if (hi->btn_11pin_35mm_flag) 
	{
            	set_irq_wake(hi->irq_btn, 0);
            	local_irq_save(irq_flags);
		disable_irq(hi->irq_btn);
            	local_irq_restore(irq_flags);
		hi->btn_11pin_35mm_flag = 0;
	}
#endif	
	if (atomic_read(&hi->btn_state)) button_released();
	hrtimer_cancel(&hi->irq_delay_timer);
	hi->btn_irq_available = 0;

	hi->debounce_time = ktime_set(0, 200000000);  /* 100 ms */
#ifdef GPIO_POPUP_SW_EN
	H2W_DBG(" POPUP SWITCH = %d, LDO = %d ",gpio_get_value(GPIO_POPUP_SW_EN), 
						gpio_get_value(GPIO_EAR_LDO_EN));
#endif
}

static void insert_headset(void)
{
	unsigned long irq_flags;
	int state;

	H2W_DBG("");
#ifdef GPIO_POPUP_SW_EN	
	gpio_set_value(GPIO_POPUP_SW_EN, 1);	//popup_sw_en on
#endif	
	gpio_set_value(GPIO_EAR_LDO_EN, 1);	//popup_sw_en off

	state = BIT_HEADSET | BIT_HEADSET_NO_MIC;

	state = switch_get_state(&hi->sdev);
	state &= ~(BIT_HEADSET_NO_MIC | BIT_HEADSET);

	/* Wait pin be stable */
	msleep(200);

#ifdef ADC_CHECK
	if( get_ear_adc() > 63)
		state |= BIT_HEADSET;
	else
		state |= BIT_HEADSET_NO_MIC;
#else
	state |= BIT_HEADSET;
#endif

	if( state == BIT_HEADSET_NO_MIC )
	{
#ifdef GPIO_POPUP_SW_EN	
	gpio_set_value(GPIO_POPUP_SW_EN, 0);	//popup_sw_en on
#endif	
	gpio_set_value(GPIO_EAR_LDO_EN, 0);	//popup_sw_en off
	}
/*
#if defined(CONFIG_MACH_GIO)
	if( state == BIT_HEADSET )
	{
#ifdef GPIO_POPUP_SW_EN	
		gpio_set_value(GPIO_POPUP_SW_EN, 1);
#endif	
		gpio_set_value(GPIO_EAR_LDO_EN, 0);
	}
#endif
*/

	mutex_lock(&hi->mutex_lock);
	switch_set_state(&hi->sdev, state);
	mutex_unlock(&hi->mutex_lock);
#ifdef ADC_CHECK
	H2W_DBG(" Insert EarJack ADC =  %d",get_ear_adc());
	H2W_DBG(" EarJack State - 4POLE => 1 3POLE =>2 : %d ", state);
#endif

#ifndef FEATURE_MAX8899_PMIC_SENDEND
	/* Enable button irq */
	if (!hi->btn_11pin_35mm_flag) 
	{
		set_irq_type(hi->irq_btn,IRQF_TRIGGER_LOW);		// detect high
		set_irq_wake(hi->irq_btn, 1);

		local_irq_save(irq_flags);
		enable_irq(hi->irq_btn);
		local_irq_restore(irq_flags);

		hi->btn_11pin_35mm_flag = 1;
	}
#endif	
	hrtimer_start(&hi->irq_delay_timer, hi->irq_delay_time, HRTIMER_MODE_REL);

	printk(KERN_INFO "11pin_3.5mm with microphone\n");

	hi->debounce_time = ktime_set(0, 20000000);  /* 500 -> 20 ms */
#ifdef GPIO_POPUP_SW_EN
	H2W_DBG(" POPUP SWITCH = %d, LDO = %d ",gpio_get_value(GPIO_POPUP_SW_EN), 
						gpio_get_value(GPIO_EAR_LDO_EN));
#endif
}

static int is_accessary_pluged_in(void)
{
	int type = 0;
	int jack_state = 0;
	
	jack_state = gpio_get_value(GPIO_DETECT_HEADSET);

	printk("[H2W] jack_state = %d\n",jack_state);

	if ((jack_state == 1))		type = H2W_NORMAL_HEADSET;
	else if ( jack_state == 0 )	type = H2W_NO_DEVICE;
	else 					type = H2W_NO_DEVICE;

	return type;
}

// for PBA test
static void earbutton_work(struct work_struct *work)
{
	mutex_lock(&hi->mutex_lock);
	switch_set_state(&hi->button_sdev, earbutton_pressed);
	mutex_unlock(&hi->mutex_lock);
}


static void detection_work(struct work_struct *work)
{
	unsigned long irq_flags;
	int jack_state;
	int type;

	H2W_DBG("");
	
	if (gpio_get_value(GPIO_DETECT_HEADSET) != 0)       
	{
		// Headset not plugged in
		if (switch_get_state(&hi->sdev) != H2W_NO_DEVICE)
			remove_headset();
		return;
	}

	/* Something plugged in, lets make sure its a headset */

	/* Disable headset interrupt while detecting.*/
	local_irq_save(irq_flags);
	disable_irq(hi->irq);
	local_irq_restore(irq_flags);
	
	/* Something plugged in, lets make sure its a headset */
	type = is_accessary_pluged_in();

	/* Restore IRQs */
	local_irq_save(irq_flags);
	enable_irq(hi->irq);
	local_irq_restore(irq_flags);

	jack_state = gpio_get_value(GPIO_DETECT_HEADSET);

    if ( gpio_get_value(GPIO_DETECT_HEADSET) == 0 )
	{
		if (switch_get_state(&hi->sdev) == H2W_NO_DEVICE)
		{
			insert_headset();
		}
		else
		{
			printk("GPIO_DETECT_HEADSET = 1, but not insert_headset\n\n");
		}
	} 
	else 
	{
		printk("JACK_S_35 was low, but not a headset " "(recent jack_state = %d)", jack_state);
	}
}


void headset_button_event(int is_press)
{
	if (!is_press) {
		if (hi->ignore_btn)
			hi->ignore_btn = 0;
		else if (atomic_read(&hi->btn_state))
			button_released();
	} else {
		if (!hi->ignore_btn && !atomic_read(&hi->btn_state))
			button_pressed();
	}
}
static enum hrtimer_restart button_event_timer_func(struct hrtimer *data)
{
	H2W_DBG("");
	// check again
#ifndef FEATURE_MAX8899_PMIC_SENDEND
	if(hi->button_state != gpio_get_value(GPIO_SEND_END))
	{
		printk("ERROR - button value : %d -> %d\n", hi->button_state, gpio_get_value(GPIO_SEND_END));
		return HRTIMER_NORESTART;
	}

    // butten active high	
        if (gpio_get_value(GPIO_SEND_END)) 
	{
		headset_button_event(1);
		/* 10 ms */
		hi->btn_debounce_time = ktime_set(0, 10000000);
	} 
	else 
	{
		headset_button_event(0);
		/* 100 ms */
		hi->btn_debounce_time = ktime_set(0, 150000000);
	}
#else
	if(hi->button_state != switch_get_state(&hi->button_sdev))
	{
		printk("ERROR - button value : %d -> %d\n", hi->button_state, switch_get_state(&hi->button_sdev));
		return HRTIMER_NORESTART;
	}

    // butten active high	
        if (switch_get_state(&hi->button_sdev)) 
	{
		headset_button_event(1);
		/* 10 ms */
		hi->btn_debounce_time = ktime_set(0, 10000000);
	} 
	else 
	{
		headset_button_event(0);
		/* 100 ms */
		//hi->btn_debounce_time = ktime_set(0, 150000000);
		hi->btn_debounce_time = ktime_set(0, 120000000); // 120 ms 
	}
#endif
	return HRTIMER_NORESTART;
}
static enum hrtimer_restart irq_delay_timer_func(struct hrtimer *data)
{
	hi->btn_irq_available = 1;
	return HRTIMER_NORESTART;
}

static enum hrtimer_restart detect_event_timer_func(struct hrtimer *data)
{
	H2W_DBG("");

	queue_work(g_detection_work_queue, &g_detection_work);
	return HRTIMER_NORESTART;
}

static irqreturn_t detect_irq_handler(int irq, void *dev_id)
{
	//int value1, value2;
	int value;
	//int retry_limit = 10;
	int time_left_ms = DET_CHECK_TIME_MS;

	H2W_DBG("");
	
	// ESCAPE Detection issue fix

	set_irq_type(hi->irq_btn, IRQF_TRIGGER_RISING);	  // button detect high
	while ( time_left_ms > 0)
	{
		if( (value = gpio_get_value(GPIO_DETECT_HEADSET)) != 0 ) //detect high
		{
			H2W_DBG(" DETECT LOW ");
			if ((switch_get_state(&hi->sdev) == H2W_NO_DEVICE) ^ (value ^ 0))
			{
				H2W_DBG(" REMOVE_HEADSET ");
				remove_headset();
			}
			
			return IRQ_HANDLED;
		}
		msleep(10);
		time_left_ms -= 10;
	}
	
	if ((switch_get_state(&hi->sdev) == H2W_NO_DEVICE) ^ (value ^ 0)) 
	{
		msleep(200);
		insert_headset();

		hi->headset_state = value;
		/* ESCAPE / GIO loopback E/P button 2 times pressd -> 1 time fix
		H2W_DBG("EARJACK detection.JACK_35_value = %d.", value);
		if (switch_get_state(&hi->sdev) == H2W_NORMAL_HEADSET){
			hi->ignore_btn = 1;
		}
		else hi->ignore_btn = 0;
		*/
		
	}
	

	return IRQ_HANDLED;

	// ESCAPE Detection issue fix END
	

	/*

	do {
		value1 = gpio_get_value(GPIO_DETECT_HEADSET);
		set_irq_type(hi->irq, value1 ?
				IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING);
		value2 = gpio_get_value(GPIO_DETECT_HEADSET);
	} while (value1 != value2 && retry_limit-- > 0);

	if ((switch_get_state(&hi->sdev) == H2W_NO_DEVICE) ^ value2) 
	{ 
		hi->headset_state = value1;
		H2W_DBG("EARJACK detection.JACK_35_value=%d->%d.", value1, value2);
		if (switch_get_state(&hi->sdev) == H2W_NORMAL_HEADSET){
			hi->ignore_btn = 1;
		}
		else hi->ignore_btn = 0;
		// Do the rest of the work in timer context 
		hrtimer_start(&hi->timer, hi->debounce_time, HRTIMER_MODE_REL);
	}
	return IRQ_HANDLED;
	*/
	
}

static void button_pressed(void)
{
	printk("button_pressed \n");
	atomic_set(&hi->btn_state, 1);
#ifdef ADC_CHECK
	input_report_key(hi->input, hi->pressed_btn, 1);
#else
	input_report_key(hi->input, KEY_SEND, 1);
#endif

	input_sync(hi->input);

	earbutton_pressed = 1;
	queue_work(g_earbutton_work_queue, &g_earbutton_work);

}

static void button_released(void)
{
	printk("button_released \n");
	atomic_set(&hi->btn_state, 0);
#ifdef ADC_CHECK
	input_report_key(hi->input, hi->pressed_btn, 0);
#else
	input_report_key(hi->input, KEY_SEND, 0);
#endif
	input_sync(hi->input);

	earbutton_pressed = 0;
	queue_work(g_earbutton_work_queue, &g_earbutton_work);

}

#ifndef FEATURE_MAX8899_PMIC_SENDEND

static irqreturn_t button_irq_handler(int irq, void *dev_id)
{
	int value1, value2;
	int retry_limit = 10;
	do {
		value1 = gpio_get_value(GPIO_SEND_END );
		set_irq_type(hi->irq_btn, value1 ?
				IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH);
		value2 = gpio_get_value(GPIO_SEND_END );
	} while (value1 != value2 && retry_limit-- > 0);

	if(value1 == value2 && retry_limit > 0)
	{
		hi->button_state = value1;
		hrtimer_start(&hi->btn_timer, hi->btn_debounce_time, HRTIMER_MODE_REL);
	}

	return IRQ_HANDLED;
}
#else
static ssize_t button_set_state(struct switch_dev *sdev, char *buf)
{
	int state = *buf - 48;
#ifdef ADC_CHECK
	int adc = get_ear_adc();
	if(switch_get_state(&hi->sdev) == BIT_HEADSET && state )
	{
	
	#if defined(CONFIG_MACH_GIO)
		if(adc >=0 && adc < 6)
			hi->pressed_btn = KEY_SEND;
		else if(adc >= 6 && adc < 16)
			hi->pressed_btn = KEY_VOLUMEUP;
		else if(adc >= 16 && adc < 40) 
			hi->pressed_btn = KEY_VOLUMEDOWN;
		H2W_DBG("EarJAck GIO Button ADC = %d", adc);
	#elif defined(CONFIG_MACH_ESCAPE)
		if(adc >= 0 && adc < 9)  
			hi->pressed_btn = KEY_SEND;
		else if(adc >= 9 && adc < 21)
			hi->pressed_btn = KEY_VOLUMEUP;
		else if(adc >= 21 && adc < 43) 
			hi->pressed_btn = KEY_VOLUMEDOWN;
		H2W_DBG("EarJAck ESCAPE Button ADC = %d", adc);
	#else
		if(adc >=0 && adc < 19)
			hi->pressed_btn = KEY_SEND;
		else if(adc >= 19 && adc < 43)
			hi->pressed_btn = KEY_VOLUMEUP;
		else if(adc >= 43 && adc < 90) 
			hi->pressed_btn = KEY_VOLUMEDOWN;
		H2W_DBG("EarJAck Button ADC = %d", adc);
	#endif
	}
#endif
	if(switch_get_state(&hi->sdev) == H2W_NO_DEVICE || hi->btn_irq_available == 0)
		return -EINVAL;
	printk(KERN_INFO "Button state: %d\n", state);
	hi->button_state = state;
       switch_set_state(&hi->button_sdev, state);	
	hrtimer_start(&hi->btn_timer, hi->btn_debounce_time, HRTIMER_MODE_REL);

	return -EINVAL;
}

#endif

#if defined(CONFIG_DEBUG_FS)
static int h2w_debug_set(void *data, u64 val)
{
	mutex_lock(&hi->mutex_lock);
	switch_set_state(&hi->sdev, (int)val);
	mutex_unlock(&hi->mutex_lock);
	return 0;
}

static int h2w_debug_get(void *data, u64 *val)
{
	return 0;
}


DEFINE_SIMPLE_ATTRIBUTE(h2w_debug_fops, h2w_debug_get, h2w_debug_set, "%llu\n");
static int __init h2w_debug_init(void)
{
	struct dentry *dent;

	dent = debugfs_create_dir("h2w", 0);
	if (IS_ERR(dent))
		return PTR_ERR(dent);

	debugfs_create_file("state", 0644, dent, NULL, &h2w_debug_fops);

	return 0;
}

device_initcall(h2w_debug_init);
#endif


static int h2w_probe(struct platform_device *pdev)
{
	int ret;

	printk(KERN_INFO "[H2W] Registering H2W (headset) driver\n");
	hi = kzalloc(sizeof(struct h2w_info), GFP_KERNEL);
	if (!hi)
		return -ENOMEM;

	atomic_set(&hi->btn_state, 0);
	hi->ignore_btn = 0;
	hi->headset_state = 0;
	hi->button_state = 0;

	hi->debounce_time = ktime_set(0, 200000000);  /* 100 ms -> 200 ms */
	hi->btn_debounce_time = ktime_set(0, 50000000); /* 50 ms */
        
	hi->btn_11pin_35mm_flag = 0;

	mutex_init(&hi->mutex_lock);

	hi->sdev.name = "h2w";
	hi->sdev.print_name = h2w_print_name;
	hi->sdev.set_state = h2w_set_state;

	ret = switch_dev_register(&hi->sdev);
	if (ret < 0)
		goto err_switch_dev_register;

	// for PBA test
	hi->button_sdev.name = "sec_earbutton";
	hi->button_sdev.print_name = button_print_name;
	hi->button_sdev.set_state = button_set_state;


	ret = switch_dev_register(&hi->button_sdev);
	if (ret < 0)
		goto err_switch_dev_register;
	switch_set_state(&hi->button_sdev, 0);
	
	g_earbutton_work_queue = create_workqueue("earbutton");
	if (g_earbutton_work_queue == NULL) {
		ret = -ENOMEM;
		goto err_create_work_queue;
	}

	g_detection_work_queue = create_workqueue("detection");
	if (g_detection_work_queue == NULL) {
		ret = -ENOMEM;
		goto err_create_work_queue;
	}

/*****************************************************************/
// JACK_S_35 GPIO setting     
/*****************************************************************/    
	ret = gpio_request(GPIO_DETECT_HEADSET, "h2w_detect");
	if (ret < 0)
		goto err_request_detect_gpio;

	ret = gpio_direction_input(GPIO_DETECT_HEADSET);
	if (ret < 0)
		goto err_set_detect_gpio;

	hi->irq = gpio_to_irq(GPIO_DETECT_HEADSET);
	if (hi->irq < 0) {
		ret = hi->irq;
		goto err_get_h2w_detect_irq_num_failed;
	}

	hrtimer_init(&hi->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hi->timer.function = detect_event_timer_func;
#ifndef FEATURE_MAX8899_PMIC_SENDEND	
/*****************************************************************/
// SEND_END GPIO setting     
/*****************************************************************/

	ret = gpio_request(GPIO_SEND_END , "h2w_button");
	if (ret < 0)
		goto err_request_button_gpio;

	ret = gpio_direction_input(GPIO_SEND_END );
	if (ret < 0)
		goto err_set_button_gpio;

	hi->irq_btn = gpio_to_irq(GPIO_SEND_END );
	if (hi->irq_btn < 0) {
		ret = hi->irq_btn;
		goto err_get_button_irq_num_failed;
	}
#endif	

	hrtimer_init(&hi->btn_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hi->btn_timer.function = button_event_timer_func;
	
#ifdef GPIO_POPUP_SW_EN	
/*****************************************************************/
//POPUP_SW_EN GPIO setting     
/*****************************************************************/
	ret = gpio_request(GPIO_POPUP_SW_EN, "h2w_popup_sw");
	if (ret < 0)
		goto err_request_popup_gpio;

	ret = gpio_direction_output(GPIO_POPUP_SW_EN, 0);
	if (ret < 0)
		goto err_set_popup_gpio;
	gpio_set_value(GPIO_POPUP_SW_EN, 0);	//popup_sw_en on
#endif	

/*****************************************************************/
//EAR_LDO_EN GPIO setting	   
/*****************************************************************/
	ret = gpio_request(GPIO_EAR_LDO_EN, "h2w_ear_ldo_sw");
	if (ret < 0)
		goto err_request_ldo_gpio;

	ret = gpio_direction_output(GPIO_EAR_LDO_EN, 0);
	if (ret < 0)
		goto err_set_ldo_gpio;
	gpio_set_value(GPIO_EAR_LDO_EN, 0);	//earvias_en on
/*****************************************************************/
// JACK_S_35 irq setting     
/*****************************************************************/       
	ret = request_threaded_irq(hi->irq, NULL, detect_irq_handler,
			  IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING | IRQF_ONESHOT , "h2w_detect", NULL);
       
	if (ret < 0)
		goto err_request_detect_irq;

      	hi->use_irq = ret == 0;

     	printk(KERN_INFO "Headset Driver: Start gpio inputs for %s in %s mode\n", hi->sdev.name, hi->use_irq ? "interrupt" : "polling");

	ret = set_irq_wake(hi->irq, 1);
	if (ret < 0)
		goto err_request_input_dev;
#ifndef FEATURE_MAX8899_PMIC_SENDEND	
/*****************************************************************/
// SEND_END irq setting     
/*****************************************************************/
    	/* Disable button until plugged in */
    	set_irq_flags(hi->irq_btn, IRQF_VALID | IRQF_NOAUTOEN);
   	ret = request_irq(hi->irq_btn, button_irq_handler, IRQF_TRIGGER_HIGH, "h2w_button", NULL);       // detect high
    	if (ret < 0)
        	goto err_request_button_irq;
#endif

	hrtimer_init(&hi->irq_delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hi->irq_delay_timer.function = irq_delay_timer_func;
	hi->irq_delay_time = ktime_set(0, 1000000000);  /* 1sec*/

	hi->input = input_allocate_device();
	if (!hi->input) {
		ret = -ENOMEM;
		goto err_request_input_dev;
	}

	hi->input->name = "sec_jack"; //"rookie-headset";	
	set_bit(EV_KEY, hi->input->evbit);
	set_bit(KEY_SEND, hi->input->keybit);
	set_bit(KEY_VOLUMEDOWN, hi->input->keybit);
	set_bit(KEY_VOLUMEUP, hi->input->keybit);
	
	ret = input_register_device(hi->input);
	if (ret < 0)
		goto err_register_input_dev;

	return 0;



err_register_input_dev:
	input_free_device(hi->input);
err_request_input_dev:
	free_irq(hi->irq, 0);
#ifndef FEATURE_MAX8899_PMIC_SENDEND	
	free_irq(hi->irq_btn, 0);
#endif
err_get_button_irq_num_failed:
err_get_h2w_detect_irq_num_failed:
err_set_button_gpio:
err_set_popup_gpio:
err_set_ldo_gpio:	
err_set_detect_gpio:
err_request_detect_irq:    
       gpio_free(GPIO_DETECT_HEADSET);
#ifndef FEATURE_MAX8899_PMIC_SENDEND	
err_request_button_gpio:
err_request_button_irq: 
	gpio_free(GPIO_SEND_END );
#endif
err_request_ldo_gpio:
	gpio_free(GPIO_EAR_LDO_EN);
err_request_popup_gpio:
#ifdef GPIO_POPUP_SW_EN
	gpio_free(GPIO_POPUP_SW_EN);
#endif

err_request_detect_gpio:
	destroy_workqueue(g_detection_work_queue);
	destroy_workqueue(g_earbutton_work_queue);
err_create_work_queue:
	switch_dev_unregister(&hi->sdev);
err_switch_dev_register:
	printk(KERN_ERR "H2W: Failed to register driver\n");

	return ret;
}

static int h2w_remove(struct platform_device *pdev)
{
	H2W_DBG("");
	if (switch_get_state(&hi->sdev))
		remove_headset();
	input_unregister_device(hi->input);
#ifndef FEATURE_MAX8899_PMIC_SENDEND
	gpio_free(GPIO_SEND_END );
	free_irq(hi->irq_btn, 0);
#endif
#ifdef GPIO_POPUP_SW_EN
	gpio_free(GPIO_POPUP_SW_EN);
#endif
	gpio_free(GPIO_EAR_LDO_EN);
	gpio_free(GPIO_DETECT_HEADSET);
	free_irq(hi->irq, 0);
	destroy_workqueue(g_detection_work_queue);
	destroy_workqueue(g_earbutton_work_queue);
	switch_dev_unregister(&hi->sdev);

	return 0;
}

static struct platform_device h2w_device = {
	.name		= "sec_jack",
};

static struct platform_driver h2w_driver = {
	.probe		= h2w_probe,
	.remove		= h2w_remove,
	.driver		= {
		.name		= "sec_jack",
		.owner		= THIS_MODULE,
	},
};

 int __init h2w_init(void)
{
	int ret;
#ifndef	FEATURE_MAX8899_PMIC_SENDEND
	H2W_DBG("JACK_S_35(%d), SEND_END(%d)", GPIO_DETECT_HEADSET, GPIO_SEND_END);
#else
	H2W_DBG("JACK_S_35(%d)", GPIO_DETECT_HEADSET);
#endif
	ret = platform_driver_register(&h2w_driver);
	if (ret)
		return ret;
	return platform_device_register(&h2w_device);
}

static void __exit h2w_exit(void)
{
	platform_device_unregister(&h2w_device);

	platform_driver_unregister(&h2w_driver);
}

module_init(h2w_init);
module_exit(h2w_exit);

MODULE_AUTHOR("anonymous <anonymous@samsung.com>");
MODULE_DESCRIPTION("SAMSUNG Headset driver for rookie");
MODULE_LICENSE("GPL");
