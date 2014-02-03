/**
 * EV Datalogger Project
 *
 * Jon Sowman 2013 <js39g13@soton.ac.uk>
 * University of Southampton
 */

#include <msp430.h>
#include <msp430f5529.h>
#include <stdint.h>
#include <stdio.h>

#include "HAL_Dogs102x6.h"
#include "HAL_Cma3000.h"
#include "typedefs.h"
#include "delay.h"
#include "uart.h"
#include "adc.h"
#include "clock.h"

#include "ff.h"
#include "diskio.h"

#define _BV(x) (1<<x)

// Private prototypes
void sys_clock_init(void);
void led_toggle(void);
void update_lcd(void);
    
// Character buffer for LCD and UART debugging
char s[UART_BUF_LEN];

/**
 * main() is run by the micro when execution begins. Call initialisation
 * routines and set up functionality to trigger as appropriate.
 */
int main(void)
{
    // Buffer for reading data from the SD card
    char filebuf[15];

    // Variables required for the operation of the FAT filesystem library
    FATFS FatFs;
    FRESULT fr;
    FIL fil;
    UINT bw;
    DWORD fsz, fre_clust, fre_sect, tot_sect;
    FATFS *fs;

    // Stop the wdt
    WDTCTL = WDTPW | WDTHOLD;

    // Set up the system clock and any required peripherals
    sys_clock_init();
    clock_init();
    uart_init();
    sd_init();
    adc_init();
    Cma3000_init();
    Dogs102x6_init();
    Dogs102x6_backlightInit();

    // Enable LED on P1.0 and turn it off
    P1DIR |= _BV(0);
    P1OUT &= ~_BV(0);

    // Select the potentiometer and enable the ADC on that channel
    P8DIR |= _BV(0);
    P8OUT |= _BV(0);
    P6SEL |= _BV(5);
    adc_select(0x05);

    // Wait for peripherals to boot
    _delay_ms(100);
    
    // Test that minicom/term is behaving
    uart_debug("Hello world");

    // Flash the LED at 1 second
    register_function_1s(&led_toggle);

    // Call the periodic fatfs timer functionality
    register_function_10ms(&disk_timerproc);

    // Test the LCD
    Dogs102x6_setBacklight(6);
    Dogs102x6_setContrast(6);
    Dogs102x6_clearScreen();
    Dogs102x6_stringDraw(0, 0, "=== EV LOGGER ===", DOGS102x6_DRAW_NORMAL);
    
    // Mount the FAT filesystem
    _delay_ms(100);
    fr = f_mount(&FatFs, "", 1);
    while( fr != FR_OK )
    {
        sprintf(s, "Mount fail: %d", fr);
        uart_debug(s);
        _delay_ms(100);
        fr = f_mount(&FatFs, "", 1);
    }

    // Find free space
    fs = &FatFs;
    fr = f_getfree("", &fre_clust, &fs);

    // Get total sectors and free sectors
    tot_sect = (fs->n_fatent - 2) * fs->csize;
    fre_sect = fre_clust * fs->csize;

    // Print the free space (assuming 512 bytes/sector)
    sprintf(s, "%lukiB available, %lukib free", tot_sect/2, fre_sect/2);
    uart_debug(s);
    sprintf(s, "%lu/%luMiB (%lu%%)", ((tot_sect - fre_sect)/2000), 
            (tot_sect/2000), 100-((fre_sect*100)/tot_sect));
    Dogs102x6_stringDraw(1, 0, s, DOGS102x6_DRAW_NORMAL);

    // Attempt to open a file
    fr = f_open(&fil, "hello.txt", FA_READ | FA_WRITE);
    while( fr != FR_OK )
    {
        _delay_ms(500);
        sprintf(s, "Open fail: %d", fr);
        uart_debug(s);
        fr = f_open(&fil, "hello.txt", FA_READ | FA_WRITE);
    }

    // Determine the size of the file
    fsz = f_size(&fil);
    sprintf(s, "file is %d bytes", (int)fsz);
    uart_debug(s);

    // Try and read from the file
    fr = f_read(&fil, filebuf, fsz, &bw);
    sprintf(s, "Read %d bytes, result %d", bw, fr);
    uart_debug(s);
    lcd_debug(filebuf);

    // Try and write something new, move to beginning of file
    fr = f_lseek(&fil, 0);

    // Write something else
    fr = f_write(&fil, "o hai world", 12, &bw);
    sprintf(s, "Wrote %d bytes, result %d", bw, fr);
    uart_debug(s);

    // Close the file
    f_close(&fil);

    // Everything is done with interrupts, so just do nothing here.
    register_function_100ms(&update_lcd);
    while(1);

    return 0;
}

/**
 * This function is run repeatedly to run systems and update the
 * LCD display.
 */
void update_lcd(void)
{
    uint16_t adc_read;

    // Run an ADC conversion and display the result
    adc_read = adc_convert();
    sprintf(s, "ADC: %u", adc_read);
    Dogs102x6_clearRow(2);
    Dogs102x6_stringDraw(2, 0, s, DOGS102x6_DRAW_NORMAL);

    // Run an accelerometer conversion and display the result
    Cma3000_readAccel();
    sprintf(s, "X:%d, Y:%d, Z:%d", Cma3000_xAccel, 
            Cma3000_yAccel, Cma3000_zAccel);
    Dogs102x6_clearRow(3);
    Dogs102x6_stringDraw(3, 0, s, DOGS102x6_DRAW_NORMAL);
}

/**
 * Set up the system clock to use the external crystal as the stabilisation
 * source for the FLL and have MCLK at 20MHz.
 */
void sys_clock_init( void )
{
    uint16_t i;

    // Port select XT2
    P5SEL |= (1 << 2) | (1 << 3);

    // Enable XT2 (4MHz xtal attached to XT2) and disable XT1 (LF & HF)
    UCSCTL6 &= ~XT2OFF;
    UCSCTL6 |= XT1OFF;
    _BIS_SR(OSCOFF); // Disable LFXT1

    // Wait for XT2 to stabilise
    do {
        UCSCTL7 &= ~XT2OFFG;
        for( i = 0xFFF; i > 0; i--);
    } while( UCSCTL7 & XT2OFFG );

    // Set FLL reference to be XT2 divided by 2 (FLLREFDIV=2)
    UCSCTL3 = SELREF__XT2CLK | FLLREFDIV__2;

    // Set the FLL loop divider to 4 (D=4) and the multiplier to 5 (N=4)
    // DCOCLK = D * (N+1) * (FLLREFCLK / FLLREFDIV)
    UCSCTL2 = 0x0004;
    UCSCTL2 |= FLLD__4; // compensate for N=0 disallowed

    // Set the DCO to range 4 (1.3 - 28.2MHz, target 20MHz)
    UCSCTL1 = DCORSEL_4;

    // Wait until the DCO has stabilised
    do {
        UCSCTL7 &= ~DCOFFG;
        for( i = 0xFFF; i > 0; i--);
    } while( UCSCTL7 & DCOFFG );

    // At this point, DCOCLK is a 20MHz stabilised reference
    // So set MCLK and SMCLK to use this
    UCSCTL4 = SELS_3 | SELM_3;
}

/**
 * Toggle the LED on P1.0, requires already set as output
 */
void led_toggle(void)
{
    P1OUT ^= _BV(0);
}


