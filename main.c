/* Modified by Kiran Kumar Lekkala <kiran4399@gmail.com>


#include "pru_cfg.h"
#include "pru_ctrl.h"
#include "pru_intc.h"
#include "resource_table.h"

volatile register uint32_t __R30;
volatile register uint32_t __R31;

/* Mapping Constant table register to variable */
//volatile far uint32_t CT_L3 __attribute__((cregister("L3OCMC", near), peripheral));
//volatile far uint32_t CT_DDR __attribute__((cregister("DDR", near), peripheral));

/* PRU-to-ARM interrupt */
#define PRU0_ARM_INTERRUPT (19+16)

#define HOST_NUM	2
#define CHAN_NUM	2
#define SERVO_NUM_PIN	8

/******PIN mappings**********
		"P8.36",	SERVO_PWR
		"P8.27",	SERVO_1
		"P8.28",	SERVO_2
		"P8.29",	SERVO_3
		"P8.30",	SERVO_4
		"P8.39",	SERVO_5
		"P8.40",	SERVO_6
		"P8.41",	SERVO_7
		"P8.42",	SERVO_8
****************************/


void main(void)
{
	volatile uint32_t mask=0;
	//volatile uint32_t *pDdr = (uint32_t *) &CT_DDR;
	double pulse_width[SERVO_NUM_PIN],pulse_original[SERVO_NUM_PIN], time_step = 1/10000;
	double period = 1/50;
	int pin;
	
	/*pulse_width[0] = pDdr[6];
	pulse_width[1] = pDdr[7];
	pulse_width[2] = pDdr[4];
	pulse_width[3] = pDdr[5];
	pulse_width[4] = pDdr[0];
	pulse_width[5] = pDdr[2];
	pulse_width[6] = pDdr[1];
	pulse_width[7] = pDdr[3];*/

	
	pulse_width[0] = 50;
	pulse_width[1] = 50;
	pulse_width[2] = 50;
	pulse_width[3] = 50;
	pulse_width[4] = 50;
	pulse_width[5] = 50;
	pulse_width[6] = 50;
	pulse_width[7] = 50;
	
	/* Clear SYSCFG[STANDBY_INIT] to enable OCP master port */
	CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

	/* Clear system event in SECR1 */
	CT_INTC.SECR1 = 0x1;

	/* Clear system event enable in ECR1 */
	CT_INTC.ECR1 = 0x1;

	/* Point C30 (L3) to 0x3000 offset and C31 (DDR) to 0x0 offset */
	PRU0_CTRL.CTPPR1 = 0x00003000;


		/* Pool for any receipt of interrupt on host 0 */
	//	while ((__R31 & 0x40000000) == 0) {
			for(pin=0;pin<=SERVO_NUM_PIN-1;pin++){
				pulse_width[pin] *= 0.01*period;
				pulse_original[pin] = pulse_width[pin];
			}
	//	}

	while(1){


		for(pin=0;pin<=SERVO_NUM_PIN-1;pin++){
				if(pulse_width[pin] > 0){
					mask |= 1 << (12-pin);
					pulse_width[pin] -= time_step;
				}

				else if(pulse_width[pin]<=-period+pulse_original[pin]){
					pulse_width[pin] = pulse_original[pin];
				}
				else if(pulse_width[pin] < 0){
					mask &= ~(1<<(12-pin));
					pulse_width[pin] -= time_step;
				}
		}

		__R30 = mask;
		
		}

	/* Halt PRU core */
	__halt();
}

