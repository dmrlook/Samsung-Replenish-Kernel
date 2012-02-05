/*
 *  Copyright (c) 2007, 2008 SAMSUNG, Inc
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without modification,
 *   are permitted provided that the following conditions are met: 
 *
 *   Redistributions of source code must retain the above copyright notice, this list 
 *   of conditions and the following disclaimer. 
 *   Redistributions in binary form must reproduce the above copyright notice, this 
 *   list of conditions and the following disclaimer in the documentation and/or other
 *   materials provided with the distribution. 
 *   Neither the name of the HTC,Inc nor the names of its contributors may be used 
 *   to endorse or promote products derived from this software without specific prior
 *   written permission. 
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED 
 *   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR 
 *   PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR 
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;LOSS OF USE, DATA, OR 
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY 
 *   OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,OR TORT 
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT 
 *   OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH 
 *   DAMAGE
 */

#ifndef AMP_MAX97000_H
#define AMP_MAX97000_H
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <asm/sizes.h>

/*************************************************************
*	IOCTL define
*************************************************************/

#define MAX97000_I2C_IOCTL_MAGIC 'm'

#define MAX97000_I2C_IOCTL_W \
	        _IOW(MAX97000_I2C_IOCTL_MAGIC, 0, unsigned)

#define MAX97000_I2C_IOCTL_R \
	        _IOR(MAX97000_I2C_IOCTL_MAGIC, 1, unsigned)

#define MAX97000_I2C_IOCTL_SWITCH_DEVICE \
	        _IO(MAX97000_I2C_IOCTL_MAGIC, 2)

#define MAX97000_HPH_VOL_SET \
	        _IOW(MAX97000_I2C_IOCTL_MAGIC, 3, unsigned)

#define MAX97000_SPK_VOL_SET \
	        _IOW(MAX97000_I2C_IOCTL_MAGIC, 4, unsigned)

#define MAX97000_SPEAKER_ON \
	        _IO(MAX97000_I2C_IOCTL_MAGIC, 5)

#define MAX97000_SPEAKER_OFF \
	        _IO(MAX97000_I2C_IOCTL_MAGIC, 6)
    
#define MAX97000_HEADSET_ON \
               _IOW(MAX97000_I2C_IOCTL_MAGIC, 7, unsigned)

#define MAX97000_HEADSET_OFF \
               _IO(MAX97000_I2C_IOCTL_MAGIC, 8)

#define MAX97000_SPK_EAR_ON \
	        _IO(MAX97000_I2C_IOCTL_MAGIC, 9)

#define MAX97000_RCV_ON \
	        _IO(MAX97000_I2C_IOCTL_MAGIC, 10)

#define MAX97000_RCV_OFF \
	        _IO(MAX97000_I2C_IOCTL_MAGIC, 11)

    
#define MAX97000_I2C_IOCTL_INIT \
	_IO(MAX97000_I2C_IOCTL_MAGIC, 12) 

#define MAX97000_AMP_SUSPEND \
	_IO(MAX97000_I2C_IOCTL_MAGIC, 13)

#define MAX97000_AMP_RESUME \
	_IO(MAX97000_I2C_IOCTL_MAGIC, 14)

#define MAX97000_AMP_RECORDING_MAIN_MIC \
	_IO(MAX97000_I2C_IOCTL_MAGIC, 15)

#define MAX97000_AMP_RECORDING_SUB_MIC \
	_IO(MAX97000_I2C_IOCTL_MAGIC, 16)

#if 0
struct MAX97000_init {
	int preview; /* in: 1 for preview, 0 for capture */
	uint16_t chipid; /* out: chip id */
};
#endif
#endif


int MAX97000_suspend(void);
int MAX97000_resume(void);

int MAX97000_gpio_recording_start(int state);


/********************************************
* 		 Log message setting for debug 
********************************************/
#if 0
#define trace_in()  debug( "%s +", __FUNCTION__ )
#define trace_out()  debug( "%s -", __FUNCTION__ )
#else
#define trace_in()  
#define trace_out() 
#endif

#if 1
#define gprintk(fmt, x... ) printk("%s(%d): "fmt,__FUNCTION__,__LINE__,## x)
#define error(fmt,arg...) printk( KERN_CRIT "MAX97000 E: " fmt "\n", ## arg)
#define debug(fmt,arg...) printk( KERN_CRIT "MAX97000 D: " fmt "\n", ## arg)
#else
#define gprintk(x...)
#define error(fmt,arg...) 
#define debug(fmt,arg...) 
#endif
