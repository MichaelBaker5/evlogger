/**
 * EV Datalogger Project
 *
 * Jon Sowman 2014 <js39g13@soton.ac.uk>
 * University of Southampton
 *
 * @file logger.c
 * @author Jon Sowman
 * @addtogroup logger
 * @{
 */

#include <string.h>

#include "HAL_Dogs102x6.h"
#include "accel.h"
#include "logger.h"
#include "adc.h"
#include "ff.h"
#include "uart.h"
#include "system.h"
#include "typedefs.h"
#include "mmc.h"

/**
 * Quick facility to get the used value of a ring buffer
 * @param b A pointer to the buffer which we wish to query
 */
#define rb_getused_m(b) ((b->tail==b->head) ? 0:(b->head - b->tail + b->len) % b->len)

/**
 * Quick facility to get the free value of a ring buffer
 * @param b A pointer to the buffer which we wish to query
 */
#define rb_getfree_m(b) ((b->tail==b->head) ? b->len : (b->tail - b->head + b->len) % b->len)

/**
 * Reset a ring buffer to its original empty state
 * @param b A pointer to the buffer which we wish to query
 */
#define rb_reset_m(b) do {b->tail = b->head = 0} while (0)

static volatile uint32_t time;
static volatile uint8_t logger_running, file_open;
static char s[UART_BUF_LEN];
static char ringbuf[SD_RINGBUF_LEN];
static char writebuf[512];
static RingBuffer sdbuf;

volatile uint16_t a; //FIXME

/**
 * A SampleBuffer to store a single set of readings before they are transferred
 * into the SD transaction buffer.
 */
static volatile SampleBuffer sb;

/**
 * @var FatFs
 * A FATFS filesystem object which we use to handle files and
 * directories on the SD Card.
 * @var fil
 * A pointer to the data file.
 * @var fsz
 * Stores the current size of the data file.
 */
FATFS FatFs;
FIL fil;
DWORD fsz;

/**
 * Set up the hardware for logging functionality, including the configuration
 * of required peripherals such as the ADC and Accelerometer.
 */
void logger_init(void)
{
    // Initialise the ADC with the sample buffer `sb`
    adc_init(&sb);
    Cma3000_init(&sb, &a);

    // Enable LEDs and turn them off (P1.0, P8.1, P8.2)
    P1DIR |= _BV(0);
    P1OUT &= ~_BV(0);
    P8DIR |= _BV(1);
    P8OUT &= ~_BV(1);
    P8DIR |= _BV(2);
    P8OUT &= ~_BV(2);

    // Select the potentiometer and enable the ADC on that channel
    P8DIR |= _BV(0);
    P8OUT |= _BV(0);
    P6SEL |= _BV(5);

    // Enable the buttons
    // S1 is on P1.7, S2 is on P2.2
    S1_PORT_OUT |= S1_PIN;
    S1_PORT_REN |= S1_PIN;
    S2_PORT_OUT |= S2_PIN;
    S2_PORT_REN |= S2_PIN;
    // Trigger on falling edge, clear int flags, enable interupts
    S1_PORT_IES &= ~S1_PIN;
    S1_PORT_IFG &= ~S1_PIN;
    S1_PORT_IE |= S1_PIN;
    S2_PORT_IES &= ~S2_PIN;
    S2_PORT_IFG &= ~S2_PIN;
    S2_PORT_IE |= S2_PIN;

    // Set up 16 bit timer TIMER1 to interrupt at the log frequency
    TA1CCR0 = 24999;

    // Clock from SMCLK with no divider, use "up" mode, use interrupts
    TA1CTL |= TASSEL_2 | TACLR;

    // Enable interrupts on CCR0
    TA1CCTL0 |= CCIE;

    // Enable interrupts (if they're not already)
    eint();

    // The logger should start in its OFF state
    Dogs102x6_clearRow(1);
    Dogs102x6_stringDraw(1, 0, "Logging: OFF", DOGS102x6_DRAW_NORMAL);
    logger_running = 0;

    // Call the SD setup routine (this doesn't terminate)
    sd_setup(&sdbuf);
}


/**
 * Update the LCD with the current status of the logger service. This should
 * not be called too regularly on the MSP-EXP430 board due to the SD card and
 * LCD panel being on the same SPI bus and will cause slowdown of SD
 * transactions.
 * @param buf A pointer to the ring buffer which we are monitoring.
 */
void update_lcd(RingBuffer *buf)
{
    FATFS *fs;
    fs = &FatFs;
    DWORD fre_clust, fre_sect, tot_sect;

    /* Get volume information and free clusters of drive 1 */
    f_getfree("", &fre_clust, &fs);

    /* Get total sectors and free sectors */
    tot_sect = (fs->n_fatent - 2) * fs->csize;
    fre_sect = fre_clust * fs->csize;

    /* Print the free space (assuming 512 bytes/sector) */
    sprintf(s, "%lu/%luMB (%lu%%)", (tot_sect-fre_sect)/2000, 
            tot_sect/2000, (100 - (100*fre_sect)/tot_sect));
    Dogs102x6_clearRow(4);
    Dogs102x6_stringDraw(4, 0, s, DOGS102x6_DRAW_NORMAL);

    // Show bytes in buffer
    sprintf(s, "Buffer: %u%%", (100*rb_getused_m(buf))/buf->len);
    Dogs102x6_clearRow(2);
    Dogs102x6_stringDraw(2, 0, s, DOGS102x6_DRAW_NORMAL);

    // Show size of file
    fsz = f_size(&fil);
    sprintf(s, "File: %lukb", (unsigned long)fsz/1000);
    Dogs102x6_clearRow(3);
    Dogs102x6_stringDraw(3, 0, s, DOGS102x6_DRAW_NORMAL);

    // Monitor buffer overflow
    if(buf->overflow)
        lcd_debug("Buffer overflow");
}

/**
 * Set up the SD card for logging to it.
 * @param sdbuf A pointer to the SD card buffer. This is a ring buffer that we
 * will use to buffer incoming samples before they are logged to the SD card,
 * such that we can write entire sectors at once.
 */
void sd_setup(RingBuffer* sdbuf)
{   
    FRESULT fr;

    // Initialise the ring buffer for SD transfers
    sdbuf->buffer = ringbuf;
    sdbuf->head = sdbuf->tail = sdbuf->overflow = 0;
    sdbuf->len = SD_RINGBUF_LEN;
    sdbuf->mask = sdbuf->len - 1;

    // Wait for an SD card to be inserted
    while(!detectCard())
    {
        _delay_ms(250);
        lcd_debug("Insert SD Card");
    }
    lcd_debug("");

    fr = f_mount(0, &FatFs);
    while( fr != FR_OK )
    {
        sprintf(s, "Mount fail: %d", fr);
        uart_debug(s);
        _delay_ms(100);
        fr = f_mount(0, &FatFs);
    }

    // Now we can begin updating the LCD
    update_lcd(sdbuf);

    while(1)
    {
        // If we just started logging then open the file
        if(logger_running && !file_open)
        {
            fr = f_open(&fil, "data.log", FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
            while( fr != FR_OK )
            {
                _delay_ms(500);
                sprintf(s, "Open fail: %d", fr);
                uart_debug(s);
                fr = f_open(&fil, "data.log", FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
            }
            sdbuf->overflow = 0;
            lcd_debug("");
            file_open = 1;
        }

        // If we just stopped logging then close the file
        if(!logger_running && file_open)
        {
            // Write any remaining data to the disk
            sd_write(sdbuf, writebuf, &fil, rb_getused_m(sdbuf));
            if(f_sync(&fil))
                lcd_debug("sync fail");

            // Close the file
            fr = f_close(&fil);
            while(fr != FR_OK)
            {
                sprintf(s, "close fail: %d", fr);
                lcd_debug(s);
                fr = f_close(&fil);
                _delay_ms(100);
            }
            file_open = 0;
        }

        // Use the fast getused() ring buffer function since we care about
        // speed. Write one sector to the SD card.
        if((rb_getused_m(sdbuf) > 512) && file_open && logger_running)
            sd_write(sdbuf, writebuf, &fil, 512);

        // Update the LCD once every 200ms
        if((clock_time() % 333) == 0)
        {
            //sprintf(s, "%i", sb.accel[0]); //FIXME
            //uart_debug(s);
            update_lcd(sdbuf);
        }
    }
}

/**
 * Write n bytes from a ring buffer to an SD card controlled by fatfs
 * @param rb A pointer to the ring buffer from which we will read the required
 * data.
 * @param writebuf A short buffer where we can temporarily place a single
 * before before writing it to the card.
 * @param n The number of bytes to be written to the card.
 * @param fil A pointer to the file to which we want to write.
 * @return FRESULT The fatfs result code for the write operation.
 */
FRESULT sd_write(RingBuffer *rb, char *writebuf, FIL *fil, uint16_t n)
{
    FRESULT fr;
    UINT bw;

    ringbuf_read(rb, writebuf, n);
    P1OUT |= _BV(0);
    fr = f_write(fil, writebuf, n, &bw);
    
    if(fr)
    {
        sprintf(s, "write fail: %u", fr);
        lcd_debug(s);
    }
    P1OUT &= ~_BV(0);
    return fr;
}

/**
 * Write n bytes to a ring buffer. This is done via a fast memcpy operation
 * and as such, there is logic in this function to transparently handle the
 * copy even if we're wrapping over the boundary of the ring buffer.
 * @param buf A pointer to the ring buffer we want to write to
 * @param data A pointer to the data to be written
 * @param n The number of bytes to be written to the ring buffer
 * @returns 0 for success, non-0 for failure
 */
uint8_t ringbuf_write(RingBuffer *buf, char* data, uint16_t n)
{
    uint16_t rem;

    // Check we're not writing more than the buffer can hold
    if(n >= buf->len)
        return 1;

    // Make sure there's enough free space in the buffer for our data
    if(rb_getfree_m(buf) < n)
    {
        buf->overflow = 1;
        return 1;
    }

    // We can do a single memcpy as long as we don't wrap around the buffer
    if(buf->head + n < buf->len)
    {
        // We won't wrap, we can quickly memcpy
        memcpy(buf->buffer + buf->head, data, n);
        buf->head = (buf->head + n) & buf->mask;
    } else {
        // We're going to wrap, copy in 2 blocks
        // Copy the first (SD_BUF_LEN - buf->head) bytes
        rem = buf->len - buf->head;
        memcpy(buf->buffer + buf->head, data, rem);
        buf->head = (buf->head + rem) & buf->mask;
        // Copy the remaining bytes
        memcpy(buf->buffer + buf->head, data + rem, n - rem);
        buf->head = (buf->head + (n-rem)) & buf->mask;
    }
    return 0;
}

/**
 * Read n bytes from a ring buffer. This is done via a fast memcpy operation
 * and as such, there is logic in this function to transparently handle the
 * copy even if we're wrapping over the boundary of the ring buffer.
 * @param buf A pointer to the ring buffer we want to write to
 * @param read_buffer Copy data into this array
 * @param n The number of bytes to be read from the ring buffer
 * @returns 0 for success, non-0 for failure
 */
uint8_t ringbuf_read(RingBuffer *buf, char* read_buffer, uint16_t n)
{
    uint16_t rem;

    // We can't read more data than the buffer holds!
    if(n >= buf->len)
        return 1;

    // We can't read more bytes than the buffer currently contains
    if(n > rb_getused_m(buf))
        n = rb_getused_m(buf);

    if(buf->tail + n < buf->len)
    {
        // We won't wrap, we can quickly memcpy
        memcpy(read_buffer, buf->buffer + buf->tail, n);
        buf->tail = (buf->tail + n) & buf->mask;
    } else {
        // We're going to wrap, copy in 2 blocks
        // Copy the first (SD_BUF_LEN - buf->head) bytes
        rem = buf->len - buf->tail;
        memcpy(read_buffer, buf->buffer + buf->tail, rem);
        buf->tail = (buf->tail + rem) & buf->mask;
        // Copy the remaining bytes
        memcpy(read_buffer + rem, buf->buffer + buf->tail, n - rem);
        buf->tail = (buf->tail + (n-rem)) & buf->mask;
    }
    return 0;
}

/**
 * Enable TA1 to begin logging by setting mode control to "up" mode,
 * counter counts to TAxCCR0.
 */
void logger_enable(void)
{
    // Stop any timer activity
    TA1CTL &= ~MC_3;

    Dogs102x6_clearRow(1);
    Dogs102x6_stringDraw(1, 0, "Logging: ON", DOGS102x6_DRAW_NORMAL);
    logger_running = 1;

    // Start the timer
    TA1CTL |= MC_1;
}

/**
 * Disable TA1 to halt logging by setting mode control to STOP.
 */
void logger_disable(void)
{
    // Clear bits 4 and 5
    TA1CTL &= ~MC_3;
    logger_running = 0;
    Dogs102x6_clearRow(1);
    Dogs102x6_stringDraw(1, 0, "Logging: OFF", DOGS102x6_DRAW_NORMAL);
}

/**
 * Interrupt for TA1, here we should log one block of data to the SD card.
 * In future we may buffer data and log it in blocks, or transfer it to the
 * SD card via DMA.
 */
interrupt(TIMER1_A0_VECTOR) TIMER1_A0_ISR(void)
{
    // Write the contents of the sample buffer (sb) to the SD buffer
    if(file_open)
    {
        ringbuf_write(&sdbuf, (char *)&sb, sizeof(SampleBuffer));
    }

    // Trigger the next conversion
    adc_convert();
    if((clock_time() % 100) == 0)
        Cma3000_readAccelFSM();
}

/**
 * Interrupt vector for button S1. We should debounce the button press using
 * the system ticks timer, and then enable or disable logging as required.
 */
interrupt(PORT1_VECTOR) PORT1_ISR(void)
{
    // If button S1 was pressed and >250ms has passed...
    if((P1IV & P1IV_P1IFG7) && (clock_time() - time) > 250)
    {
        time = clock_time();
        if(logger_running)
            logger_disable();
        else
            logger_enable();
    }

}

/**
 * Interrupt vector for button S2
 */
interrupt(PORT2_VECTOR) PORT2_ISR(void)
{
    if(P2IV & P2IV_P2IFG2)
    {
        ;
    }
}

/**
 * @}
 */
