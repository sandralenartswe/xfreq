/*
 * XFreq.c #0.16 SR0 by CyrIng
 *
 * Copyright (C) 2013-2014 CYRIL INGENIERIE
 * Licenses: GPL2
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "xfreq.h"

//	Read, Write a Model Specific Register.
#define	Read_MSR(FD, offset, msr)  pread(FD, msr, sizeof(*msr), offset)
#define	Write_MSR(FD, offset, msr) pwrite(FD, msr, sizeof(*msr), offset)

// The drawing thread.
static pthread_mutex_t	uDraw_mutex;
static void *uDraw(void *uArg);

//	Open one MSR handle per Processor Core.
int	Open_MSR(uARG *A) {
	ssize_t	retval=0;
	int	tmpFD=open("/dev/cpu/0/msr", O_RDONLY);
	int	rc=0;
	// Read the minimum, maximum & the turbo ratios from Core number 0
	if(tmpFD != -1) {
		rc=((retval=Read_MSR(tmpFD, IA32_MISC_ENABLE, (MISC_PROC_FEATURES *) &A->P.MiscFeatures)) != -1);
		rc=((retval=Read_MSR(tmpFD, MSR_PLATFORM_INFO, (PLATFORM *) &A->P.Platform)) != -1);
		rc=((retval=Read_MSR(tmpFD, MSR_TURBO_RATIO_LIMIT, (TURBO *) &A->P.Turbo)) != -1);
		close(tmpFD);
	}
	char	pathname[]="/dev/cpu/999/msr";
	int	cpu=0;
	for(cpu=0; rc && (cpu < A->P.CPU); cpu++) {
		sprintf(pathname, "/dev/cpu/%d/msr", cpu);
		if( (rc=((A->P.Core[cpu].FD=open(pathname, O_RDWR)) != -1)) )
			// Enable the Performance Counters 1 and 2 :
			// - Set the global counter bits
			rc=((retval=Read_MSR(A->P.Core[cpu].FD, IA32_PERF_GLOBAL_CTRL, (GLOBAL_PERF_COUNTER *) &A->P.Core[cpu].GlobalPerfCounter)) != -1);
			A->P.Core[cpu].GlobalPerfCounter.EN_FIXED_CTR1=1;
			A->P.Core[cpu].GlobalPerfCounter.EN_FIXED_CTR2=1;
			rc=((retval=Write_MSR(A->P.Core[cpu].FD, IA32_PERF_GLOBAL_CTRL, (GLOBAL_PERF_COUNTER *) &A->P.Core[cpu].GlobalPerfCounter)) != -1);
			// - Set the fixed counter bits
			rc=((retval=Read_MSR(A->P.Core[cpu].FD, IA32_FIXED_CTR_CTRL, (FIXED_PERF_COUNTER *) &A->P.Core[cpu].FixedPerfCounter)) != -1);
			A->P.Core[cpu].FixedPerfCounter.EN1_OS=1;
			A->P.Core[cpu].FixedPerfCounter.EN2_OS=1;
			A->P.Core[cpu].FixedPerfCounter.EN1_Usr=1;
			A->P.Core[cpu].FixedPerfCounter.EN2_Usr=1;
			if(A->P.PerCore) {
				A->P.Core[cpu].FixedPerfCounter.AnyThread_EN1=1;
				A->P.Core[cpu].FixedPerfCounter.AnyThread_EN2=1;
			}
			else {
				A->P.Core[cpu].FixedPerfCounter.AnyThread_EN1=0;
				A->P.Core[cpu].FixedPerfCounter.AnyThread_EN2=0;
			}
			rc=((retval=Write_MSR(A->P.Core[cpu].FD, IA32_FIXED_CTR_CTRL, (FIXED_PERF_COUNTER *) &A->P.Core[cpu].FixedPerfCounter)) != -1);
			// Retreive the Thermal Junction Max. Fallback to 100°C if not available.
			rc=((retval=Read_MSR(A->P.Core[cpu].FD, MSR_TEMPERATURE_TARGET, (TJMAX *) &A->P.Core[cpu].TjMax)) != -1);
			if(A->P.Core[cpu].TjMax.Target == 0)
				A->P.Core[cpu].TjMax.Target=100;
	}
	return(rc);
}

// Close all MSR handles.
void	Close_MSR(uARG *A) {
	int	cpu=0;
	for(cpu=0; cpu < A->P.CPU; cpu++) {
		// Reset the fixed counters.
		A->P.Core[cpu].FixedPerfCounter.EN1_Usr=0;
		A->P.Core[cpu].FixedPerfCounter.EN2_Usr=0;
		A->P.Core[cpu].FixedPerfCounter.EN1_OS=0;
		A->P.Core[cpu].FixedPerfCounter.EN2_OS=0;
		A->P.Core[cpu].FixedPerfCounter.AnyThread_EN1=0;
		A->P.Core[cpu].FixedPerfCounter.AnyThread_EN2=0;
		Write_MSR(A->P.Core[cpu].FD, IA32_FIXED_CTR_CTRL, &A->P.Core[cpu].FixedPerfCounter);
		// Reset the global counters.
		A->P.Core[cpu].GlobalPerfCounter.EN_FIXED_CTR1=0;
		A->P.Core[cpu].GlobalPerfCounter.EN_FIXED_CTR2=0;
		Write_MSR(A->P.Core[cpu].FD, IA32_PERF_GLOBAL_CTRL, &A->P.Core[cpu].GlobalPerfCounter);
		// Release the MSR handle associated to the Core.
		if(A->P.Core[cpu].FD != -1)
			close(A->P.Core[cpu].FD);
	}
}

// Read the Time Stamp Counter.
static __inline__ unsigned long long RDTSC(void)
{
	unsigned Hi, Lo;

	__asm__ volatile
	(
		"rdtsc;"
		:"=a" (Lo),
		 "=d" (Hi)
	);
	return ((unsigned long long) Lo) | (((unsigned long long) Hi) << 32);
}

// The Processor thread which updates the Core values.
static void *uCycle(void *uArg) {
	uARG *A=(uARG *) uArg;

	register unsigned int cpu=0;
	for(cpu=0; cpu < A->P.CPU; cpu++) {
		// Initial read of the Unhalted Core & Reference Cycles.
		Read_MSR(A->P.Core[cpu].FD, IA32_FIXED_CTR1, (unsigned long long *) &A->P.Core[cpu].UnhaltedCoreCycles[0]);
		Read_MSR(A->P.Core[cpu].FD, IA32_FIXED_CTR2, (unsigned long long *) &A->P.Core[cpu].UnhaltedRefCycles[0] );
		// Initial read of other C-States.
		Read_MSR(A->P.Core[cpu].FD, MSR_CORE_C3_RESIDENCY, (unsigned long long *) &A->P.Core[cpu].RefCycles.C3[0]);
		Read_MSR(A->P.Core[cpu].FD, MSR_CORE_C6_RESIDENCY, (unsigned long long *) &A->P.Core[cpu].RefCycles.C6[0]);
		// Initial read of the TSC in relation to the Logical Core.
		Read_MSR(A->P.Core[cpu].FD, IA32_TIME_STAMP_COUNTER, (unsigned long long *) &A->P.Core[cpu].TSC[0]);
	}

	while(A->LOOP) {
		// Settle down some microseconds as specified by the command argument.
		usleep(A->P.IdleTime);

/* CRITICAL_IN  */
		for(cpu=0; cpu < A->P.CPU; cpu++) {
			// Update the Base Operating Ratio.
			Read_MSR(A->P.Core[cpu].FD, IA32_PERF_STATUS, (PERF_STATUS *) &A->P.Core[cpu].Operating);
			// Update the Unhalted Core & the Reference Cycles.
			Read_MSR(A->P.Core[cpu].FD, IA32_FIXED_CTR1, (unsigned long long *) &A->P.Core[cpu].UnhaltedCoreCycles[1]);
			Read_MSR(A->P.Core[cpu].FD, IA32_FIXED_CTR2, (unsigned long long *) &A->P.Core[cpu].UnhaltedRefCycles[1]);
			// Update C-States.
			Read_MSR(A->P.Core[cpu].FD, MSR_CORE_C3_RESIDENCY, (unsigned long long *) &A->P.Core[cpu].RefCycles.C3[1]);
			Read_MSR(A->P.Core[cpu].FD, MSR_CORE_C6_RESIDENCY, (unsigned long long *) &A->P.Core[cpu].RefCycles.C6[1]);
			// Update TSC.
			Read_MSR(A->P.Core[cpu].FD, IA32_TIME_STAMP_COUNTER, (unsigned long long *) &A->P.Core[cpu].TSC[1]);
		}
/* CRITICAL_OUT */

		// Reset C-States average.
		A->P.Avg.C0=A->P.Avg.C3=A->P.Avg.C6=0;

		unsigned int maxFreq=0, maxTemp=A->P.Core[0].TjMax.Target;
		for(cpu=0; cpu < A->P.CPU; cpu++) {
			// Compute the Operating Frequency.
			A->P.Core[cpu].OperatingFreq=A->P.Core[cpu].Operating.Ratio * A->P.ClockSpeed;
			// Compute the Delta of Unhalted (Core & Ref) Cycles = Current[1] - Previous[0]
			register unsigned long long	UnhaltedCoreCycles	= A->P.Core[cpu].UnhaltedCoreCycles[1]
										- A->P.Core[cpu].UnhaltedCoreCycles[0],
							UnhaltedRefCycles	= A->P.Core[cpu].UnhaltedRefCycles[1]
										- A->P.Core[cpu].UnhaltedRefCycles[0],
							DeltaC3RefCycles	= A->P.Core[cpu].RefCycles.C3[1]
										- A->P.Core[cpu].RefCycles.C3[0],
							DeltaC6RefCycles	= A->P.Core[cpu].RefCycles.C6[1]
										- A->P.Core[cpu].RefCycles.C6[0];
			A->P.Core[cpu].DeltaTSC=A->P.Core[cpu].TSC[1] - A->P.Core[cpu].TSC[0];
			// Compute C-States.
			A->P.Core[cpu].State.C0=(double) (UnhaltedRefCycles) / (double) (A->P.Core[cpu].DeltaTSC);
			A->P.Core[cpu].State.C3=(double) (DeltaC3RefCycles)  / (double) (A->P.Core[cpu].DeltaTSC);
			A->P.Core[cpu].State.C6=(double) (DeltaC6RefCycles)  / (double) (A->P.Core[cpu].DeltaTSC);
			// Compute the Current Core Ratio per Cycles Delta. Set with the Operating value to protect against a division by zero.
			A->P.Core[cpu].UnhaltedRatio	= (UnhaltedRefCycles != 0) ?
							 (A->P.Core[cpu].Operating.Ratio * UnhaltedCoreCycles) / UnhaltedRefCycles
							: A->P.Core[cpu].Operating.Ratio;
			// Dynamic Frequency = Unhalted Ratio x Bus Clock Frequency
			A->P.Core[cpu].UnhaltedFreq=A->P.Core[cpu].UnhaltedRatio * A->P.ClockSpeed;
			// Save TSC.
			A->P.Core[cpu].TSC[0]=A->P.Core[cpu].TSC[1];
			// Save the Unhalted Core & Reference Cycles for next iteration.
			A->P.Core[cpu].UnhaltedCoreCycles[0]=A->P.Core[cpu].UnhaltedCoreCycles[1];
			A->P.Core[cpu].UnhaltedRefCycles[0] =A->P.Core[cpu].UnhaltedRefCycles[1];
			// Save also C-State Reference Cycles.
			A->P.Core[cpu].RefCycles.C3[0]=A->P.Core[cpu].RefCycles.C3[1];
			A->P.Core[cpu].RefCycles.C6[0]=A->P.Core[cpu].RefCycles.C6[1];
			// Sum the C-States before the average.
			A->P.Avg.C0+=A->P.Core[cpu].State.C0;
			A->P.Avg.C3+=A->P.Core[cpu].State.C3;
			A->P.Avg.C6+=A->P.Core[cpu].State.C6;

			// Index the Top CPU speed.
			if(maxFreq < A->P.Core[cpu].UnhaltedFreq) {
				maxFreq=A->P.Core[cpu].UnhaltedFreq;
				A->P.Top=cpu;
			}
			// Update the Digital Thermal Sensor.
			if( (Read_MSR(A->P.Core[cpu].FD, IA32_THERM_STATUS, (THERM_STATUS *) &A->P.Core[cpu].ThermStat)) == -1)
				A->P.Core[cpu].ThermStat.DTS=0;
			Read_MSR(A->P.Core[cpu].FD, IA32_THERM_INTERRUPT, (THERM_INTERRUPT *) &A->P.Core[cpu].ThermIntr);

			// Index the Hotest Core.
			if(A->P.Core[cpu].ThermStat.DTS < maxTemp) {
				maxTemp=A->P.Core[cpu].ThermStat.DTS;
				A->P.Hot=cpu;
			}
		}
		// Average the C-States.
		A->P.Avg.C0/=A->P.CPU;
		A->P.Avg.C3/=A->P.CPU;
		A->P.Avg.C6/=A->P.CPU;

		// Fire the drawing thread.
		if(pthread_mutex_trylock(&uDraw_mutex) == 0)
			pthread_create(&A->TID_Draw, NULL, uDraw, A);
	}
	// Drawing is still processing ?
	if(pthread_mutex_trylock(&uDraw_mutex) == EBUSY)
		pthread_join(A->TID_Draw, NULL);

	return(NULL);
}

// Read any data from the SMBIOS.
int	Read_SMBIOS(int structure, int instance, off_t offset, void *buf, size_t nbyte) {
	ssize_t	retval=0;
	char	pathname[]="/sys/firmware/dmi/entries/999-99/raw";
	int	tmpFD=0, rc=-1;

	sprintf(pathname, "/sys/firmware/dmi/entries/%d-%d/raw", structure, instance);
	if( (tmpFD=open(pathname, O_RDONLY)) != -1 ) {
		retval=pread(tmpFD, buf, nbyte, offset);
		close(tmpFD);
		rc=(retval != nbyte) ? -1 : 0;
	}
	return(rc);
}

// Old fashion style to compute the processor frequency based on TSC.
unsigned long long int FallBack_Freq()
{
	struct timezone tz;
	struct timeval tvstart, tvstop;
	unsigned long long int cycles[2];
	unsigned int microseconds;

	memset(&tz, 0, sizeof(tz));

	gettimeofday(&tvstart, &tz);
	cycles[0] = RDTSC();
	gettimeofday(&tvstart, &tz);

	usleep(10000);

	cycles[1] = RDTSC();
	gettimeofday(&tvstop, &tz);
	microseconds = ( (tvstop.tv_sec - tvstart.tv_sec) * 10000) + (tvstop.tv_usec - tvstart.tv_usec);

	return( (cycles[1] - cycles[0]) / microseconds );
}

// Read the Bus Clock Frequency from the BIOS.
int	Get_ExternalClock() {
	int	BCLK=0;

	if( Read_SMBIOS(SMBIOS_PROCINFO_STRUCTURE,
			SMBIOS_PROCINFO_INSTANCE,
			SMBIOS_PROCINFO_EXTCLK, &BCLK, 1) != -1)
		return(BCLK);
	else
		return(0);
}

// Read the number of logical Cores activated in the BIOS.
int	Get_ThreadCount() {
	short int ThreadCount=0;

	if( Read_SMBIOS(SMBIOS_PROCINFO_STRUCTURE,
			SMBIOS_PROCINFO_INSTANCE,
			SMBIOS_PROCINFO_THREADS, &ThreadCount, 1) != -1)
		return(ThreadCount);
	else
		return(0);
}

// Read the number of physicial Cores activated in the BIOS.
int	Get_CoreCount() {
	short int CoreCount=0;

	if( Read_SMBIOS(SMBIOS_PROCINFO_STRUCTURE,
			SMBIOS_PROCINFO_INSTANCE,
			SMBIOS_PROCINFO_CORES, &CoreCount, 1) != -1)
		return(CoreCount);
	else
		return(0);
}

// Call the CPUID instruction.
void	CPUID(FEATURES *features)
{
	__asm__ volatile
	(
		"movq	$0x1, %%rax;"
		"cpuid;"
		: "=a"	(features->Std.EAX),
		  "=b"	(features->Std.EBX),
		  "=c"	(features->Std.ECX),
		  "=d"	(features->Std.EDX)
	);
	__asm__ volatile
	(
		"movq	$0x4, %%rax;"
		"xorq	%%rcx, %%rcx;"
		"cpuid;"
		"shr	$26, %%rax;"
		"and	$0x3f, %%rax;"
		"add	$1, %%rax;"
		: "=a"	(features->ThreadCount)
	);
	__asm__ volatile
	(
		"movq	$0x80000000, %%rax;"
		"cpuid;"
		: "=a"	(features->LargestExtFunc)
	);
	if(features->LargestExtFunc >= 0x80000004 && features->LargestExtFunc <= 0x80000008)
	{
		__asm__ volatile
		(
			"movq	$0x80000001, %%rax;"
			"cpuid;"
			: "=c"	(features->Ext.ECX),
			  "=d"	(features->Ext.EDX)
		);
		struct
		{
			struct
			{
			unsigned char Chr[4];
			} EAX, EBX, ECX, EDX;
		} Brand;
		char tmpString[48+1]={0x20};
		int ix=0, jx=0, px=0;
		for(ix=0; ix<3; ix++)
		{
			__asm__ volatile
			(
				"cpuid;"
				: "=a"	(Brand.EAX),
				  "=b"	(Brand.EBX),
				  "=c"	(Brand.ECX),
				  "=d"	(Brand.EDX)
				: "a"	(0x80000002 + ix)
			);
				for(jx=0; jx<4; jx++, px++)
					tmpString[px]=Brand.EAX.Chr[jx];
				for(jx=0; jx<4; jx++, px++)
					tmpString[px]=Brand.EBX.Chr[jx];
				for(jx=0; jx<4; jx++, px++)
					tmpString[px]=Brand.ECX.Chr[jx];
				for(jx=0; jx<4; jx++, px++)
					tmpString[px]=Brand.EDX.Chr[jx];
		}
		for(ix=jx=0; jx < px; jx++)
			if(!(tmpString[jx] == 0x20 && tmpString[jx+1] == 0x20))
				features->BrandString[ix++]=tmpString[jx];
	}
}

// Read data from the PCI bus.
#define PCI_CONFIG_ADDRESS(bus, dev, fn, reg) \
	(0x80000000 | (bus << 16) | (dev << 11) | (fn << 8) | (reg & ~3))

// Retreive the Integrated Memory Controler settings: the number of channels & their associated RAM timings.
struct IMCINFO *IMC_Read_Info()
{
	struct	IMCINFO *imc=calloc(1, sizeof(struct IMCINFO));

	if(!iopl(3))
	{
		unsigned bus=0xff, dev=0x4, func=0;
		outl(PCI_CONFIG_ADDRESS(bus, 3, 0, 0x48), 0xcf8);
		int code=(inw(0xcfc + (0x48 & 2)) >> 8) & 0x7;
		imc->ChannelCount=(code == 7 ? 3 : code == 4 ? 1 : code == 2 ? 1 : code == 1 ? 1 : 2);
		imc->Channel=calloc(imc->ChannelCount, sizeof(struct CHANNEL));

		unsigned cha=0;
		for(cha=0; cha < imc->ChannelCount; cha++)
		{
			unsigned int MRs=0, RANK_TIMING_B=0, BANK_TIMING=0, REFRESH_TIMING=0;

			outl(PCI_CONFIG_ADDRESS(0xff, (dev + cha), func, 0x70), 0xcf8);
			MRs=inl(0xcfc);
			outl(PCI_CONFIG_ADDRESS(0xff, (dev + cha), func, 0x84), 0xcf8);
			RANK_TIMING_B=inl(0xcfc);
			outl(PCI_CONFIG_ADDRESS(0xff, (dev + cha), func, 0x88), 0xcf8);
			BANK_TIMING=inl(0xcfc);
			outl(PCI_CONFIG_ADDRESS(0xff, (dev + cha), func, 0x8c), 0xcf8);
			REFRESH_TIMING=inl(0xcfc);

			imc->Channel[cha].Timing.tCL  =((MRs >> 4) & 0x7) != 0 ? ((MRs >> 4) & 0x7) + 4 : 0;
			imc->Channel[cha].Timing.tRCD =(BANK_TIMING & 0x1E00) >> 9;
			imc->Channel[cha].Timing.tRP  =(BANK_TIMING & 0xF);
			imc->Channel[cha].Timing.tRAS =(BANK_TIMING & 0x1F0) >> 4;
			imc->Channel[cha].Timing.tRRD =(RANK_TIMING_B & 0x1c0) >> 6;
			imc->Channel[cha].Timing.tRFC =(REFRESH_TIMING & 0x1ff);
			imc->Channel[cha].Timing.tWR  =((MRs >> 9) & 0x7) != 0 ? ((MRs >> 9) & 0x7) + 4 : 0;
			imc->Channel[cha].Timing.tRTPr=(BANK_TIMING & 0x1E000) >> 13;
			imc->Channel[cha].Timing.tWTPr=(BANK_TIMING & 0x3E0000) >> 17;
			imc->Channel[cha].Timing.tFAW =(RANK_TIMING_B & 0x3f);
			imc->Channel[cha].Timing.B2B  =(RANK_TIMING_B & 0x1f0000) >> 16;
		}
		iopl(0);
	}
	return(imc);
}
// Release the IMC structure pointers.
void IMC_Free_Info(struct IMCINFO *imc)
{
	if(imc!=NULL)
	{
		if(imc->Channel!=NULL)
			free(imc->Channel);
		free(imc);
	}
}

#define	_IS_MDI_ (A->L.MDI != 0)

// All-in-One function to print a string filled with some New Line terminated texts.
XMAXPRINT XPrint(Display *display, Drawable drawable, GC gc, int x, int y, char *NewLineStr, int spacing) {
	char *pStartLine=NewLineStr, *pNewLine=NULL;
	XMAXPRINT  max={0,0};
	while((pNewLine=strchr(pStartLine,'\n')) != NULL) {
		int cols=pNewLine - pStartLine;
		XDrawString(	display, drawable, gc,
				x,
				y + (spacing * max.rows),
				pStartLine, cols);
		max.cols=(cols > max.cols) ? cols : max.cols;
		max.rows++ ;
		pStartLine=pNewLine+1;
	}
	return(max);
}

// Adjust window size & inform WM.
void	SizeWidget(uARG *A, int G) {
	XSizeHints *hints=NULL;
	if((hints=XAllocSizeHints()) != NULL) {
		hints->min_width= hints->max_width= A->W[G].width;
		hints->min_height=hints->max_height=A->W[G].height;
		hints->flags=PMinSize|PMaxSize;
		XSetWMNormalHints(A->display, A->W[G].window, hints);
		XFree(hints);
	}
	XWindowAttributes xwa={0};
	XGetWindowAttributes(A->display, A->W[G].window, &xwa);
	if((xwa.width != A->W[G].width) || (xwa.height != A->W[G].height))
		XResizeWindow(A->display, A->W[G].window, A->W[G].width, A->W[G].height);
}

// Scale the MDI window.
void	ScaleMDI(uARG *A) {
	int G=0, RightMost=0, BottomMost=0;
	for(G=FIRST_WIDGET; G < LAST_WIDGET; G++) {
		if((A->W[RightMost].x + A->W[RightMost].width) < (A->W[G].x + A->W[G].width))
			RightMost=G;
		if((A->W[BottomMost].y + A->W[BottomMost].height) < (A->W[G].y + A->W[G].height))
			BottomMost=G;
	}
	A->W[MAIN].width=A->W[RightMost].x + A->W[RightMost].width + A->L.margin.H;
	A->W[MAIN].height=A->W[BottomMost].y + A->W[BottomMost].height + (A->L.margin.V >> 1);
	// Adjust the axes with the refreshed width.
	A->L.axes[MAIN][0].x2=A->W[MAIN].width;
}

// Create the X-Window Widget.
int	OpenWidgets(uARG *A) {
	int noerr=true;
	char str[sizeof(HDSIZE)];

	// Allocate memory for chart elements.
	A->L.usage.C0=malloc(A->P.CPU * sizeof(XRectangle));
	A->L.usage.C3=malloc(A->P.CPU * sizeof(XRectangle));
	A->L.usage.C6=malloc(A->P.CPU * sizeof(XRectangle));

	if((A->display=XOpenDisplay(NULL)) && (A->screen=DefaultScreenOfDisplay(A->display)) )
		{
		XSetWindowAttributes swa={
	/* Pixmap: background, None, or ParentRelative	*/	background_pixmap:None,
	/* unsigned long: background pixel		*/	background_pixel:BlackPixel(A->display, DefaultScreen(A->display)),
	/* Pixmap: border of the window or CopyFromParent */	border_pixmap:CopyFromParent,
	/* unsigned long: border pixel value */			border_pixel:WhitePixel(A->display, DefaultScreen(A->display)),
	/* int: one of bit gravity values */			bit_gravity:0,
	/* int: one of the window gravity values */		win_gravity:0,
	/* int: NotUseful, WhenMapped, Always */		backing_store:DoesBackingStore(DefaultScreenOfDisplay(A->display)),
	/* unsigned long: planes to be preserved if possible */	backing_planes:AllPlanes,
	/* unsigned long: value to use in restoring planes */	backing_pixel:0,
	/* Bool: should bits under be saved? (popups) */	save_under:DoesSaveUnders(DefaultScreenOfDisplay(A->display)),
	/* long: set of events that should be saved */		event_mask:EventMaskOfScreen(DefaultScreenOfDisplay(A->display)),
	/* long: set of events that should not propagate */	do_not_propagate_mask:0,
	/* Bool: boolean value for override_redirect */		override_redirect:False,
	/* Colormap: color map to be associated with window */	colormap:DefaultColormap(A->display, DefaultScreen(A->display)),
	/* Cursor: cursor to be displayed (or None) */		cursor:None};

		// Try to load the requested font.
		if(strlen(A->fontName) == 0)
			strcpy(A->fontName, "Fixed");

		if((A->xfont=XLoadQueryFont(A->display, A->fontName)) == NULL)
			noerr=false;

		int G=0;
		for(G=0; noerr && (G < LAST_WIDGET); G++) {
			// Dispose Widgets from each other : [Right & Bottom + width & height] Or -[1,-1] = X,Y origin + Margins.
			int U=A->W[G].x;
			int V=A->W[G].y;
			A->W[G].x=(U == -1) ? A->W[MAIN].x + A->L.margin.H : A->W[U].x + A->W[U].width + A->L.margin.H;
			A->W[G].y=(V == -1) ? A->W[MAIN].y + A->L.margin.V : A->W[V].y + A->W[V].height + A->L.margin.V;
			// Define the Widgets.
			if((A->W[G].window=XCreateWindow(A->display,
							_IS_MDI_ && (G != MAIN) ?
							A->W[MAIN].window
							: DefaultRootWindow(A->display),
							A->W[G].x, A->W[G].y, A->W[G].width, A->W[G].height,
							_IS_MDI_ ? 1 : 0,
							CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect, &swa)) )
				{
				if((A->W[G].gc=XCreateGC(A->display, A->W[G].window, 0, NULL)))
					{
					XSetFont(A->display, A->W[G].gc, A->xfont->fid);

					switch(G) {
						case MAIN: {
							// Compute Window scaling.
							XTextExtents(	A->xfont, HDSIZE, MAIN_WIDTH,
									&A->W[G].extents.dir, &A->W[G].extents.ascent,
									&A->W[G].extents.descent, &A->W[G].extents.overall);

							A->W[G].extents.charWidth=A->xfont->max_bounds.rbearing
										- A->xfont->min_bounds.lbearing;
							A->W[G].extents.charHeight=A->W[G].extents.ascent
											+ A->W[G].extents.descent;
							A->W[G].width=	A->W[G].extents.overall.width;
							A->W[G].height=	(A->W[G].extents.charWidth >> 1)
									+ A->W[G].extents.charHeight * MAIN_HEIGHT;

							// Prepare the chart axes.
							A->L.axes[G]=malloc(sizeof(XSegment));
							A->L.axes[G][0].x1=0;
							A->L.axes[G][0].y1=A->W[G].extents.charHeight + (A->W[G].extents.charHeight >> 1) - 2;
							A->L.axes[G][0].x2=A->W[G].width;
							A->L.axes[G][0].y2=A->L.axes[G][0].y1;

							// First run : if MAIN defined as the MDI then reset its position.
							if(_IS_MDI_) {
								A->W[G].x=0;
								A->W[G].y=0;
							}
							// First run : adjust the global margins with the font size. Don't overlap axes.
							A->L.margin.H=A->W[G].extents.charWidth << 1;
							A->L.margin.V=A->W[G].extents.charHeight << 1;
						}
							break;
						case CORES: {
							// Compute Window scaling.
							XTextExtents(	A->xfont, HDSIZE, A->P.Turbo.MaxRatio_1C << 1,
									&A->W[G].extents.dir, &A->W[G].extents.ascent,
									&A->W[G].extents.descent, &A->W[G].extents.overall);

							A->W[G].extents.charWidth=A->xfont->max_bounds.rbearing
										- A->xfont->min_bounds.lbearing;
							A->W[G].extents.charHeight=A->W[G].extents.ascent
										 + A->W[G].extents.descent;
							A->W[G].width=	A->W[G].extents.overall.width
									+ (A->W[G].extents.charWidth << 2)
									+ (A->W[G].extents.charWidth >> 1);
							A->W[G].height=	(A->W[G].extents.charWidth >> 1)
									+ A->W[G].extents.charHeight * (A->P.CPU + 2);

							// Prepare the chart axes.
							A->L.axes[G]=malloc((A->P.Turbo.MaxRatio_1C + 1) * sizeof(XSegment));
							int i=0, j=A->W[G].extents.overall.width / A->P.Turbo.MaxRatio_1C;
							for(i=0; i <= A->P.Turbo.MaxRatio_1C; i++) {
								A->L.axes[G][i].x1=(j * i) + (A->W[G].extents.charWidth * 3);
								A->L.axes[G][i].y1=3 + A->W[G].extents.charHeight;
								A->L.axes[G][i].x2=A->L.axes[G][i].x1;
								A->L.axes[G][i].y2=((A->P.CPU + 1) * A->W[G].extents.charHeight) - 3;
								}
						}
							break;
						case CSTATES: {
							// Compute Window scaling.
							XTextExtents(	A->xfont, HDSIZE, A->P.CPU * 3,
									&A->W[G].extents.dir, &A->W[G].extents.ascent,
									&A->W[G].extents.descent, &A->W[G].extents.overall);

							A->W[G].extents.charWidth=A->xfont->max_bounds.rbearing
										- A->xfont->min_bounds.lbearing;
							A->W[G].extents.charHeight=A->W[G].extents.ascent
											+ A->W[G].extents.descent;
							A->W[G].width=	(A->W[G].extents.overall.width << 1)
									+ (A->W[G].extents.charWidth << 1);
							A->W[G].height=	(A->W[G].extents.charWidth >> 1)
									+ (A->W[G].extents.charHeight << 1)
									+ (A->W[G].extents.charHeight * CSTATES_HEIGHT);

							// Prepare the chart axes.
							A->L.axes[G]=malloc((10 + 1) * sizeof(XSegment));
							int i=0;
							for(i=0; i <= 10; i++) {
								A->L.axes[G][i].x1=0;
								A->L.axes[G][i].y1=(i + 1) * A->W[G].extents.charHeight;
								A->L.axes[G][i].x2=(A->W[G].extents.overall.width << 1);
								A->L.axes[G][i].y2=A->L.axes[G][i].y1;
							}
						}
							break;
						case TEMPS: {
							// Compute Window scaling.
							int	amplitude=A->P.Features.ThreadCount,
								history=amplitude << 2;

							XTextExtents(	A->xfont, HDSIZE, history,
									&A->W[G].extents.dir, &A->W[G].extents.ascent,
									&A->W[G].extents.descent, &A->W[G].extents.overall);

							A->W[G].extents.charWidth=A->xfont->max_bounds.rbearing
										- A->xfont->min_bounds.lbearing;
							A->W[G].extents.charHeight=A->W[G].extents.ascent
											+ A->W[G].extents.descent;
							A->W[G].width=	A->W[G].extents.overall.width + (A->W[G].extents.charWidth * 5);
							A->W[G].height=	(A->W[G].extents.charWidth >> 1)
									+ A->W[G].extents.charHeight * (amplitude + 1 + 1);

							// Prepare the chart axes.
							A->L.axes[G]=malloc(history * sizeof(XSegment));
							int i=0;
							for(i=0; i < history; i++) {
								A->L.axes[G][i].x1=(i + 3) * A->W[G].extents.charWidth;
								A->L.axes[G][i].y1=(amplitude + 1) * A->W[G].extents.charHeight;
								A->L.axes[G][i].x2=A->L.axes[G][i].x1 + A->W[G].extents.charWidth;
								A->L.axes[G][i].y2=A->L.axes[G][i].y1;
							}
						}
							break;
						case SYSINFO: {
							// Compute Window scaling.
							XTextExtents(	A->xfont, HDSIZE, SYSINFO_WIDTH,
									&A->W[G].extents.dir, &A->W[G].extents.ascent,
									&A->W[G].extents.descent, &A->W[G].extents.overall);

							A->W[G].extents.charWidth=A->xfont->max_bounds.rbearing
										- A->xfont->min_bounds.lbearing;
							A->W[G].extents.charHeight=A->W[G].extents.ascent
											+ A->W[G].extents.descent;
							A->W[G].width=	A->W[G].extents.overall.width;
							A->W[G].height=	(A->W[G].extents.charWidth >> 1)
									+ A->W[G].extents.charHeight * SYSINFO_HEIGHT;

							// Prepare the chart axes.
							A->L.axes[G]=malloc(sizeof(XSegment));
							A->L.axes[G][0].x1=0;
							A->L.axes[G][0].y1=A->W[G].extents.charHeight + (A->W[G].extents.charHeight >> 1) - 1;
							A->L.axes[G][0].x2=A->W[G].width;
							A->L.axes[G][0].y2=A->W[G].extents.charHeight + (A->W[G].extents.charHeight >> 1) - 1;
						}
							break;
					}
					SizeWidget(A, G);
					XSetWindowBorder(A->display, A->W[G].window, A->W[G].foreground);
				}
				else	noerr=false;
			}
			else	noerr=false;
		}
		if(noerr && _IS_MDI_) {
			ScaleMDI(A);
			SizeWidget(A, MAIN);
		}
		for(G=0; noerr && (G < LAST_WIDGET); G++)
			if((A->W[G].pixmap.B=XCreatePixmap(A->display, A->W[G].window,
								A->W[G].width, A->W[G].height,
								DefaultDepthOfScreen(A->screen)))
			&& (A->W[G].pixmap.F=XCreatePixmap(A->display, A->W[G].window,
								A->W[G].width, A->W[G].height,
								DefaultDepthOfScreen(A->screen))) )
				XSelectInput(A->display, A->W[G].window , VisibilityChangeMask
									| ExposureMask
									| KeyPressMask
									| StructureNotifyMask);
			else	noerr=false;
	}
	else	noerr=false;

	// Prepare a Wallboard string with the Processor information.
	sprintf(str, OVERCLOCK, A->P.Features.BrandString, A->P.Platform.MaxNonTurboRatio * A->P.ClockSpeed);
	A->L.wbLength=strlen(str) + (A->P.Platform.MaxNonTurboRatio << 2);
	A->L.wbString=calloc(A->L.wbLength, 1);
	memset(A->L.wbString, 0x20, A->L.wbLength);
	memcpy(&A->L.wbString[A->P.Platform.MaxNonTurboRatio << 1], str, strlen(str));
	A->L.wbScroll=(A->P.Platform.MaxNonTurboRatio << 1);
	A->L.wbLength=strlen(str) + A->L.wbScroll;

	// Store some Ratios into a string for future chart drawing.
	sprintf(A->L.bump, "%02d%02d%02d",	A->P.Platform.MinimumRatio,
						A->P.Platform.MaxNonTurboRatio,
						A->P.Turbo.MaxRatio_1C);
	return(noerr);
}

// Center the layout of the current page.
void	CenterLayout(uARG *A, int G) {
	A->L.Page[G].hScroll=1 ;
	A->L.Page[G].vScroll=1 ;
}

// Display & scroll content into the specified clip rectangle.
void	ScrollLayout(uARG *A, int G, char *items, int spacing, XRectangle *R) {
	XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
	XDrawString(	A->display, A->W[G].pixmap.B, A->W[G].gc,
			A->W[G].extents.charWidth,
			A->W[G].extents.charHeight,
			A->L.Page[G].title,
			strlen(A->L.Page[G].title) );

	XSetForeground(A->display, A->W[G].gc, 0x666666);
	XDrawSegments(A->display, A->W[G].pixmap.B, A->W[G].gc, A->L.axes[G], 1);

	XSetClipRectangles(	A->display, A->W[G].gc,
				0,
				A->W[G].extents.charHeight + (A->W[G].extents.charHeight >> 1),
				R, 1, Unsorted);

	XSetForeground(A->display, A->W[G].gc, 0xf0f0f0);
	A->L.Page[G].max=XPrint(	A->display, A->W[G].pixmap.B, A->W[G].gc,
						A->W[G].extents.charWidth * A->L.Page[G].hScroll,
						A->W[G].extents.charHeight
						+ (A->W[G].extents.charHeight >> 1)
						+ (A->W[G].extents.charHeight * A->L.Page[G].vScroll),
						items,
						spacing);
	XSetClipMask(A->display, A->W[G].gc, None);
}

// Draw the layout background.
void	BuildLayout(uARG *A, int G) {
	XSetBackground(A->display, A->W[G].gc, A->W[G].background);
	// Clear entirely the background.
	XSetForeground(A->display, A->W[G].gc, A->W[G].background);
	XFillRectangle(A->display, A->W[G].pixmap.B, A->W[G].gc, 0, 0, A->W[G].width, A->W[G].height);

	switch(G) {
		case MAIN:
		{
			char items[4096]={0};
			strcpy(items, MENU_FORMAT);
			XRectangle R[]=	{ {
						x:0,
						y:0,
						width:A->W[G].extents.overall.width,
						height:(A->W[G].extents.charWidth >> 1) + A->W[G].extents.charHeight * MAIN_HEIGHT,
					} };
			ScrollLayout(A, G, items, A->W[G].extents.charHeight  + (A->W[G].extents.charHeight >> 1), R);
		}
			break;
		case CORES: {
			char str[sizeof(CORE_NUM)];
			// Draw the axes.
			XSetForeground(A->display, A->W[G].gc, 0x404040);
			XDrawSegments(A->display, A->W[G].pixmap.B, A->W[G].gc, A->L.axes[G], A->P.Turbo.MaxRatio_1C + 1);

			// Draw the Core identifiers, the header, and the footer on the chart.
			XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
			int cpu=0;
			for(cpu=0; cpu < A->P.CPU; cpu++) {
				sprintf(str, CORE_NUM, cpu);
				XDrawString(	A->display, A->W[G].pixmap.B, A->W[G].gc,
						A->W[G].extents.charWidth >> 1,
						( A->W[G].extents.charHeight * (cpu + 1 + 1) ),
						str, strlen(str) );
			}
			XSetForeground(A->display, A->W[G].gc, 0xc0c0c0);
			XDrawString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth >> 2,
					A->W[G].extents.charHeight,
					"Core", 4);
			XDrawString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth >> 2,
					A->W[G].extents.charHeight * (A->P.CPU + 1 + 1),
					"Ratio", 5);
			XDrawString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth * ((5 + 1) * 2),
					A->W[G].extents.charHeight * (A->P.CPU + 1 + 1),
					"5", 1);
			XDrawString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth * ((A->P.Platform.MinimumRatio + 1) * 2),
					A->W[G].extents.charHeight * (A->P.CPU + 1 + 1),
					&A->L.bump[0], 2);
			XDrawString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth * ((A->P.Platform.MaxNonTurboRatio + 1) * 2),
					A->W[G].extents.charHeight * (A->P.CPU + 1 + 1),
					&A->L.bump[2], 2);
			XDrawString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth * ((A->P.Turbo.MaxRatio_1C + 1) * 2),
					A->W[G].extents.charHeight * (A->P.CPU + 1 + 1),
					&A->L.bump[4], 2);
		}
			break;
		case CSTATES:
		{
			char str[sizeof(CORE_NUM)];
			// Draw the axes.
			XSetForeground(A->display, A->W[G].gc, 0x404040);
			XDrawSegments(A->display, A->W[G].pixmap.B, A->W[G].gc, A->L.axes[G], 10 + 1);

			XSetForeground(A->display, A->W[G].gc, 0xc0c0c0);
			XDrawLine(A->display, A->W[G].pixmap.B, A->W[G].gc,
					(A->W[G].extents.overall.width << 1) + (A->W[G].extents.charWidth >> 1),
					A->W[G].extents.charHeight,
					(A->W[G].extents.overall.width << 1) + (A->W[G].extents.charWidth >> 1),
					A->W[G].extents.charHeight + (A->W[G].extents.charHeight * 10) );
			XDrawImageString(A->display, A->W[G].pixmap.B, A->W[G].gc,
					(A->W[G].extents.overall.width << 1) + (A->W[G].extents.charWidth >> 2),
					A->W[G].extents.charHeight,
					"%", 1 );
			XDrawImageString(A->display, A->W[G].pixmap.B, A->W[G].gc,
					(A->W[G].extents.overall.width << 1) - (A->W[G].extents.charWidth >> 2),
					A->W[G].extents.charHeight * (1 + 1),
					"90", 2 );
			XDrawImageString(A->display, A->W[G].pixmap.B, A->W[G].gc,
					(A->W[G].extents.overall.width << 1) - (A->W[G].extents.charWidth >> 2),
					A->W[G].extents.charHeight * (3 + 1),
					"70", 2 );
			XDrawImageString(A->display, A->W[G].pixmap.B, A->W[G].gc,
					(A->W[G].extents.overall.width << 1) - (A->W[G].extents.charWidth >> 2),
					A->W[G].extents.charHeight * (5 + 1),
					"50", 2 );
			XDrawImageString(A->display, A->W[G].pixmap.B, A->W[G].gc,
					(A->W[G].extents.overall.width << 1) - (A->W[G].extents.charWidth >> 2),
					A->W[G].extents.charHeight * (7 + 1),
					"30", 2 );
			XDrawImageString(A->display, A->W[G].pixmap.B, A->W[G].gc,
					(A->W[G].extents.overall.width << 1) - (A->W[G].extents.charWidth >> 2),
					A->W[G].extents.charHeight * (9 + 1),
					"10", 2 );
			XDrawString(A->display, A->W[G].pixmap.B, A->W[G].gc,
					A->W[G].extents.charWidth,
					A->W[G].extents.charHeight
					+ (A->W[G].extents.charHeight * (10 + 1)),
					"~", 1 );

			// Draw the Core identifiers.
			XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
			int cpu=0;
			for(cpu=0; cpu < A->P.CPU; cpu++) {
				sprintf(str, CORE_NUM, cpu);
				XDrawString(	A->display, A->W[G].pixmap.B, A->W[G].gc,
						(A->W[G].extents.charWidth << 1)
						+ ((cpu * 3) * (A->W[G].extents.charWidth << 1)),
						A->W[G].extents.charHeight,
						str, strlen(str) );
			}
		}
			break;
		case TEMPS:
		{
			char str[sizeof(CORE_NUM)];
			int amplitude=A->P.Features.ThreadCount;

			XSetForeground(A->display, A->W[G].gc, 0xc0c0c0);
			XDrawLine(A->display, A->W[G].pixmap.B, A->W[G].gc,
					A->W[G].extents.charWidth + (A->W[G].extents.charWidth >> 1),
					A->W[G].extents.charHeight,
					A->W[G].extents.charWidth + (A->W[G].extents.charWidth >> 1),
					A->W[G].extents.charHeight + (A->W[G].extents.charHeight * amplitude) );
			XDrawImageString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth >> 2,
					A->W[G].extents.charHeight,
					"Core", 4);
			XDrawString(	A->display,
					A->W[G].pixmap.B,
					A->W[G].gc,
					A->W[G].extents.charWidth >> 2,
					A->W[G].extents.charHeight * (amplitude + 1 + 1),
					"Temps", 5);
			// Draw the Core identifiers.
			XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
			int cpu=0;
			for(cpu=0; cpu < A->P.CPU; cpu++) {
				sprintf(str, CORE_NUM, cpu);
				XDrawString(	A->display, A->W[G].pixmap.B, A->W[G].gc,
						(A->W[G].extents.charWidth * 5)
						+ (A->W[G].extents.charWidth >> 1)
						+ (cpu << 1) * (A->W[G].extents.charWidth << 1),
						A->W[G].extents.charHeight,
						str, strlen(str) );
			}
		}
			break;
		case SYSINFO:
		{
			char items[16384]={0}, str[4096]={0};

			const char	powered[2]={'N', 'Y'},
					*enabled[2]={"OFF", "ON"};
			sprintf(items, PROC_FORMAT,
					A->P.Features.BrandString,
					ARCH[A->P.ArchID].Architecture,
					A->P.Features.Std.EAX.ExtFamily + A->P.Features.Std.EAX.Family,
					(A->P.Features.Std.EAX.ExtModel << 4) + A->P.Features.Std.EAX.Model,
					A->P.Features.Std.EAX.Stepping,
					A->P.Features.ThreadCount,
					powered[A->P.Features.Std.EDX.VME],
					powered[A->P.Features.Std.EDX.DE],
					powered[A->P.Features.Std.EDX.PSE],
					powered[A->P.Features.Std.EDX.TSC],
					powered[A->P.Features.Std.EDX.MSR],
					powered[A->P.Features.Std.EDX.PAE],
					powered[A->P.Features.Std.EDX.APIC],
					powered[A->P.Features.Std.EDX.MTRR],
					powered[A->P.Features.Std.EDX.PGE],
					powered[A->P.Features.Std.EDX.MCA],
					powered[A->P.Features.Std.EDX.PAT],
					powered[A->P.Features.Std.EDX.PSE36],
					powered[A->P.Features.Std.EDX.PSN],
					powered[A->P.Features.Std.EDX.DS],
					powered[A->P.Features.Std.EDX.ACPI],
					powered[A->P.Features.Std.EDX.SS],
					powered[A->P.Features.Std.EDX.HTT],
					powered[A->P.Features.Std.EDX.TM1],
					powered[A->P.Features.Std.ECX.TM2],
					powered[A->P.Features.Std.EDX.PBE],
					powered[A->P.Features.Std.ECX.DTES64],
					powered[A->P.Features.Std.ECX.DS_CPL],
					powered[A->P.Features.Std.ECX.VMX],
					powered[A->P.Features.Std.ECX.SMX],
					powered[A->P.Features.Std.ECX.EIST],	enabled[A->P.MiscFeatures.EIST],
					powered[A->P.Features.Std.ECX.CNXT_ID],
					powered[A->P.Features.Std.ECX.FMA],
					powered[A->P.Features.Std.ECX.xTPR],	enabled[!A->P.MiscFeatures.xTPR],
					powered[A->P.Features.Std.ECX.PDCM],
					powered[A->P.Features.Std.ECX.PCID],
					powered[A->P.Features.Std.ECX.DCA],
					powered[A->P.Features.Std.ECX.x2APIC],
					powered[A->P.Features.Std.ECX.TSCDEAD],
					powered[A->P.Features.Std.ECX.XSAVE],
					powered[A->P.Features.Std.ECX.OSXSAVE],
					powered[A->P.Features.Ext.EDX.XD_Bit],	enabled[!A->P.MiscFeatures.XD_Bit],
					powered[A->P.Features.Ext.EDX.PG_1GB],
										enabled[A->P.MiscFeatures.FastStrings],
										enabled[A->P.MiscFeatures.TCC],
										enabled[A->P.MiscFeatures.PerfMonitoring],
										enabled[!A->P.MiscFeatures.BTS],
										enabled[!A->P.MiscFeatures.PEBS],
										enabled[A->P.MiscFeatures.CPUID_MaxVal],
										enabled[!A->P.MiscFeatures.Turbo],
					powered[A->P.Features.Std.EDX.FPU],
					powered[A->P.Features.Std.EDX.CX8],
					powered[A->P.Features.Std.EDX.SEP],
					powered[A->P.Features.Std.EDX.CMOV],
					powered[A->P.Features.Std.EDX.CLFSH],
					powered[A->P.Features.Std.EDX.MMX],
					powered[A->P.Features.Std.EDX.FXSR],
					powered[A->P.Features.Std.EDX.SSE],
					powered[A->P.Features.Std.EDX.SSE2],
					powered[A->P.Features.Std.ECX.SSE3],
					powered[A->P.Features.Std.ECX.SSSE3],
					powered[A->P.Features.Std.ECX.SSE41],
					powered[A->P.Features.Std.ECX.SSE42],
					powered[A->P.Features.Std.ECX.PCLMULDQ],
					powered[A->P.Features.Std.ECX.MONITOR],	enabled[A->P.MiscFeatures.FSM],
					powered[A->P.Features.Std.ECX.CX16],
					powered[A->P.Features.Std.ECX.MOVBE],
					powered[A->P.Features.Std.ECX.POPCNT],
					powered[A->P.Features.Std.ECX.AES],
					powered[A->P.Features.Std.ECX.AVX],
					powered[A->P.Features.Std.ECX.F16C],
					powered[A->P.Features.Std.ECX.RDRAND],
					powered[A->P.Features.Ext.ECX.LAHFSAHF],
					powered[A->P.Features.Ext.EDX.SYSCALL],
					powered[A->P.Features.Ext.EDX.RDTSCP],
					powered[A->P.Features.Ext.EDX.IA64] );

			strcat(items, "\nRAM\n");
			strcat(items, CHA_FORMAT);
			if(A->M != NULL) {
				unsigned cha=0;
				for(cha=0; cha < A->M->ChannelCount; cha++) {
					sprintf(str, CAS_FORMAT,
						cha,
						A->M->Channel[cha].Timing.tCL,
						A->M->Channel[cha].Timing.tRCD,
						A->M->Channel[cha].Timing.tRP,
						A->M->Channel[cha].Timing.tRAS,
						A->M->Channel[cha].Timing.tRRD,
						A->M->Channel[cha].Timing.tRFC,
						A->M->Channel[cha].Timing.tWR,
						A->M->Channel[cha].Timing.tRTPr,
						A->M->Channel[cha].Timing.tWTPr,
						A->M->Channel[cha].Timing.tFAW,
						A->M->Channel[cha].Timing.B2B);
					strcat(items, str);
				}
			}
			else
				strcat(items, "Unknown\n");

			strcat(items, "\nBIOS\n");
			sprintf(str, BIOS_FORMAT, A->P.ClockSpeed);
			strcat(items, str);
			XRectangle R[]=	{      {0,
						0,
						A->W[G].width,
						A->W[G].height - (A->W[G].extents.charHeight + (A->W[G].extents.charHeight >> 1))
					}	};
			ScrollLayout(A, G, items, A->W[G].extents.charHeight, R);
		}
			break;
	}
}

// Release the Widget ressources.
void	CloseWidgets(uARG *A)
{
	XUnloadFont(A->display, A->xfont->fid);

	int G=0;
	for(G=LAST_WIDGET - 1; G >= 0 ; G--) {
		XFreePixmap(A->display, A->W[G].pixmap.B);
		XFreePixmap(A->display, A->W[G].pixmap.F);
		XFreeGC(A->display, A->W[G].gc);
		XDestroyWindow(A->display, A->W[G].window);
		free(A->L.axes[G]);
	}
	XCloseDisplay(A->display);

	free(A->L.wbString);
	free(A->L.usage.C0);
	free(A->L.usage.C3);
	free(A->L.usage.C6);
	free(A->fontName);
}

// Fusion in one map the background and the foreground layouts.
void	MapLayout(uARG *A, int G) {
	XCopyArea(A->display, A->W[G].pixmap.B, A->W[G].pixmap.F, A->W[G].gc, 0, 0, A->W[G].width, A->W[G].height, 0, 0);
}

// Send the map to the display.
void	FlushLayout(uARG *A, int G) {
	XCopyArea(A->display,A->W[G].pixmap.F, A->W[G].window, A->W[G].gc, 0, 0, A->W[G].width, A->W[G].height, 0, 0);
	XFlush(A->display);
}

// An activity pulse blinks during the calculation (red) or when in pause (yellow).
void	DrawPulse(uARG *A, int G) {
	XSetForeground(A->display, A->W[G].gc, (A->L.pulse=!A->L.pulse) ? 0xff0000 : A->W[G].foreground);
	XDrawArc(A->display, A->W[G].pixmap.F, A->W[G].gc,
		A->W[G].width - A->W[G].extents.charWidth - (A->W[G].extents.charWidth >> 1),
		A->W[G].extents.charWidth >> 1,
		A->W[G].extents.charWidth,
		A->W[G].extents.charWidth,
		0, 360 << 8);
}

// Scroll the wallboard.
void	DrawWB(uARG *A, int G) {
	if(A->L.wbScroll < A->L.wbLength)
		A->L.wbScroll++;
	else
		A->L.wbScroll=0;
	// Display the Wallboard.
	XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
	XDrawString(	A->display, A->W[G].pixmap.F, A->W[G].gc,
			(A->W[G].extents.charWidth * 6),
			A->W[G].extents.charHeight,
			&A->L.wbString[A->L.wbScroll], (A->P.Platform.MaxNonTurboRatio << 1));
}

// Draw the layout foreground.
void	DrawLayout(uARG *A, int G) {
	switch(G) {
		case CORES:
		{
			char str[16];
			int cpu=0;
			for(cpu=0; cpu < A->P.CPU; cpu++) {
				// Select a color based ratio.
				if(A->P.Core[cpu].UnhaltedRatio <= A->P.Platform.MinimumRatio)
					XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
				if(A->P.Core[cpu].UnhaltedRatio >  A->P.Platform.MinimumRatio)
					XSetForeground(A->display, A->W[G].gc, 0x009966);
				if(A->P.Core[cpu].UnhaltedRatio >  A->P.Platform.MaxNonTurboRatio)
					XSetForeground(A->display, A->W[G].gc, 0xffa500);
				if(A->P.Core[cpu].UnhaltedRatio >= A->P.Turbo.MaxRatio_4C)
					XSetForeground(A->display, A->W[G].gc, 0xff0000);

				// Draw the Core frequency.
				XFillRectangle(	A->display, A->W[G].pixmap.F, A->W[G].gc,
						A->W[G].extents.charWidth * 3,
						3 + ( A->W[G].extents.charHeight * (cpu + 1) ),
						(A->W[G].extents.overall.width
						* (unsigned long long) (A->P.Core[cpu].UnhaltedRatio * A->P.Core[cpu].State.C0))
						/ A->P.Turbo.MaxRatio_1C,
						A->W[G].extents.charHeight - 3);
				// Display the Core frequency & C-STATE
				if(A->L.hertz) {
					XSetForeground(A->display, A->W[G].gc, 0xdddddd);
					sprintf(str, CORE_FREQ, A->P.Core[cpu].UnhaltedFreq);
					XDrawString(	A->display, A->W[G].pixmap.F, A->W[G].gc,
							(A->W[G].extents.charWidth * 5),
							( A->W[G].extents.charHeight * (cpu + 1 + 1) ),
							str, strlen(str) );
				}
				if(A->L.cStateFlag) {
					XSetForeground(A->display, A->W[G].gc, 0x737373);
					sprintf(str, CORE_STATE,100 * A->P.Core[cpu].State.C0,
									100 * A->P.Core[cpu].State.C3,
									100 * A->P.Core[cpu].State.C6);
					XDrawString(	A->display, A->W[G].pixmap.F, A->W[G].gc,
							(A->W[G].extents.charWidth * 18),
							A->W[G].extents.charHeight * (cpu + 1 + 1),
							str, strlen(str) );
				}
			}
			if(A->L.wallboard)
				DrawWB(A, G);
			if(A->L.activity)
				DrawPulse(A, G);
		}
			break;
		case CSTATES:
		{
			char str[32];
			XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
			int cpu=0;
			for(cpu=0; cpu < A->P.CPU; cpu++) {
				// Prepare the C0 chart.
				A->L.usage.C0[cpu].x=(A->W[G].extents.charWidth >> 1) + ((cpu * 3) * (A->W[G].extents.charWidth << 1));
				A->L.usage.C0[cpu].y=A->W[G].extents.charHeight
							+ (A->W[G].extents.charHeight * (10 - 1)) * (1 - A->P.Core[cpu].State.C0);
				A->L.usage.C0[cpu].width=A->W[G].extents.charWidth;
				A->L.usage.C0[cpu].height=A->W[G].extents.charHeight
							+ (A->W[G].extents.charHeight * (10 - 1)) * A->P.Core[cpu].State.C0;
				// Prepare the C3 chart.
				A->L.usage.C3[cpu].x=A->L.usage.C0[cpu].x + A->W[G].extents.charWidth + (A->W[G].extents.charWidth >> 1);
				A->L.usage.C3[cpu].y=A->W[G].extents.charHeight
							+ (A->W[G].extents.charHeight * (10 - 1)) * (1 - A->P.Core[cpu].State.C3);
				A->L.usage.C3[cpu].width=A->W[G].extents.charWidth;
				A->L.usage.C3[cpu].height=A->W[G].extents.charHeight
							+ (A->W[G].extents.charHeight * (10 - 1)) * A->P.Core[cpu].State.C3;
				// Prepare the C6 chart.
				A->L.usage.C6[cpu].x=A->L.usage.C3[cpu].x + A->W[G].extents.charWidth + (A->W[G].extents.charWidth >> 1);
				A->L.usage.C6[cpu].y=A->W[G].extents.charHeight
							+ (A->W[G].extents.charHeight * (10 - 1)) * (1 - A->P.Core[cpu].State.C6);
				A->L.usage.C6[cpu].width=A->W[G].extents.charWidth;
				A->L.usage.C6[cpu].height=A->W[G].extents.charHeight
							+ (A->W[G].extents.charHeight * (10 - 1)) * A->P.Core[cpu].State.C6;
			}
			// Display the C-State averages.
			sprintf(str, CORE_STATE,
						100 * A->P.Avg.C0,
						100 * A->P.Avg.C3,
						100 * A->P.Avg.C6);
			XDrawString(A->display, A->W[G].pixmap.F, A->W[G].gc,
						(A->W[G].extents.charWidth),
						A->W[G].extents.charHeight
						+ (A->W[G].extents.charHeight * (10 + 1)),
						str, strlen(str) );

			// Draw C0, C3 & C6 states.
			XSetForeground(A->display, A->W[G].gc, 0x6666f0);
			XFillRectangles(A->display, A->W[G].pixmap.F, A->W[G].gc, A->L.usage.C0, A->P.CPU);
			XSetForeground(A->display, A->W[G].gc, 0x6666b0);
			XFillRectangles(A->display, A->W[G].pixmap.F, A->W[G].gc, A->L.usage.C3, A->P.CPU);
			XSetForeground(A->display, A->W[G].gc, 0x666690);
			XFillRectangles(A->display, A->W[G].pixmap.F, A->W[G].gc, A->L.usage.C6, A->P.CPU);
		}
			break;
		case TEMPS:
		{
			char str[16];
			// Update & draw the temperature histogram.
			int	amplitude=A->P.Features.ThreadCount,
				history=amplitude << 2;
			int i=0;
			XSegment *U=&A->L.axes[G][i], *V=&A->L.axes[G][i+1];
			for(i=0; i < (history - 1); i++, U=&A->L.axes[G][i], V=&A->L.axes[G][i+1]) {
				U->x1=V->x1 - A->W[G].extents.charWidth;
				U->x2=V->x2 - A->W[G].extents.charWidth;
				U->y1=V->y1;
				U->y2=V->y2;
			}
			V=&A->L.axes[G][i-1];
			U->x1=(history + 2) * A->W[G].extents.charWidth;
			U->y1=V->y2;
			U->x2=U->x1 + A->W[G].extents.charWidth;
			U->y2=(( (amplitude * A->P.Core[A->P.Hot].ThermStat.DTS)
				/ A->P.Core[A->P.Hot].TjMax.Target) + 2) * A->W[G].extents.charHeight;

			XSetForeground(A->display, A->W[G].gc, 0x6666f0);
			XDrawSegments(A->display, A->W[G].pixmap.F, A->W[G].gc, A->L.axes[G], history);

			int cpu=0;
			for(cpu=0; cpu < A->P.CPU; cpu++) {
				// Display the Core temperature.
				XSetForeground(A->display, A->W[G].gc, A->W[G].foreground);
				sprintf(str, "%3d", A->P.Core[cpu].TjMax.Target - A->P.Core[cpu].ThermStat.DTS);
				XDrawString(	A->display, A->W[G].pixmap.F, A->W[G].gc,
						(A->W[G].extents.charWidth * 5)
						+ (A->W[G].extents.charWidth >> 1)
						+ (cpu << 1) * (A->W[G].extents.charWidth << 1),
						A->W[G].extents.charHeight * (amplitude + 1 + 1),
						str, strlen(str));
			}
			XSetForeground(A->display, A->W[G].gc, 0xc04040);
			// Show Temperature Thresholds
			int Threshold[2]={(( (amplitude * A->P.Core[A->P.Hot].ThermIntr.Threshold1)
					/ A->P.Core[A->P.Hot].TjMax.Target) + 2) * A->W[G].extents.charHeight,
					(( (amplitude * A->P.Core[A->P.Hot].ThermIntr.Threshold2)
					/ A->P.Core[A->P.Hot].TjMax.Target) + 2) * A->W[G].extents.charHeight};
			XDrawLine(	A->display, A->W[G].pixmap.F, A->W[G].gc,
					A->W[G].extents.charWidth << 1,
					Threshold[0],
					(A->W[G].extents.charWidth << 2) + (A->P.CPU << 1) * (A->W[G].extents.charWidth << 1),
					Threshold[1]);
			XSetForeground(A->display, A->W[G].gc, 0xc0c0c0);
			XDrawString(A->display,
					A->W[G].pixmap.F,
					A->W[G].gc,
					A->W[G].extents.charWidth << 2,
					Threshold[0],
					"T#1", 3);
			XDrawString(A->display,
					A->W[G].pixmap.F,
					A->W[G].gc,
					(A->P.CPU << 1) * (A->W[G].extents.charWidth << 1),
					Threshold[1],
					"T#2", 3);
			// Display the hottest temperature.
			XSetForeground(A->display, A->W[G].gc, 0xffa500);
			sprintf(str, "%2d", A->P.Core[A->P.Hot].TjMax.Target - A->P.Core[A->P.Hot].ThermStat.DTS);
			XDrawImageString(A->display,
					A->W[G].pixmap.F,
					A->W[G].gc,
					A->W[G].extents.charWidth >> 1,
					U->y2,
					str, 2);
		}
			break;
	}
}

// Update the Widget name with the Top Core frequency and its temperature.
void	UpdateTitle(uARG *A, int G) {
	char str[32];
	switch(G) {
		case MAIN:
			sprintf(str, "X-Freq %s.%s-%s",
				_MAJOR, _MINOR, _NIGHTLY);
			break;
		case CORES:
			sprintf(str, "Core#%d @ %dMHz",
				A->P.Top, A->P.Core[A->P.Top].UnhaltedFreq);
			break;
		case CSTATES:
			sprintf(str, "C-States [%.2f%%] [%.2f%%]", 100 * A->P.Avg.C0, 100 * (A->P.Avg.C3 + A->P.Avg.C6));
			break;
		case TEMPS:
			sprintf(str, "Core#%d @ %dC",
				A->P.Top, A->P.Core[A->P.Hot].TjMax.Target - A->P.Core[A->P.Hot].ThermStat.DTS);
			break;
		case SYSINFO:
			sprintf(str, "Clock @ %dMHz",
				A->P.ClockSpeed);
			break;
	}
	XStoreName(A->display, A->W[G].window, str);
	XSetIconName(A->display, A->W[G].window, str);
}

// The far drawing procedure which paints the foreground.
static void *uDraw(void *uArg) {
	uARG *A=(uARG *) uArg;
	int G=0;
	for(G=0; G < LAST_WIDGET; G++)
		if(!A->PAUSE[G] && !A->L.Page[G].pageable) {
			MapLayout(A, G);
			DrawLayout(A, G);
			UpdateTitle(A, G);
			FlushLayout(A, G);
		}
	// Drawing is done.
	pthread_mutex_unlock(&uDraw_mutex);
	return(NULL);
}

// the main thread which manages the X-Window events loop.
static void *uLoop(uARG *A) {
	XEvent	E={0};
	while(A->LOOP) {
		XNextEvent(A->display, &E);

		int G=0;
		for(G=0; G < LAST_WIDGET; G++)
			if(E.xany.window == A->W[G].window)
				break;

		switch(E.type) {
			case Expose:
				if(!E.xexpose.count)
					FlushLayout(A, G);
				break;
			case KeyPress:
				switch(XLookupKeysym(&E.xkey, 0)) {
					case XK_Escape:
						A->LOOP=false;
						break;
					case XK_Pause:
						for(G=0; G < LAST_WIDGET; G++)
							A->PAUSE[G]=true;
						break;
					case XK_space:
						for(G=0; G < LAST_WIDGET; G++)
							A->PAUSE[G]=false;
						break;
					case XK_Return:
						// Fire the drawing thread.
						if(pthread_mutex_trylock(&uDraw_mutex) == 0)
							pthread_create(&A->TID_Draw, NULL, uDraw, A);
						break;
					case XK_Home:
						if(A->L.alwaysOnTop == false) {
							XRaiseWindow(A->display, A->W[G].window);
							A->L.alwaysOnTop=true;
						}
						break;
					case XK_End:
						if(A->L.alwaysOnTop == true) {
							XLowerWindow(A->display, A->W[G].window);
							A->L.alwaysOnTop=false;
						}
						break;
					case XK_a:
					case XK_A:
						A->L.activity=!A->L.activity;
						break;
					case XK_h:
					case XK_H:
						A->L.hertz=!A->L.hertz;
						break;
					case XK_p:
					case XK_P:
						A->L.cStateFlag=!A->L.cStateFlag;
						break;
					case XK_w:
					case XK_W:
						A->L.wallboard=!A->L.wallboard;
						break;
					case XK_c:
					case XK_C:
						if(A->L.Page[G].pageable) {
							CenterLayout(A, G);
							BuildLayout(A, G);
							MapLayout(A, G);
							FlushLayout(A, G);
					}
						break;
					case XK_Left:
						if(A->L.Page[G].pageable
						&& A->L.Page[G].hScroll < A->L.Page[G].max.cols) {
							A->L.Page[G].hScroll++ ;
							BuildLayout(A, G);
							MapLayout(A, G);
							FlushLayout(A, G);
					}
						break;
					case XK_Right:
						if(A->L.Page[G].pageable
						&& A->L.Page[G].hScroll > -A->L.Page[G].max.cols) {
							A->L.Page[G].hScroll-- ;
							BuildLayout(A, G);
							MapLayout(A, G);
							FlushLayout(A, G);
					}
						break;
					case XK_Up:
						if(A->L.Page[G].pageable
						&& A->L.Page[G].vScroll < A->L.Page[G].max.rows) {
							A->L.Page[G].vScroll++ ;
							BuildLayout(A, G);
							MapLayout(A, G);
							FlushLayout(A, G);
					}
						break;
					case XK_Down:
						if(A->L.Page[G].pageable
						&& A->L.Page[G].vScroll > -A->L.Page[G].max.rows) {
							A->L.Page[G].vScroll-- ;
							BuildLayout(A, G);
							MapLayout(A, G);
							FlushLayout(A, G);
					}
						break;
					case XK_Page_Up:
						if(A->L.Page[G].pageable
						&& A->L.Page[G].vScroll < A->L.Page[G].max.rows) {
							A->L.Page[G].vScroll+=10 ;
							BuildLayout(A, G);
							MapLayout(A, G);
							FlushLayout(A, G);
					}
						break;
					case XK_Page_Down:
						if(A->L.Page[G].pageable
						&& A->L.Page[G].vScroll > -A->L.Page[G].max.rows) {
							A->L.Page[G].vScroll-=10;
							BuildLayout(A, G);
							MapLayout(A, G);
							FlushLayout(A, G);
					}
						break;
					case XK_KP_Add: {
						char str[32];
						if(A->P.IdleTime > 50000)
							A->P.IdleTime-=25000;
						sprintf(str, "[%d usecs]", A->P.IdleTime);
						XSetForeground(A->display, A->W[G].gc, 0xffff00);
						XDrawImageString(A->display, A->W[G].window, A->W[G].gc,
									A->W[G].width >> 1, A->W[G].height >> 1,
									str, strlen(str) );
					}
						break;
					case XK_KP_Subtract: {
						char str[32];
						A->P.IdleTime+=25000;
						sprintf(str, "[%d usecs]", A->P.IdleTime);
						XSetForeground(A->display, A->W[G].gc, 0xffff00);
						XDrawImageString(A->display, A->W[G].window, A->W[G].gc,
									A->W[G].width >> 1, A->W[G].height >> 1,
									str, strlen(str) );
					}
					break;
					case XK_F1:
					case XK_F2:
					case XK_F3:
					case XK_F4:
					case XK_F5: {
						// Convert the function key number into a Widget index.
						G=XLookupKeysym(&E.xkey, 0) - XK_F1;
						// Get Map status.
						XWindowAttributes xwa={0};
						XGetWindowAttributes(A->display, A->W[G].window, &xwa);
						// Hide or unhide the Widget.
						if(xwa.map_state == IsUnmapped)
							XMapWindow(A->display, A->W[G].window);
						else if(_IS_MDI_) {
								if(G != MAIN)
									XUnmapWindow(A->display, A->W[G].window);
							}
							else
								XIconifyWindow(A->display, A->W[G].window, DefaultScreen(A->display));
					}
						break;
				}
				break;
			case DestroyNotify:
				A->LOOP=false;
				break;
			case UnmapNotify:
				A->PAUSE[G]=true;
				break;
			case MapNotify:
				A->PAUSE[G]=false;
				break;
			case VisibilityNotify:
				switch (E.xvisibility.state) {
					case VisibilityUnobscured:
						break;
					case VisibilityPartiallyObscured:
					case VisibilityFullyObscured:
						if(A->L.alwaysOnTop)
							XRaiseWindow(A->display, A->W[G].window);
						break;
				}
				break;
		}
	}
	return(NULL);
}

// Apply the command line arguments.
int	Help(uARG *A, int argc, char *argv[]) {
	OPTION	options[] = {	{"-x", "%d", &A->L.margin.H,      "Left position"                },
				{"-y", "%d", &A->L.margin.V,      "Top position"                 },
				{"-b", "%x", &A->W[MAIN].background, "Background color"          },
				{"-f", "%x", &A->W[MAIN].foreground, "Foreground color"          },
				{"-c", "%ud",&A->P.PerCore,       "Monitor per Thread/Core (0/1)"},
				{"-s", "%ld",&A->P.IdleTime,      "Idle time (usec)"             },
				{"-a", "%ud",&A->L.activity,      "Pulse activity (0/1)"         },
				{"-h", "%ud",&A->L.hertz,         "CPU frequency (0/1)"          },
				{"-p", "%ud",&A->L.cStateFlag,    "C-STATE percentage (0/1)"     },
				{"-t", "%ud",&A->L.alwaysOnTop,   "Always On Top (0/1)"          },
				{"-w", "%ud",&A->L.wallboard,     "Scroll wallboard (0/1)"       },
				{"-D", "%lx",&A->L.MDI,           "Enable MDI Window (0/1)"      },
				{"-F", "%s", A->fontName,         "Font name"                    },
		};
	const int s=sizeof(options)/sizeof(OPTION);
	int uid=geteuid(), i=0, j=0, noerr=true;

	if((argc - ((argc >> 1) << 1)) && (uid == 0)) {
		for(j=1; j < argc; j+=2) {
			for(i=0; i < s; i++)
				if(!strcmp(argv[j], options[i].argument)) {
					sscanf(argv[j+1], options[i].format, options[i].pointer);
					break;
				}
			if(i == s) {
				noerr=false;
				break;
			}
		}
	}
	else
		noerr=false;

	if(noerr == false) {
		printf("Usage: %s [OPTION...]\n\n", argv[0]);
		for(i=0; i < s; i++)
			printf("\t%s\t%s\n", options[i].argument, options[i].manual);
		printf("\nExit status:\n0\tif OK,\n1\tif problems,\n2\tif serious trouble.\n");
		printf("\nRoot Access Granted [%s]\n\nReport bugs to webmaster@cyring.fr\n", (!uid)? "OK":"NO");
	}
	return(noerr);
}

// Verify the prerequisites & start the threads.
int main(int argc, char *argv[]) {
	uARG	A= {
			display:NULL,
			screen:NULL,
			fontName:malloc(sizeof(char)*256),
			P: {
				ArchID:-1,
				ClockSpeed:0,
				CPU:0,
				Core:NULL,
				Top:0,
				Hot:0,
				PerCore:false,
				IdleTime:1000000,
			},
			W: {
				// MAIN
				{
				window:0,
				pixmap: {
					B:0,
					F:0,
				},
				gc:0,
				x:-1,
				y:-1,
				width:300,
				height:150,
				extents: {
					overall:{0},
					dir:0,
					ascent:11,
					descent:2,
					charWidth:6,
					charHeight:13,
				},
				background:0x333333,
				foreground:0x8fcefa,
				},
				// CORES
				{
				window:0,
				pixmap: {
					B:0,
					F:0,
				},
				gc:0,
				x:MAIN,
				y:-1,
				width:300,
				height:150,
				extents: {
					overall:{0},
					dir:0,
					ascent:11,
					descent:2,
					charWidth:6,
					charHeight:13,
				},
				background:0x333333,
				foreground:0x8fcefa,
				},
				// CSTATES
				{
				window:0,
				pixmap: {
					B:0,
					F:0,
				},
				gc:0,
				x:-1,
				y:MAIN,
				width:200,
				height:150,
				extents: {
					overall:{0},
					dir:0,
					ascent:11,
					descent:2,
					charWidth:6,
					charHeight:13,
				},
				background:0x333333,
				foreground:0x8fcefa,
				},
				// TEMPS
				{
				window:0,
				pixmap: {
					B:0,
					F:0,
				},
				gc:0,
				x:CSTATES,
				y:CORES,
				width:150,
				height:150,
				extents: {
					overall:{0},
					dir:0,
					ascent:11,
					descent:2,
					charWidth:6,
					charHeight:13,
				},
				background:0x333333,
				foreground:0x8fcefa,
				},
				// SYSINFO
				{
				window:0,
				pixmap: {
					B:0,
					F:0,
				},
				gc:0,
				x:-1,
				y:CSTATES,
				width:400,
				height:400,
				extents: {
					overall:{0},
					dir:0,
					ascent:11,
					descent:2,
					charWidth:6,
					charHeight:13,
				},
				background:0x333333,
				foreground:0x8fcefa,
				},
			},
			L: {
				// Margins must be initialized with a zero size.
				margin: {
					H:0,
					V:0,
				},
				Page: {
					// MAIN
					{
						pageable: true,
						title: version,
						max: {
							cols:0,
							rows:1,
						},
						hScroll:1,
						vScroll:1,
					},
					// CORES
					{
						pageable: false,
						title: NULL,
						max: {
							cols:0,
							rows:0,
						},
						hScroll:0,
						vScroll:0,
					},
					// CSTATES
					{
						pageable: false,
						title: NULL,
						max: {
							cols:0,
							rows:0,
						},
						hScroll:1,
						vScroll:1,
					},
					// TEMPS
					{
						pageable: false,
						title: NULL,
						max: {
							cols:0,
							rows:0,
						},
						hScroll:1,
						vScroll:1,
					},
					// SYSINFO
					{
						pageable: true,
						title: "System Information",
						max: {
							cols:0,
							rows:1,
						},
						hScroll:1,
						vScroll:1,
					},
				},
				activity:false,
				hertz:true,
				cStateFlag:false,
				alwaysOnTop: false,
				pulse:false,
				wallboard:false,
				wbScroll:0,
				wbLength:0,
				wbString:NULL,
				bump:{0},
				usage: {C0:NULL, C3:NULL, C6:NULL},
				axes:{NULL},
			},
			LOOP: true,
			PAUSE: {false},
			TID_Draw: 0,
		};
	int	rc=0;

	if(Help(&A, argc, argv))
	{
		// Read the CPU Features.
		CPUID(&A.P.Features);
		// Find the Processor Architecture.
		for(A.P.ArchID=ARCHITECTURES; A.P.ArchID >=0 ; A.P.ArchID--)
				if(!(ARCH[A.P.ArchID].Signature.ExtFamily ^ A.P.Features.Std.EAX.ExtFamily)
				&& !(ARCH[A.P.ArchID].Signature.Family ^ A.P.Features.Std.EAX.Family)
				&& !(ARCH[A.P.ArchID].Signature.ExtModel ^ A.P.Features.Std.EAX.ExtModel)
				&& !(ARCH[A.P.ArchID].Signature.Model ^ A.P.Features.Std.EAX.Model))
					break;
		if(!A.P.PerCore) {
			if( (A.P.CPU=Get_ThreadCount()) == 0)
				// Fallback to the CPUID fixed count of threads if unavailable from BIOS.
				A.P.CPU=A.P.Features.ThreadCount;
		}
		else
			if( (A.P.CPU=Get_CoreCount()) == 0)
				A.P.CPU=ARCH[A.P.ArchID].MaxOfCores;

		// Allocate the Cores working structure.
		pthread_mutex_init(&uDraw_mutex, NULL);

		A.P.Core=malloc(A.P.CPU * sizeof(struct THREADS));

		// Open once the MSR gate.
		if( Open_MSR(&A) )
		{
			// Read the bus clock frequency from the BIOS.
			if( (A.P.ClockSpeed=Get_ExternalClock()) == 0)
				// Fallback first to an estimated clock frequency.
				if((A.P.ClockSpeed=FallBack_Freq() / A.P.Platform.MaxNonTurboRatio) == 0)
					// Fallback at least to the default clock.
					if(A.P.ArchID != -1)
						A.P.ClockSpeed=ARCH[A.P.ArchID].ClockSpeed;

			// Read the Integrated Memory Controler information.
			A.M=IMC_Read_Info();

			// Initialize & run the Widget.
			if(XInitThreads() && OpenWidgets(&A))
			{
				int G=0;
				for(G=0; G < LAST_WIDGET; G++) {
					BuildLayout(&A, G);
					MapLayout(&A, G);
					UpdateTitle(&A, G);
					XMapWindow(A.display, A.W[G].window);
				}

				pthread_t TID_Cycle=0;
				if(!pthread_create(&TID_Cycle, NULL, uCycle, &A)) {
					uLoop(&A);
					pthread_join(TID_Cycle, NULL);
				}
				else rc=2;

				CloseWidgets(&A);
			}
			else	rc=2;

			// Release the ressources.
			IMC_Free_Info(A.M);
			Close_MSR(&A);
		}
		else	rc=2;

		free(A.P.Core);
		pthread_mutex_destroy(&uDraw_mutex);
	}
	else	rc=1;
	return(rc);
}
