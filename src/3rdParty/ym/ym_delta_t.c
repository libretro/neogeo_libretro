// license:GPL-2.0+
// copyright-holders:Jarek Burczynski
/*
 **
 ** File: ymdeltat.c
 **
 ** YAMAHA DELTA-T adpcm sound emulation subroutine
 ** used by fmopl.c (Y8950) and fm.c (YM2608 and YM2610/B)
 **
 ** Base program is YM2610 emulator by Hiromitsu Shioya.
 ** Written by Tatsuyuki Satoh
 ** Improvements by Jarek Burczynski (bujar at mame dot net)
 **
 **
 ** History:
 **
 ** 03-08-2003 Jarek Burczynski:
 **  - fixed BRDY flag implementation.
 **
 ** 24-07-2003 Jarek Burczynski, Frits Hilderink:
 **  - fixed delault value for control2 in YM_DELTAT_ADPCM_Reset
 **
 ** 22-07-2003 Jarek Burczynski, Frits Hilderink:
 **  - fixed external memory support
 **
 ** 15-06-2003 Jarek Burczynski:
 **  - implemented CPU -> AUDIO ADPCM synthesis (via writes to the ADPCM data reg $08)
 **  - implemented support for the Limit address register
 **  - supported two bits from the control register 2 ($01): RAM TYPE (x1 bit/x8 bit), ROM/RAM
 **  - implemented external memory access (read/write) via the ADPCM data reg reads/writes
 **    Thanks go to Frits Hilderink for the example code.
 **
 ** 14-06-2003 Jarek Burczynski:
 **  - various fixes to enable proper support for status register flags: BSRDY, PCM BSY, ZERO
 **  - modified EOS handling
 **
 ** 05-04-2003 Jarek Burczynski:
 **  - implemented partial support for external/processor memory on sample replay
 **
 ** 01-12-2002 Jarek Burczynski:
 **  - fixed first missing sound in gigandes thanks to previous fix (interpolator) by ElSemi
 **  - renamed/removed some YM_DELTAT struct fields
 **
 ** 28-12-2001 Acho A. Tang
 **  - added EOS status report on ADPCM playback.
 **
 ** 05-08-2001 Jarek Burczynski:
 **  - now_step is initialized with 0 at the start of play.
 **
 ** 12-06-2001 Jarek Burczynski:
 **  - corrected end of sample bug in YM_DELTAT_ADPCM_CALC.
 **    Checked on real YM2610 chip - address register is 24 bits wide.
 **    Thanks go to Stefan Jokisch (stefan.jokisch@gmx.de) for tracking down the problem.
 **
 ** TO DO:
 **      Check size of the address register on the other chips....
 **
 ** Version 0.72
 **
 ** sound chips that have this unit:
 ** YM2608   OPNA
 ** YM2610/B OPNB
 ** Y8950    MSX AUDIO
 **
 */

#include "ym_delta_t.h"

#define YM_DELTAT_SHIFT    (16)

#define YM_DELTAT_DELTA_MAX (24576)
#define YM_DELTAT_DELTA_MIN (127)
#define YM_DELTAT_DELTA_DEF (127)

#define YM_DELTAT_DECODE_RANGE 32768
#define YM_DELTAT_DECODE_MIN (-(YM_DELTAT_DECODE_RANGE))
#define YM_DELTAT_DECODE_MAX ((YM_DELTAT_DECODE_RANGE)-1)


/* Forecast to next Forecast (rate = *8) */
/* 1/8 , 3/8 , 5/8 , 7/8 , 9/8 , 11/8 , 13/8 , 15/8 */
static int32_t ym_deltat_decode_tableB1[16] = {
	1,   3,   5,   7,   9,  11,  13,  15,
	-1,  -3,  -5,  -7,  -9, -11, -13, -15,
};
/* delta to next delta (rate= *64) */
/* 0.9 , 0.9 , 0.9 , 0.9 , 1.2 , 1.6 , 2.0 , 2.4 */
static int32_t ym_deltat_decode_tableB2[16] = {
	57,  57,  57,  57, 77, 102, 128, 153,
	57,  57,  57,  57, 77, 102, 128, 153
};

/* 0-DRAM x1, 1-ROM, 2-DRAM x8, 3-ROM (3 is bad setting - not allowed by the manual) */
static uint8_t dram_rightshift[4]={3,0,0,0};

#define YM_DELTAT_Limit(val,max,min)    \
{                                       \
	if ( val > max ) val = max;         \
	else if ( val < min ) val = min;    \
}

uint8_t ADPCMB_read(YM_DELTAT *adpcmb)
{
	uint8_t v = 0;
	
	/* external memory read */
	if ((adpcmb->portstate & 0xe0) == 0x20)
	{
		/* two dummy reads */
		if (adpcmb->memread)
		{
			adpcmb->now_addr = adpcmb->start << 1;
			adpcmb->memread--;
			return 0;
		}
		
		
		if (adpcmb->now_addr != (adpcmb->end << 1))
		{
			v = adpcmb->read_byte[adpcmb->now_addr>>1];
			
			/*logerror("YM Delta-T memory read  $%08x, v=$%02x\n", now_addr >> 1, v);*/
			
			adpcmb->now_addr += 2; /* two nibbles at a time */
			
			/* reset BRDY bit in status register, which means we are reading the memory now */
			if (adpcmb->status_reset_handler && adpcmb->status_change_BRDY_bit)
				(adpcmb->status_reset_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_BRDY_bit);
			
			/* setup a timer that will callback us in 10 master clock cycles for Y8950
			 * in the callback set the BRDY flag to 1 , which means we have another data ready.
			 * For now, we don't really do this; we simply reset and set the flag in zero time, so that the IRQ will work.
			 */
			/* set BRDY bit in status register */
			if (adpcmb->status_set_handler && adpcmb->status_change_BRDY_bit)
				(adpcmb->status_set_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_BRDY_bit);
		}
		else
		{
			/* set EOS bit in status register */
			if (adpcmb->status_set_handler && adpcmb->status_change_EOS_bit)
				(adpcmb->status_set_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_EOS_bit);
		}
	}
	
	return v;
}

/* DELTA-T ADPCM write register */
void ADPCMB_write(YM_DELTAT *adpcmb, int r, int v)
{
	if (r >= 0x10) return;
	adpcmb->reg[r] = v; /* stock data */
	
	switch (r)
	{
		case 0x00:
			/*
			 START:
			 Accessing *external* memory is started when START bit (D7) is set to "1", so
			 you must set all conditions needed for recording/playback before starting.
			 If you access *CPU-managed* memory, recording/playback starts after
			 read/write of ADPCM data register $08.
			 
			 REC:
			 0 = ADPCM synthesis (playback)
			 1 = ADPCM analysis (record)
			 
			 MEMDATA:
			 0 = processor (*CPU-managed*) memory (means: using register $08)
			 1 = external memory (using start/end/limit registers to access memory: RAM or ROM)
			 
			 
			 SPOFF:
			 controls output pin that should disable the speaker while ADPCM analysis
			 
			 RESET and REPEAT only work with external memory.
			 
			 
			 some examples:
			 value:   START, REC, MEMDAT, REPEAT, SPOFF, x,x,RESET   meaning:
			 C8     1      1    0       0       1      0 0 0       Analysis (recording) from AUDIO to CPU (to reg $08), sample rate in PRESCALER register
			 E8     1      1    1       0       1      0 0 0       Analysis (recording) from AUDIO to EXT.MEMORY,       sample rate in PRESCALER register
			 80     1      0    0       0       0      0 0 0       Synthesis (playing) from CPU (from reg $08) to AUDIO,sample rate in DELTA-N register
			 a0     1      0    1       0       0      0 0 0       Synthesis (playing) from EXT.MEMORY to AUDIO,        sample rate in DELTA-N register
			 
			 60     0      1    1       0       0      0 0 0       External memory write via ADPCM data register $08
			 20     0      0    1       0       0      0 0 0       External memory read via ADPCM data register $08
			 
			 */
			/* handle emulation mode */
//			if (emulation_mode == EMULATION_MODE_YM2610)
//			{
				v |= 0x20;      /*  YM2610 always uses external memory and doesn't even have memory flag bit. */
				v &= ~0x40;     /*  YM2610 has no rec bit */
//			}
			
			adpcmb->portstate = v & (0x80|0x40|0x20|0x10|0x01); /* start, rec, memory mode, repeat flag copy, reset(bit0) */
			
			if (adpcmb->portstate & 0x80)/* START,REC,MEMDATA,REPEAT,SPOFF,--,--,RESET */
			{
				/* set PCM BUSY bit */
				adpcmb->PCM_BSY = 1;
				
				/* start ADPCM */
				adpcmb->now_step = 0;
				adpcmb->acc      = 0;
				adpcmb->prev_acc = 0;
				adpcmb->adpcml   = 0;
				adpcmb->adpcmd   = YM_DELTAT_DELTA_DEF;
				adpcmb->now_data = 0;
				
			}
			
			if (adpcmb->portstate & 0x20) /* do we access external memory? */
			{
				adpcmb->now_addr = adpcmb->start << 1;
				adpcmb->memread = 2;    /* two dummy reads needed before accesing external memory via register $08*/
			}
			else    /* we access CPU memory (ADPCM data register $08) so we only reset now_addr here */
			{
				adpcmb->now_addr = 0;
			}
			
			if (adpcmb->portstate & 0x01)
			{
				adpcmb->portstate = 0x00;
				
				/* clear PCM BUSY bit (in status register) */
				adpcmb->PCM_BSY = 0;
				
				/* set BRDY flag */
				if (adpcmb->status_set_handler && adpcmb->status_change_BRDY_bit)
					(adpcmb->status_set_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_BRDY_bit);
			}
			break;
			
		case 0x01:  /* L,R,-,-,SAMPLE,DA/AD,RAMTYPE,ROM */
			/* handle emulation mode */
//			if (emulation_mode == EMULATION_MODE_YM2610)
//			{
				v |= 0x01;      /*  YM2610 always uses ROM as an external memory and doesn't tave ROM/RAM memory flag bit. */
//			}
			
			adpcmb->pan = &adpcmb->output_pointer[(v >> 6) & 0x03];
			if ((adpcmb->control2 & 3) != (v & 3))
			{
				/*0-DRAM x1, 1-ROM, 2-DRAM x8, 3-ROM (3 is bad setting - not allowed by the manual) */
				if (adpcmb->DRAMportshift != dram_rightshift[v & 3])
				{
					adpcmb->DRAMportshift = dram_rightshift[v & 3];
					
					/* final shift value depends on chip type and memory type selected:
					 8 for YM2610 (ROM only),
					 5 for ROM for Y8950 and YM2608,
					 5 for x8bit DRAMs for Y8950 and YM2608,
					 2 for x1bit DRAMs for Y8950 and YM2608.
					 */
					
					/* refresh addresses */
					adpcmb->start  = (adpcmb->reg[0x3] * 0x0100 | adpcmb->reg[0x2]) << (adpcmb->portshift - adpcmb->DRAMportshift);
					adpcmb->end    = (adpcmb->reg[0x5] * 0x0100 | adpcmb->reg[0x4]) << (adpcmb->portshift - adpcmb->DRAMportshift);
					adpcmb->end   += (1 << (adpcmb->portshift - adpcmb->DRAMportshift)) - 1;
					adpcmb->limit  = (adpcmb->reg[0xd]*0x0100 | adpcmb->reg[0xc]) << (adpcmb->portshift - adpcmb->DRAMportshift);
				}
			}
			adpcmb->control2 = v;
			break;
			
		case 0x02:  /* Start Address L */
		case 0x03:  /* Start Address H */
			adpcmb->start  = (adpcmb->reg[0x3] * 0x0100 | adpcmb->reg[0x2]) << (adpcmb->portshift - adpcmb->DRAMportshift);
			/*logerror("DELTAT start: 02=%2x 03=%2x addr=%8x\n",reg[0x2], reg[0x3],start );*/
			break;
			
		case 0x04:  /* Stop Address L */
		case 0x05:  /* Stop Address H */
			adpcmb->end    = (adpcmb->reg[0x5]*0x0100 | adpcmb->reg[0x4]) << (adpcmb->portshift - adpcmb->DRAMportshift);
			adpcmb->end   += (1 << (adpcmb->portshift - adpcmb->DRAMportshift)) - 1;
			/*logerror("DELTAT end  : 04=%2x 05=%2x addr=%8x\n",reg[0x4], reg[0x5],end   );*/
			break;
			
		case 0x06:  /* Prescale L (ADPCM and Record frq) */
		case 0x07:  /* Prescale H */
			break;
			
		case 0x08:  /* ADPCM data */
			/*
			 some examples:
			 value:   START, REC, MEMDAT, REPEAT, SPOFF, x,x,RESET   meaning:
			 C8     1      1    0       0       1      0 0 0       Analysis (recording) from AUDIO to CPU (to reg $08), sample rate in PRESCALER register
			 E8     1      1    1       0       1      0 0 0       Analysis (recording) from AUDIO to EXT.MEMORY,       sample rate in PRESCALER register
			 80     1      0    0       0       0      0 0 0       Synthesis (playing) from CPU (from reg $08) to AUDIO,sample rate in DELTA-N register
			 a0     1      0    1       0       0      0 0 0       Synthesis (playing) from EXT.MEMORY to AUDIO,        sample rate in DELTA-N register
			 
			 60     0      1    1       0       0      0 0 0       External memory write via ADPCM data register $08
			 20     0      0    1       0       0      0 0 0       External memory read via ADPCM data register $08
			 
			 */
			
			/* external memory write */
			if ((adpcmb->portstate & 0xe0) == 0x60)
			{
				if (adpcmb->memread)
				{
					adpcmb->now_addr = adpcmb->start << 1;
					adpcmb->memread = 0;
				}
				
				/*logerror("YM Delta-T memory write $%08x, v=$%02x\n", now_addr >> 1, v);*/
				
				if (adpcmb->now_addr != (adpcmb->end << 1))
				{
					if (adpcmb->write_byte) {
						adpcmb->write_byte(0, adpcmb->now_addr >> 1, v);
					}
					adpcmb->now_addr += 2; /* two nybbles at a time */
					
					/* reset BRDY bit in status register, which means we are processing the write */
					if (adpcmb->status_reset_handler && adpcmb->status_change_BRDY_bit)
						(adpcmb->status_reset_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_BRDY_bit);
					
					/* setup a timer that will callback us in 10 master clock cycles for Y8950
					 * in the callback set the BRDY flag to 1 , which means we have written the data.
					 * For now, we don't really do this; we simply reset and set the flag in zero time, so that the IRQ will work.
					 */
					/* set BRDY bit in status register */
					if (adpcmb->status_set_handler && adpcmb->status_change_BRDY_bit)
						(adpcmb->status_set_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_BRDY_bit);
					
				}
				else
				{
					/* set EOS bit in status register */
					if (adpcmb->status_set_handler && adpcmb->status_change_EOS_bit)
						(adpcmb->status_set_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_EOS_bit);
				}
				
				return;
			}
			
			/* ADPCM synthesis from CPU */
			if ((adpcmb->portstate & 0xe0) == 0x80)
			{
				adpcmb->CPU_data = v;
				
				/* Reset BRDY bit in status register, which means we are full of data */
				if (adpcmb->status_reset_handler && adpcmb->status_change_BRDY_bit)
					(adpcmb->status_reset_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_BRDY_bit);
				return;
			}
			
			break;
			
		case 0x09:  /* DELTA-N L (ADPCM Playback Prescaler) */
		case 0x0a:  /* DELTA-N H */
			adpcmb->delta  = (adpcmb->reg[0xa] * 0x0100 | adpcmb->reg[0x9]);
			adpcmb->step     = (uint32_t)((double)(adpcmb->delta /* *(1<<(YM_DELTAT_SHIFT-16)) */) * adpcmb->freqbase);
			/*logerror("DELTAT deltan:09=%2x 0a=%2x\n",reg[0x9], reg[0xa]);*/
			break;
			
		case 0x0b:  /* Output level control (volume, linear) */
		{
			const int32_t oldvol = adpcmb->volume;
			adpcmb->volume = (v & 0xff) * (adpcmb->output_range / 256) / YM_DELTAT_DECODE_RANGE;
			/*                              v     *     ((1<<16)>>8)        >>  15;
			 *                       thus:   v     *     (1<<8)              >>  15;
			 *                       thus: output_range must be (1 << (15+8)) at least
			 *                               v     *     ((1<<23)>>8)        >>  15;
			 *                               v     *     (1<<15)             >>  15;
			 */
			/*logerror("DELTAT vol = %2x\n",v&0xff);*/
			if (oldvol != 0)
			{
				adpcmb->adpcml = (int)((double)(adpcmb->adpcml) / (double)(oldvol) * (double)(adpcmb->volume));
			}
		}
			break;
			
		case 0x0c:  /* Limit Address L */
		case 0x0d:  /* Limit Address H */
			adpcmb->limit  = (adpcmb->reg[0xd] * 0x0100 | adpcmb->reg[0xc]) << (adpcmb->portshift - adpcmb->DRAMportshift);
			/*logerror("DELTAT limit: 0c=%2x 0d=%2x addr=%8x\n",reg[0xc], reg[0xd],limit );*/
			break;
	}
}

void ADPCMB_reset(int panidx, YM_DELTAT *adpcmb)
{
	adpcmb->now_addr  = 0;
	adpcmb->now_step  = 0;
	adpcmb->step      = 0;
	adpcmb->start     = 0;
	adpcmb->end       = 0;
	adpcmb->limit     = ~0; /* this way YM2610 and Y8950 (both of which don't have limit address reg) will still work */
	adpcmb->volume    = 0;
	adpcmb->pan       = &adpcmb->output_pointer[panidx];
	adpcmb->acc       = 0;
	adpcmb->prev_acc  = 0;
	adpcmb->adpcmd    = 127;
	adpcmb->adpcml    = 0;
//	emulation_mode = uint8_t(mode);
	adpcmb->portstate = 0x20;
	adpcmb->control2  = 0x01; /* default setting depends on the emulation mode. MSX demo called "facdemo_4" doesn't setup control2 register at all and still works */
	adpcmb->DRAMportshift = dram_rightshift[adpcmb->control2 & 3];
	
	/* The flag mask register disables the BRDY after the reset, however
	 ** as soon as the mask is enabled the flag needs to be set. */
	
	/* set BRDY bit in status register */
	if (adpcmb->status_set_handler && adpcmb->status_change_BRDY_bit)
		(adpcmb->status_set_handler)(adpcmb->status_change_which_chip, adpcmb->status_change_BRDY_bit);
}

void ADPCMB_postload(YM_DELTAT *adpcmb, uint8_t *regs)
{
	/* to keep adpcml */
	adpcmb->volume = 0;
	/* update */
	for (int r = 1; r < 16; r++)
		ADPCMB_write(adpcmb, r, (int)regs[r]);
	adpcmb->reg[0] = regs[0];
	
	/* current rom data */
	adpcmb->now_data = adpcmb->read_byte[adpcmb->now_addr >> 1];
	
}

static inline void YM_DELTAT_synthesis_from_external_memory(YM_DELTAT *DELTAT)
{
	uint32_t step;
	int data;
	
	DELTAT->now_step += DELTAT->step;
	if ( DELTAT->now_step >= (1<<YM_DELTAT_SHIFT) )
	{
		step = DELTAT->now_step >> YM_DELTAT_SHIFT;
		DELTAT->now_step &= (1<<YM_DELTAT_SHIFT)-1;
		do{
			if ( DELTAT->now_addr == (DELTAT->limit<<1) )
				DELTAT->now_addr = 0;
			
			if ( DELTAT->now_addr == (DELTAT->end<<1) ) {   /* 12-06-2001 JB: corrected comparison. Was > instead of == */
				if( DELTAT->portstate&0x10 ){
					/* repeat start */
					DELTAT->now_addr = DELTAT->start<<1;
					DELTAT->acc      = 0;
					DELTAT->adpcmd   = YM_DELTAT_DELTA_DEF;
					DELTAT->prev_acc = 0;
				}else{
					/* set EOS bit in status register */
					if(DELTAT->status_set_handler)
						if(DELTAT->status_change_EOS_bit)
							(DELTAT->status_set_handler)(DELTAT->status_change_which_chip, DELTAT->status_change_EOS_bit);
					
					/* clear PCM BUSY bit (reflected in status register) */
					DELTAT->PCM_BSY = 0;
					
					DELTAT->portstate = 0;
					DELTAT->adpcml = 0;
					DELTAT->prev_acc = 0;
					return;
				}
			}
			
			if( DELTAT->now_addr&1 ) data = DELTAT->now_data & 0x0f;
			else
			{
				DELTAT->now_data = DELTAT->read_byte[DELTAT->now_addr>>1];
				data = DELTAT->now_data >> 4;
			}
			
			DELTAT->now_addr++;
			/* 12-06-2001 JB: */
			/* YM2610 address register is 24 bits wide.*/
			/* The "+1" is there because we use 1 bit more for nibble calculations.*/
			/* WARNING: */
			/* Side effect: we should take the size of the mapped ROM into account */
			DELTAT->now_addr &= ( (1<<(24+1))-1);
			
			/* store accumulator value */
			DELTAT->prev_acc = DELTAT->acc;
			
			/* Forecast to next Forecast */
			DELTAT->acc += (ym_deltat_decode_tableB1[data] * DELTAT->adpcmd / 8);
			YM_DELTAT_Limit(DELTAT->acc,YM_DELTAT_DECODE_MAX, YM_DELTAT_DECODE_MIN);
			
			/* delta to next delta */
			DELTAT->adpcmd = (DELTAT->adpcmd * ym_deltat_decode_tableB2[data] ) / 64;
			YM_DELTAT_Limit(DELTAT->adpcmd,YM_DELTAT_DELTA_MAX, YM_DELTAT_DELTA_MIN );
			
			/* ElSemi: Fix interpolator. */
			/*DELTAT->prev_acc = prev_acc + ((DELTAT->acc - prev_acc) / 2 );*/
			
		}while(--step);
		
	}
	
	/* ElSemi: Fix interpolator. */
	DELTAT->adpcml = DELTAT->prev_acc * (int)((1<<YM_DELTAT_SHIFT)-DELTAT->now_step);
	DELTAT->adpcml += (DELTAT->acc * (int)DELTAT->now_step);
	DELTAT->adpcml = (DELTAT->adpcml>>YM_DELTAT_SHIFT) * (int)DELTAT->volume;
	
	/* output for work of output channels (outd[OPNxxxx])*/
	*(DELTAT->pan) += DELTAT->adpcml;
}

static inline void YM_DELTAT_synthesis_from_CPU_memory(YM_DELTAT *DELTAT)
{
	uint32_t step;
	int data;
	
	DELTAT->now_step += DELTAT->step;
	if ( DELTAT->now_step >= (1<<YM_DELTAT_SHIFT) )
	{
		step = DELTAT->now_step >> YM_DELTAT_SHIFT;
		DELTAT->now_step &= (1<<YM_DELTAT_SHIFT)-1;
		do{
			if( DELTAT->now_addr&1 )
			{
				data = DELTAT->now_data & 0x0f;
				
				DELTAT->now_data = DELTAT->CPU_data;
				
				/* after we used CPU_data, we set BRDY bit in status register,
				 * which means we are ready to accept another byte of data */
				if(DELTAT->status_set_handler)
					if(DELTAT->status_change_BRDY_bit)
						(DELTAT->status_set_handler)(DELTAT->status_change_which_chip, DELTAT->status_change_BRDY_bit);
			}
			else
			{
				data = DELTAT->now_data >> 4;
			}
			
			DELTAT->now_addr++;
			
			/* store accumulator value */
			DELTAT->prev_acc = DELTAT->acc;
			
			/* Forecast to next Forecast */
			DELTAT->acc += (ym_deltat_decode_tableB1[data] * DELTAT->adpcmd / 8);
			YM_DELTAT_Limit(DELTAT->acc,YM_DELTAT_DECODE_MAX, YM_DELTAT_DECODE_MIN);
			
			/* delta to next delta */
			DELTAT->adpcmd = (DELTAT->adpcmd * ym_deltat_decode_tableB2[data] ) / 64;
			YM_DELTAT_Limit(DELTAT->adpcmd,YM_DELTAT_DELTA_MAX, YM_DELTAT_DELTA_MIN );
			
			
		}while(--step);
		
	}
	
	/* ElSemi: Fix interpolator. */
	DELTAT->adpcml = DELTAT->prev_acc * (int)((1<<YM_DELTAT_SHIFT)-DELTAT->now_step);
	DELTAT->adpcml += (DELTAT->acc * (int)DELTAT->now_step);
	DELTAT->adpcml = (DELTAT->adpcml>>YM_DELTAT_SHIFT) * (int)DELTAT->volume;
	
	/* output for work of output channels (outd[OPNxxxx])*/
	*(DELTAT->pan) += DELTAT->adpcml;
}



/* ADPCM B (Delta-T control type) */
void ADPCMB_CALC(YM_DELTAT *adpcmb)
{
	/*
	 some examples:
	 value:   START, REC, MEMDAT, REPEAT, SPOFF, x,x,RESET   meaning:
	 80     1      0    0       0       0      0 0 0       Synthesis (playing) from CPU (from reg $08) to AUDIO,sample rate in DELTA-N register
	 a0     1      0    1       0       0      0 0 0       Synthesis (playing) from EXT.MEMORY to AUDIO,        sample rate in DELTA-N register
	 C8     1      1    0       0       1      0 0 0       Analysis (recording) from AUDIO to CPU (to reg $08), sample rate in PRESCALER register
	 E8     1      1    1       0       1      0 0 0       Analysis (recording) from AUDIO to EXT.MEMORY,       sample rate in PRESCALER register
	 
	 60     0      1    1       0       0      0 0 0       External memory write via ADPCM data register $08
	 20     0      0    1       0       0      0 0 0       External memory read via ADPCM data register $08
	 
	 */
	
	if ( (adpcmb->portstate & 0xe0)==0xa0 )
	{
		YM_DELTAT_synthesis_from_external_memory(adpcmb);
		return;
	}
	
	if ( (adpcmb->portstate & 0xe0)==0x80 )
	{
		/* ADPCM synthesis from CPU-managed memory (from reg $08) */
		YM_DELTAT_synthesis_from_CPU_memory(adpcmb);    /* change output based on data in ADPCM data reg ($08) */
		return;
	}
	
	//todo: ADPCM analysis
	//  if ( (portstate & 0xe0)==0xc0 )
	//  if ( (portstate & 0xe0)==0xe0 )
	
	return;
}
