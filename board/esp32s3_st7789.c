/* Waveshare ESP32-S3-Touch-LCD-3.5: ST7796 lcd_dev, unified shadow bit-bang. */
#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <nuttx/arch.h>
#include <nuttx/signal.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/fb.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/ioexpander/ioexpander.h>
#include <nuttx/ioexpander/pca9557.h>
#include "esp32s3_i2c.h"
#include "esp32s3_gpio.h"
#include "esp32s3_dma.h"
#include "esp32s3_spi.h"
#include <nuttx/spi/spi.h>
#include <nuttx/kmalloc.h>
#include <nuttx/input/ft5x06.h>

#define PIN_MOSI 1
#define PIN_CLK  5
#define PIN_DC   3
#define PIN_BL   6
#define XRES 480
#define YRES 320
#define MOSI_M (1U<<PIN_MOSI)
#define CLK_M  (1U<<PIN_CLK)
#define DC_M   (1U<<PIN_DC)
#define REG_OUT 0x60004004U
static inline void wreg(uint32_t a, uint32_t v){ *(volatile uint32_t *)a = v; }
static uint32_t g_out;   /* shadow: BL(+others) set, MOSI/CLK/DC cleared */

struct wslcd_s { struct lcd_dev_s dev; int power; uint16_t runbuf[XRES]; };
static struct wslcd_s g_lcd;
static struct pca9557_config_s g_tca = { 0x20, 100000, NULL };

static int axp_wr(struct i2c_master_s *i,uint8_t r,uint8_t v){struct i2c_msg_s m;uint8_t b[2];b[0]=r;b[1]=v;m.frequency=400000;m.addr=0x34;m.flags=0;m.buffer=b;m.length=2;return I2C_TRANSFER(i,&m,1);}
static void axp_power(struct i2c_master_s *i){axp_wr(i,0x80,0x01);axp_wr(i,0x82,(3300-1500)/100);axp_wr(i,0x90,0x00);axp_wr(i,0x91,0x00);nxsig_usleep(5000);axp_wr(i,0x92,(3300-500)/100);axp_wr(i,0x96,(1500-500)/100);axp_wr(i,0x97,(2800-500)/100);axp_wr(i,0x90,0x31);nxsig_usleep(120000);}

/* ---- PCF85063 RTC on the same I2C bus (addr 0x51) -------------------------
 * The board has a real battery-backed RTC chip. We drive it DIRECTLY over I2C
 * instead of via NuttX's CONFIG_RTC framework -- enabling that framework pulls
 * in ESP32S3_RT_TIMER and kills the USB CDC console (proven, 3 attempts). This
 * way we get persistent time with ZERO config changes and no USB risk.
 * Time regs 0x04..0x0A = sec(+OS flag) min hour day weekday month year, BCD. */
#define PCF_ADDR 0x51
static struct i2c_master_s *g_rtc_i2c;

static int pcf_rd(struct i2c_master_s *i, uint8_t reg, uint8_t *buf, int len)
{
  struct i2c_msg_s m[2];
  m[0].frequency=400000; m[0].addr=PCF_ADDR; m[0].flags=0;
  m[0].buffer=&reg;      m[0].length=1;
  m[1].frequency=400000; m[1].addr=PCF_ADDR; m[1].flags=I2C_M_READ;
  m[1].buffer=buf;       m[1].length=len;
  return I2C_TRANSFER(i, m, 2);
}
static int pcf_wr(struct i2c_master_s *i, uint8_t reg, const uint8_t *buf, int len)
{
  struct i2c_msg_s m; uint8_t b[8];
  if (len > 7) return -1;
  b[0] = reg; memcpy(&b[1], buf, len);
  m.frequency=400000; m.addr=PCF_ADDR; m.flags=0; m.buffer=b; m.length=len+1;
  return I2C_TRANSFER(i, &m, 1);
}
static inline int     bcd2dec(uint8_t v){ return (v >> 4) * 10 + (v & 0x0f); }
static inline uint8_t dec2bcd(int v)    { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* civil date <-> days since 1970-01-01 (no localtime/mktime needed) */
static long days_from_civil(int y, int m, int d)
{
  y -= (m <= 2);
  long era = (y >= 0 ? y : y - 399) / 400;
  int  yoe = (int)(y - era * 400);
  int  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + doe - 719468;
}
static void civil_from_days(long z, int *y, int *m, int *d)
{
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  int  doe = (int)(z - era * 146097);
  int  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy  = yoe + era * 400;
  int  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int  mp  = (5 * doy + 2) / 153;
  int  dd  = doy - (153 * mp + 2) / 5 + 1;
  int  mm  = mp + (mp < 10 ? 3 : -9);
  *y = (int)(yy + (mm <= 2)); *m = mm; *d = dd;
}

/* Persist the given time into the RTC chip (velapaw calls this on Set Clock). */
int board_rtc_settime(time_t t)
{
  uint8_t b[7]; int y, mo, d;
  long days = (long)(t / 86400);
  int  rem  = (int)(t % 86400);
  if (g_rtc_i2c == NULL) return -1;
  civil_from_days(days, &y, &mo, &d);
  b[0] = dec2bcd(rem % 60);          /* seconds; bit7 OS=0 marks time valid */
  b[1] = dec2bcd((rem / 60) % 60);
  b[2] = dec2bcd(rem / 3600);        /* 24h */
  b[3] = dec2bcd(d);
  b[4] = 0;                          /* weekday (unused) */
  b[5] = dec2bcd(mo);
  b[6] = dec2bcd((y - 2000) % 100);
  return pcf_wr(g_rtc_i2c, 0x04, b, 7);
}

/* Read the RTC chip. 0 = ok, -1 = absent or oscillator stopped (never set). */
static int board_rtc_gettime(time_t *t)
{
  uint8_t b[7]; int y, mo, d, hh, mi, ss;
  if (g_rtc_i2c == NULL) return -1;
  if (pcf_rd(g_rtc_i2c, 0x04, b, 7) < 0) return -1;
  if (b[0] & 0x80) return -1;        /* OS flag: clock lost power, invalid */
  ss = bcd2dec(b[0] & 0x7f);
  mi = bcd2dec(b[1] & 0x7f);
  hh = bcd2dec(b[2] & 0x3f);
  d  = bcd2dec(b[3] & 0x3f);
  mo = bcd2dec(b[5] & 0x1f);
  y  = 2000 + bcd2dec(b[6]);
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23) return -1;
  *t = (time_t)(days_from_civil(y, mo, d) * 86400L + hh * 3600 + mi * 60 + ss);
  return 0;
}

/* unified shadow bit-bang: always ends CLK low; D/C carried in the shadow */
static inline void clock_byte(uint8_t b, uint32_t base)
{ int i; for(i=0;i<8;i++){ uint32_t o=base|((b&0x80)?MOSI_M:0); b<<=1; wreg(REG_OUT,o); wreg(REG_OUT,o|CLK_M);} }
static void cmd(uint8_t c){ uint32_t base=g_out;       wreg(REG_OUT,base); clock_byte(c,base); wreg(REG_OUT,base); }
static void dat(uint8_t d){ uint32_t base=g_out|DC_M;  wreg(REG_OUT,base); clock_byte(d,base); wreg(REG_OUT,base); }
static void bb_window(int x0,int x1,int y0,int y1){
  cmd(0x2A);dat(x0>>8);dat(x0&0xff);dat(x1>>8);dat(x1&0xff);
  cmd(0x2B);dat(y0>>8);dat(y0&0xff);dat(y1>>8);dat(y1&0xff);
  cmd(0x2C);
}
static void fill_be(uint8_t hi,uint8_t lo,long npix){ uint32_t base=g_out|DC_M; long n; for(n=0;n<npix;n++){clock_byte(hi,base);clock_byte(lo,base);} wreg(REG_OUT,base); }

/* --- Hardware SPI2 probe: does the peripheral actually drive GPIO1/5? --- */
static struct spi_dev_s *g_spi;
/* IO-expander handle kept so the capture build can hardware-reset the panel
 * AFTER the pads have been handed to SPI (the panel's CS is tied low, so a
 * hardware reset is the only way to re-sync its bit counter). */
static struct ioexpander_dev_s *g_ioe;

/* VelaPaw: speaker-amp enable.  The NS4150B's PA_CTRL is EXIO7 on this same
 * TCA9554 -- it is NOT a GPIO, so the ES8311 driver has no way to reach it and
 * playback is silent until someone asserts it here.  Capture (the wake-word
 * path) needs nothing; only playback does.  Exposed so the app can gate the
 * amp per-playback later -- leaving it on idles the amp and can hiss.
 */

void board_speaker_enable(bool on)
{
  if (g_ioe == NULL)
    {
      return;
    }

  IOEXP_SETDIRECTION(g_ioe, 7, IOEXPANDER_DIRECTION_OUT);
  IOEXP_WRITEPIN(g_ioe, 7, on);
}

/* Wait for SPI2 to finish the current transaction before flipping DC, so DC
 * never changes mid-byte (SPI_CMD_REG(2)=0x60024000, SPI_USR=bit24). */
static inline void spi_idle(void){ while(*(volatile uint32_t *)0x60024000U & (1U<<24)); }
static void spi_cmd(uint8_t c){ uint8_t b=c; spi_idle(); esp32s3_gpiowrite(PIN_DC,false); SPI_SNDBLOCK(g_spi,&b,1); }
static void spi_dat(uint8_t d){ uint8_t b=d; spi_idle(); esp32s3_gpiowrite(PIN_DC,true);  SPI_SNDBLOCK(g_spi,&b,1); }
static void spi_dc_data(void){ spi_idle(); esp32s3_gpiowrite(PIN_DC,true); }  /* DC->data after cmds drain */
/* Send the 4 address params as ONE block instead of 4 separate 1-byte
 * SPI_SNDBLOCK calls: each call carries fixed driver overhead that dwarfs the
 * 0.1us the byte itself takes, and LVGL sets a window on every putarea. This
 * takes the window from 11 SPI calls down to 5. */
static void spi_params(uint8_t a, uint8_t b, uint8_t c, uint8_t d){
  uint8_t p[4]; p[0]=a; p[1]=b; p[2]=c; p[3]=d;
  spi_idle(); esp32s3_gpiowrite(PIN_DC,true); SPI_SNDBLOCK(g_spi,p,4);
}
static void spi_window(int x0,int x1,int y0,int y1){
  spi_cmd(0x2A); spi_params(x0>>8, x0&0xff, x1>>8, x1&0xff);
  spi_cmd(0x2B); spi_params(y0>>8, y0&0xff, y1>>8, y1&0xff);
  spi_cmd(0x2C);
}
static uint8_t g_swap[XRES*2];   /* per-row byte-swap buffer for HW-SPI writes */
/* Per-operation lock + config, in the CALLING task (standard NuttX SPI-LCD
 * pattern). Fixes windowed writes failing when putarea runs in a task other
 * than the one that inited the bus. */
/* 40 MHz = APB(80)/2, the fastest divider the ESP32-S3 offers below sysclk.
 * A full 480x320 repaint is 307200 B: ~246 ms at 10 MHz vs ~61 ms at 40 MHz.
 * NOTE: an early note claimed "40 MHz is too fast" -- but that predates finding
 * the real bug (the CS-tied-low panel desyncing at the pad handover), so it was
 * probably a misdiagnosis of the same failure. If the panel shows corruption,
 * step down: 26.7 MHz (80/3), 20 MHz (80/4), 16 MHz (80/5). */
static void spi_begin(void){ SPI_LOCK(g_spi,true); SPI_SETMODE(g_spi,SPIDEV_MODE0); SPI_SETBITS(g_spi,8); SPI_SETFREQUENCY(g_spi,40000000); }
static void spi_end(void){ SPI_LOCK(g_spi,false); }
static void spi_lcd_init(void){
  g_spi = esp32s3_spibus_initialize(2);
  syslog(LOG_ERR,"[wslcd] SPI2 lcd handle=%p\n", g_spi);
  esp32s3_configgpio(PIN_DC,OUTPUT);
}

static void panel_init(void)
{
  esp32s3_configgpio(PIN_MOSI,OUTPUT);esp32s3_configgpio(PIN_CLK,OUTPUT);esp32s3_configgpio(PIN_DC,OUTPUT);
  g_out=(*(volatile uint32_t *)REG_OUT)&~(MOSI_M|CLK_M|DC_M);
  cmd(0x01);up_mdelay(150);cmd(0x11);up_mdelay(120);
  cmd(0xF0);dat(0xC3);cmd(0xF0);dat(0x96);cmd(0x36);dat(0x28);cmd(0x3A);dat(0x55);
  cmd(0xF0);dat(0x3C);cmd(0xF0);dat(0x69);cmd(0x21);cmd(0x29);up_mdelay(120);
  bb_window(0,XRES-1,0,YRES-1); fill_be(0,0,(long)XRES*YRES);
  syslog(LOG_ERR,"[wslcd] panel_init done\n");
}

/****************************************************************************
 * panel_init_spi -- bring the display up entirely over HARDWARE SPI.
 *
 * THE ORDER HERE IS THE WHOLE FIX. The panel's CS is tied LOW, so it can never
 * re-sync its bit counter on a CS edge -- it just counts clock edges forever.
 * If we bit-bang the init and hand the pads to SPI afterwards, that handover
 * glitches CLK by one edge and the panel is desynced permanently: every command
 * decodes as garbage, RAMWR never fires, no pixels reach GRAM, and the screen
 * just keeps showing whatever bit-bang last painted. That single fact caused
 * ~60 failed build cycles of "HW-SPI renders nothing".
 *
 * So: give the pads to SPI FIRST, then HARDWARE-reset the panel (a SWRESET
 * cannot work -- it would itself be misread), then init over SPI. These pins
 * must never be bit-banged again afterwards.
 ****************************************************************************/
static void panel_init_spi(void)
{
  int i, r;

  g_spi = esp32s3_spibus_initialize(2);

  /* A pad must be a GPIO output BEFORE a matrix signal will drive it -- without
   * this the peripheral's output never reaches the pin. */
  esp32s3_configgpio(PIN_MOSI, OUTPUT);
  esp32s3_configgpio(PIN_CLK,  OUTPUT);
  esp32s3_gpio_matrix_out(PIN_MOSI, 103, 0, 0);   /* FSPID_OUT   -> GPIO1 */
  esp32s3_gpio_matrix_out(PIN_CLK,  101, 0, 0);   /* FSPICLK_OUT -> GPIO5 */
  esp32s3_configgpio(PIN_DC, OUTPUT);             /* DC stays a plain GPIO */

  spi_begin();

  /* Now that SPI owns the pins, hardware-reset the panel to re-sync it. */
  if (g_ioe)
    {
      IOEXP_WRITEPIN(g_ioe, 1, false); nxsig_usleep(120000);
      IOEXP_WRITEPIN(g_ioe, 1, true);  nxsig_usleep(150000);
    }

  spi_cmd(0x01); up_mdelay(150);                  /* SWRESET */
  spi_cmd(0x11); up_mdelay(120);                  /* SLPOUT  */
  spi_cmd(0xF0); spi_dat(0xC3);
  spi_cmd(0xF0); spi_dat(0x96);
  spi_cmd(0x36); spi_dat(0x28);                   /* MADCTL: landscape, BGR */
  spi_cmd(0x3A); spi_dat(0x55);                   /* COLMOD: 16bpp */
  spi_cmd(0xF0); spi_dat(0x3C);
  spi_cmd(0xF0); spi_dat(0x69);
  spi_cmd(0x21);                                  /* INVON  */
  spi_cmd(0x29); up_mdelay(120);                  /* DISPON */

  /* clear to black */
  spi_window(0, XRES-1, 0, YRES-1);
  spi_dc_data();
  for (i = 0; i < XRES * 2; i++) g_swap[i] = 0;
  for (r = 0; r < YRES; r++) SPI_SNDBLOCK(g_spi, g_swap, XRES * 2);
  spi_end();

  syslog(LOG_ERR, "[wslcd] panel_init_spi done - HARDWARE SPI @40MHz\n");
}

static int lcd_getvideoinfo(struct lcd_dev_s *dev, struct fb_videoinfo_s *v)
{ v->fmt=FB_FMT_RGB16_565; v->xres=XRES; v->yres=YRES; v->nplanes=1; return OK; }

/* Render over HARDWARE SPI. The byte-swap is because LVGL hands us
 * little-endian RGB565 and the panel wants big-endian; we swap into g_swap
 * (internal SRAM) rather than sending `buffer` directly, because buffer lives
 * in PSRAM and SPI DMA cannot read PSRAM cache-coherently. */
static int lcd_putarea(struct lcd_dev_s *dev, fb_coord_t rs, fb_coord_t re, fb_coord_t cs, fb_coord_t ce, const uint8_t *buffer, fb_coord_t stride)
{
  int rows=re-rs+1, r; size_t rowb=(size_t)(ce-cs+1)*2, i;
  if (rowb > sizeof(g_swap)) return -EINVAL;
  spi_begin();
  spi_window(cs, ce, rs, re);
  spi_dc_data();
  for(r=0;r<rows;r++)
    {
      const uint8_t *p=buffer+(size_t)r*stride;
      for(i=0;i<rowb;i+=2){ g_swap[i]=p[i+1]; g_swap[i+1]=p[i]; }
      SPI_SNDBLOCK(g_spi, g_swap, rowb);
    }
  spi_end();
  return OK;
}
static int lcd_putrun(struct lcd_dev_s *dev, fb_coord_t row, fb_coord_t col, const uint8_t *buffer, size_t npixels)
{
  size_t i, nb=npixels*2;
  if (nb > sizeof(g_swap)) return -EINVAL;
  spi_begin();
  spi_window(col, col+npixels-1, row, row);
  spi_dc_data();
  for(i=0;i<nb;i+=2){ g_swap[i]=buffer[i+1]; g_swap[i+1]=buffer[i]; }
  SPI_SNDBLOCK(g_spi, g_swap, nb);
  spi_end();
  return OK;
}
#ifndef CONFIG_LCD_NOGETRUN
static int lcd_getrun(struct lcd_dev_s *dev, fb_coord_t row, fb_coord_t col, uint8_t *buffer, size_t npixels){ return -ENOSYS; }
#endif
static int lcd_getplaneinfo(struct lcd_dev_s *dev, unsigned int planeno, struct lcd_planeinfo_s *p)
{ p->putrun=lcd_putrun; p->putarea=lcd_putarea;
#ifndef CONFIG_LCD_NOGETRUN
  p->getrun=lcd_getrun;
#endif
  p->buffer=(uint8_t *)g_lcd.runbuf; p->bpp=16; p->dev=dev; return OK; }
static int lcd_getpower(struct lcd_dev_s *dev){ return g_lcd.power; }
static int lcd_setpower(struct lcd_dev_s *dev, int power){ g_lcd.power=power; esp32s3_gpiowrite(PIN_BL, power>0); return OK; }

static void ts_wakeup(const struct ft5x06_config_s *c){(void)c;}
static void ts_nreset(const struct ft5x06_config_s *c, bool st){(void)c;(void)st;}
static const struct ft5x06_config_s g_ts = {
  .address=0x38, .frequency=400000, .wakeup=ts_wakeup, .nreset=ts_nreset,
};

/* ---- OV5640 camera phase 1: XCLK on GPIO38 + SCCB chip-ID probe ---- */
/* Hardcoded esp32s3 register addrs/bits (the hardware/ headers pull in
 * soc/soc.h which isn't on the board include path). Values verified against
 * esp-hal soc/*_reg.h and esp32s3_ledc.c. */
#define CAM_XCLK_PIN        38
#define LEDC_BASE           0x60019000U
#define R_LEDC_CH0_CONF0    (LEDC_BASE + 0x00)
#define R_LEDC_CH0_HPOINT   (LEDC_BASE + 0x04)
#define R_LEDC_CH0_DUTY     (LEDC_BASE + 0x08)
#define R_LEDC_CH0_CONF1    (LEDC_BASE + 0x0c)
#define R_LEDC_TIMER0_CONF  (LEDC_BASE + 0xa0)
#define R_LEDC_CONF         (LEDC_BASE + 0xd0)
#define B_LEDC_CLK_EN       (1U << 31)
#define B_LEDC_T0_RST       (1U << 23)
#define B_LEDC_T0_PARAUP    (1U << 25)
#define S_LEDC_DUTYRES      0
#define S_LEDC_CLKDIV       4
#define B_LEDC_CH0_SIGOUT   (1U << 2)
#define B_LEDC_CH0_DUTYST   (1U << 31)
#define B_LEDC_CH0_PARAUP   (1U << 4)
#define R_SYS_CLK_EN0       0x600C0018U
#define R_SYS_RST_EN0       0x600C0020U
#define B_SYS_LEDC          (1U << 11)
#define LEDC_SIG_OUT0_IDX   73

static inline uint32_t rreg(uint32_t a){ return *(volatile uint32_t *)a; }
static inline void sreg(uint32_t a,uint32_t b){ wreg(a, rreg(a)|b); }
static inline void creg(uint32_t a,uint32_t b){ wreg(a, rreg(a)&~b); }

static void cam_xclk_start(void)
{
  /* LEDC peripheral clock on, source = APB 80MHz (mirrors esp32s3_ledc.c) */
  sreg(R_SYS_CLK_EN0, B_SYS_LEDC);
  creg(R_SYS_RST_EN0, B_SYS_LEDC);
  wreg(R_LEDC_CONF, 1);                 /* clock source = APB */
  sreg(R_LEDC_CONF, B_LEDC_CLK_EN);
  /* timer0: 80MHz / (prescaler 1 * 2^2) = 20MHz */
  sreg(R_LEDC_TIMER0_CONF, B_LEDC_T0_RST);
  wreg(R_LEDC_TIMER0_CONF,
       (2u << S_LEDC_DUTYRES) |
       (0u << S_LEDC_CLKDIV) |
       (1u << (S_LEDC_CLKDIV + 8)));
  sreg(R_LEDC_TIMER0_CONF, B_LEDC_T0_PARAUP);
  /* channel0 -> timer0, 50% duty (reload/2 = 2), output enable */
  wreg(R_LEDC_CH0_CONF0, 0);
  wreg(R_LEDC_CH0_CONF1, 0);
  wreg(R_LEDC_CH0_HPOINT, 0);
  wreg(R_LEDC_CH0_DUTY, 2u << 4);
  sreg(R_LEDC_CH0_CONF0, B_LEDC_CH0_SIGOUT);
  sreg(R_LEDC_CH0_CONF1, B_LEDC_CH0_DUTYST);
  sreg(R_LEDC_CH0_CONF0, B_LEDC_CH0_PARAUP);
  /* route LEDC ch0 output signal to GPIO38 */
  esp32s3_configgpio(CAM_XCLK_PIN, OUTPUT);
  esp32s3_gpio_matrix_out(CAM_XCLK_PIN, LEDC_SIG_OUT0_IDX + 0, 0, 0);
  syslog(LOG_ERR,"[cam] XCLK ~20MHz on GPIO%d\n", CAM_XCLK_PIN);
}

/* ---- FEEDER: 28BYJ-48 STEPPER via ULN2003 --------------------------------
 * Replaces the MG90S servo. A stepper gives CONTINUOUS one-direction rotation,
 * so pockets ratchet inlet->outlet and never reverse -> TRUE per-pocket
 * metering: grams = pockets x GRAMS_PER_POCKET, literally. (The servo could
 * only oscillate; with the inlet/outlet 180 deg apart that made honest per-
 * pocket dosing impossible -- see git history.)
 *
 * 28BYJ-48: 32 steps/motor-rev x ~63.68 gear, driven FULL-step (4-phase) =
 * ~2048 full steps per OUTPUT revolution. 6 pockets -> 2048/6 ~= 341 steps per
 * pocket. Driven through a ULN2003 board: IN1..IN4 <- 4 GPIOs; the board takes
 * 5V (same separate supply as before) + GND common with the ESP32.
 *
 * Full-step, not half-step: two coils are energised at every phase, so torque
 * is roughly double and the rotor does not stall against the food column. This
 * is what is on the board and what turns -- do not "upgrade" it to half-step
 * for smoothness. See docs/ and the stepper bring-up notes.
 *
 * Pins: these are the ONLY four GPIOs this board actually exposes for general
 * use. Verified against the ESP32-S3-Touch-LCD-3.5 schematic: the 2x16 header
 * J8 carries IO9/IO10/IO11 (shared with the unused microSD slot, whose CS is on
 * expander EXIO3 and stays deasserted) and IO43 (U0TXD, free because the
 * console is USB-Serial/JTAG). Everything else on J8 is camera, I2C, USB, EN,
 * PWRON or power.
 *
 * Do NOT move these to IO12/13/14/15/16. Those are the ES8311 codec's I2S bus
 * and are NOT routed to any header -- each net has exactly two nodes, the
 * ESP32 and the codec. Driving them reaches no motor, and once the codec is
 * enabled for the onboard mic/speaker it would corrupt audio on every dispense.
 * (An earlier revision had IN1..IN3 on 12/13/15: three coils went nowhere, and
 * the motor buzzed instead of turning.)
 *
 * IO44 (U0RXD) does not work as an output -- the UART0 mux re-wins over
 * esp32s3_configgpio() and clamps the pin. That was the other half of the
 * "buzzes but won't turn" saga. Use IO43.
 */
#define STEP_IN1             9        /* J8 pin 14 (SD_MISO, slot unused) */
#define STEP_IN2             10       /* J8 pin 12 (SD_MOSI, slot unused) */
#define STEP_IN3             11       /* J8 pin 10 (SD_SCLK, slot unused) */
#define STEP_IN4             43       /* J8 pin 27 (U0TXD; NOT 44 -- clamped) */
#define ROTOR_POCKETS        6              /* vanes; must match the .scad */
#define STEP_PER_REV         2048           /* 28BYJ-48 FULL-step (64:1 gear) */
#define STEP_PER_POCKET      (STEP_PER_REV / ROTOR_POCKETS)   /* ~341 */
#define STEP_DELAY_US        3000           /* per full-step; ~333 steps/s, within pull-in torque */
#define GRAMS_PER_POCKET     5              /* CALIBRATE: weigh one pocket */

/* 4-step FULL-step drive sequence; bit0..bit3 = IN1..IN4 (NOT bit3..bit0 --
 * getting this backwards drives the rotor in reverse and food never reaches
 * the outlet). This is the textbook 28BYJ-48 sequence and it is correct; all
 * three distinct coil-order topologies were swept during bring-up and the
 * fault was never the table, it was a dead pin. Do not "fix" it again.
 */
static const uint8_t k_fullstep[4] =
  { 0x3, 0x6, 0xC, 0x9 };                   /* IN1+2, IN2+3, IN3+4, IN4+1 */

static void step_coils(uint8_t m)
{
  esp32s3_gpiowrite(STEP_IN1, (m & 0x1) != 0);
  esp32s3_gpiowrite(STEP_IN2, (m & 0x2) != 0);
  esp32s3_gpiowrite(STEP_IN3, (m & 0x4) != 0);
  esp32s3_gpiowrite(STEP_IN4, (m & 0x8) != 0);
}

/* Claim all four coil lines as plain GPIO outputs. MUST be re-called right
 * before every dispense: other subsystems (and the UART0 mux) can steal a pin
 * back between feeds, and a silently re-muxed coil is exactly the failure that
 * looks like "the motor buzzes but won't turn".
 */
static void feeder_pins_output(void)
{
  esp32s3_configgpio(STEP_IN1, OUTPUT);
  esp32s3_configgpio(STEP_IN2, OUTPUT);
  esp32s3_configgpio(STEP_IN3, OUTPUT);
  esp32s3_configgpio(STEP_IN4, OUTPUT);
}

void board_feeder_init(void)
{
  feeder_pins_output();
  step_coils(0);                       /* coils off at rest: no heat, no hum */
  syslog(LOG_ERR, "[feeder] 28BYJ-48 stepper ready (ULN2003 on %d/%d/%d/%d)\n",
         STEP_IN1, STEP_IN2, STEP_IN3, STEP_IN4);
}

/* Rotate the rotor FORWARD by n full steps (ONE direction -> pockets ratchet
 * inlet->outlet). Busy-waits STEP_DELAY_US per step, so it MUST run off the UI
 * thread -- the app calls board_feeder_dispense from a detached worker (see
 * hal/feeder_servo.c). Coils de-energise at the end (no holding current/heat). */
static void rotor_step(int n)
{
  static int phase = 0;
  int i;

  for (i = 0; i < n; i++)
    {
      phase = (phase + 1) & 3;
      step_coils(k_fullstep[phase]);
      up_udelay(STEP_DELAY_US);
    }
  step_coils(0);
}

int board_feeder_dispense(int grams)
{
  int pockets;
  int steps;

  if (grams <= 0) return -1;

  feeder_pins_output();     /* re-claim the coil lines; see the note above */

  /* round to the nearest whole pocket; always give at least one */
  pockets = (grams + GRAMS_PER_POCKET / 2) / GRAMS_PER_POCKET;
  if (pockets < 1) pockets = 1;

  steps = pockets * STEP_PER_POCKET;
  rotor_step(steps);

  syslog(LOG_ERR, "[feeder] dispensed %d pocket(s) ~%dg (%d steps, asked %dg)\n",
         pockets, pockets * GRAMS_PER_POCKET, steps, grams);
  return 0;
}

/* OV5640 SCCB read: 7-bit addr 0x3C, 16-bit reg + 8-bit data, big-endian */
static int ov5640_rd(struct i2c_master_s *i, uint16_t reg, uint8_t *val)
{
  struct i2c_msg_s m[2]; uint8_t a[2];
  a[0]=(uint8_t)(reg>>8); a[1]=(uint8_t)(reg&0xff);
  m[0].frequency=100000; m[0].addr=0x3C; m[0].flags=0;          m[0].buffer=a;   m[0].length=2;
  m[1].frequency=100000; m[1].addr=0x3C; m[1].flags=I2C_M_READ; m[1].buffer=val; m[1].length=1;
  return I2C_TRANSFER(i, m, 2);
}

static void cam_probe(struct i2c_master_s *i2c)
{
  uint8_t idh=0xFF, idl=0xFF; int r1, r2;
  cam_xclk_start();
  nxsig_usleep(30000);     /* let the OV5640 internal PLL settle after XCLK */
  r1 = ov5640_rd(i2c, 0x300A, &idh);
  r2 = ov5640_rd(i2c, 0x300B, &idl);
  syslog(LOG_ERR,"[cam] OV5640 id=0x%02x%02x ret=%d/%d (expect 0x5640)\n",
         idh, idl, r1, r2);
}

/* ===== OV5640 -> QVGA RGB565 config + LCD_CAM/GDMA single-shot capture ===== */
#define OV_DLY 0xffff
#define OV_END 0x0000

static int ov5640_wr(struct i2c_master_s *i, uint16_t reg, uint8_t val)
{
  struct i2c_msg_s m; uint8_t b[3];
  b[0]=(uint8_t)(reg>>8); b[1]=(uint8_t)(reg&0xff); b[2]=val;
  m.frequency=100000; m.addr=0x3C; m.flags=0; m.buffer=b; m.length=3;
  return I2C_TRANSFER(i, &m, 1);
}

/* OV5640 default register table (from esp32-camera ov5640_settings.h) */
static const uint16_t ov_default_regs[][2] = {
  {0x3008,0x82},{OV_DLY,10},{0x3008,0x42},{0x3103,0x13},{0x3017,0xff},{0x3018,0xff},
  {0x302c,0xc3},{0x4740,0x21},{0x4713,0x02},{0x5001,0x83},{0x3000,0x20},{OV_DLY,10},
  {0x3002,0x1c},{0x3004,0xff},{0x3006,0xc3},{0x5000,0xa7},{0x5001,0xa3},{0x5003,0x08},
  {0x370c,0x02},{0x3634,0x40},{0x3a02,0x03},{0x3a03,0xd8},{0x3a08,0x01},{0x3a09,0x27},
  {0x3a0a,0x00},{0x3a0b,0xf6},{0x3a0d,0x04},{0x3a0e,0x03},{0x3a0f,0x30},{0x3a10,0x28},
  {0x3a11,0x60},{0x3a13,0x43},{0x3a14,0x03},{0x3a15,0xd8},{0x3a18,0x00},{0x3a19,0xf8},
  {0x3a1b,0x30},{0x3a1e,0x26},{0x3a1f,0x14},{0x3600,0x08},{0x3601,0x33},{0x3c01,0xa4},
  {0x3c04,0x28},{0x3c05,0x98},{0x3c06,0x00},{0x3c07,0x08},{0x3c08,0x00},{0x3c09,0x1c},
  {0x3c0a,0x9c},{0x3c0b,0x40},{0x460c,0x22},{0x4001,0x02},{0x4004,0x02},{0x5180,0xff},
  {0x5181,0xf2},{0x5182,0x00},{0x5183,0x14},{0x5184,0x25},{0x5185,0x24},{0x5186,0x09},
  {0x5187,0x09},{0x5188,0x09},{0x5189,0x75},{0x518a,0x54},{0x518b,0xe0},{0x518c,0xb2},
  {0x518d,0x42},{0x518e,0x3d},{0x518f,0x56},{0x5190,0x46},{0x5191,0xf8},{0x5192,0x04},
  {0x5193,0x70},{0x5194,0xf0},{0x5195,0xf0},{0x5196,0x03},{0x5197,0x01},{0x5198,0x04},
  {0x5199,0x12},{0x519a,0x04},{0x519b,0x00},{0x519c,0x06},{0x519d,0x82},{0x519e,0x38},
  {0x5381,0x1e},{0x5382,0x5b},{0x5383,0x08},{0x5384,0x0a},{0x5385,0x7e},{0x5386,0x88},
  {0x5387,0x7c},{0x5388,0x6c},{0x5389,0x10},{0x538a,0x01},{0x538b,0x98},{0x5300,0x10},
  {0x5301,0x10},{0x5302,0x18},{0x5303,0x19},{0x5304,0x10},{0x5305,0x10},{0x5306,0x08},
  {0x5307,0x16},{0x5308,0x40},{0x5309,0x10},{0x530a,0x10},{0x530b,0x04},{0x530c,0x06},
  {0x5480,0x01},{0x5481,0x00},{0x5482,0x1e},{0x5483,0x3b},{0x5484,0x58},{0x5485,0x66},
  {0x5486,0x71},{0x5487,0x7d},{0x5488,0x83},{0x5489,0x8f},{0x548a,0x98},{0x548b,0xa6},
  {0x548c,0xb8},{0x548d,0xca},{0x548e,0xd7},{0x548f,0xe3},{0x5490,0x1d},{0x5580,0x06},
  {0x5583,0x40},{0x5584,0x10},{0x5586,0x20},{0x5587,0x00},{0x5588,0x00},{0x5589,0x10},
  {0x558a,0x00},{0x558b,0xf8},{0x501d,0x40},{0x3008,0x02},{0x3c00,0x04},{OV_DLY,300},
  {OV_END,0x00},
};

static void ov_write_array(struct i2c_master_s *i, const uint16_t regs[][2])
{
  int k;
  for (k=0; ; k++)
    {
      uint16_t reg=regs[k][0], val=regs[k][1];
      if (reg==OV_END && val==0) break;
      if (reg==OV_DLY) { up_mdelay(val); continue; }
      ov5640_wr(i, reg, (uint8_t)val);
    }
}

static void ov5640_config_qvga(struct i2c_master_s *i, int testbar)
{
  ov5640_wr(i, 0x3008, 0x82); up_mdelay(100);   /* software reset */
  ov_write_array(i, ov_default_regs);
  /* pixel format RGB565 */
  ov5640_wr(i, 0x501F, 0x01); ov5640_wr(i, 0x4300, 0x61);
  /* framesize QVGA 320x240 (4x3) windowing */
  ov5640_wr(i,0x3800,0x00);ov5640_wr(i,0x3801,0x00);ov5640_wr(i,0x3802,0x00);ov5640_wr(i,0x3803,0x00);
  ov5640_wr(i,0x3804,0x0A);ov5640_wr(i,0x3805,0x3F);ov5640_wr(i,0x3806,0x07);ov5640_wr(i,0x3807,0x9F);
  ov5640_wr(i,0x3808,0x01);ov5640_wr(i,0x3809,0x40);ov5640_wr(i,0x380A,0x00);ov5640_wr(i,0x380B,0xF0);
  ov5640_wr(i,0x380C,0x08);ov5640_wr(i,0x380D,0x0C);ov5640_wr(i,0x380E,0x03);ov5640_wr(i,0x380F,0xD8);
  ov5640_wr(i,0x3810,0x00);ov5640_wr(i,0x3811,0x10);ov5640_wr(i,0x3812,0x00);ov5640_wr(i,0x3813,0x08);
  /* Both bit1 (0x02, "sensor-side") and bit2 (0x04, "ISP-side") of these
   * regs shift the raw Bayer CFA phase and corrupt color -- confirmed by
   * testing bit2-only (0x05/0x05), which still produced the purple tint.
   * In the OV5640, bits 1+2 are meant to be toggled TOGETHER as one flip
   * unit (mask 0x06), not used as independent "safe" vs "unsafe" halves;
   * splitting them left the sensor in an undefined partial-flip state.
   * The real orientation bug (confirmed via a hand test on hardware: a
   * vertical hand rendered horizontal) was a 90 deg transpose that these
   * flip/mirror bits structurally cannot cause or fix -- that is now
   * corrected in software (hal/camera_ov5640.c downscale/resample, which
   * also carries the selfie mirror). So: leave the sensor's own flip/
   * mirror OFF (bit0 preserved, meaning unknown/unrelated to flip -- not
   * touched) and let software own 100% of the orientation. NOT touching
   * 0x4514 -- ruled out last round. */
  ov5640_wr(i,0x3820,0x01);ov5640_wr(i,0x3821,0x01);
  ov5640_wr(i,0x4520,0x0b);ov5640_wr(i,0x3814,0x31);ov5640_wr(i,0x3815,0x31);
  /* PLL for QVGA RGB565: set_pll(false,8,1,1,false,1,true,4) */
  ov5640_wr(i,0x3039,0x00);ov5640_wr(i,0x3034,0x1A);ov5640_wr(i,0x3035,0x11);ov5640_wr(i,0x3036,0x08);
  ov5640_wr(i,0x3037,0x01);ov5640_wr(i,0x3108,0x16);ov5640_wr(i,0x3824,0x04);ov5640_wr(i,0x460C,0x22);
  ov5640_wr(i,0x3103,0x13);
  if (testbar) { ov5640_wr(i, 0x503D, 0x80); }   /* color-bar test pattern */
  up_mdelay(100);
}

/* LCD_CAM camera-mode registers (base 0x60041000) */
#define R_CAM_CTRL      0x60041004U
#define R_CAM_CTRL1     0x60041008U
#define R_CAM_RGB_YUV   0x6004100cU
#define R_CAM_INT_CLR   0x60041070U
#define R_SYS_CLK_EN1   0x600C001CU
#define R_SYS_RST_EN1   0x600C0024U
#define B_SYS_LCDCAM    (1U<<8)
/* CAM_CTRL fields */
#define B_CAM_UPDATE    (1U<<4)
#define B_CAM_VS_EOF_EN (1U<<8)
/* CAM_CTRL1 fields */
#define B_CAM_VSFILT_EN (1U<<23)
#define B_CAM_START     (1U<<29)
#define B_CAM_RESET     (1U<<30)
#define B_CAM_AFIFO_RST (1U<<31)
/* DVP pins + gpio-matrix input signal indices */
#define CAM_W 320
#define CAM_H 240
#define CAM_FRAME_BYTES (CAM_W*CAM_H*2)
#define CAM_VSYNC 17
#define CAM_HREF  18
#define CAM_PCLK  41
static const uint8_t cam_dpin[8] = {45,47,48,46,42,40,39,21};  /* D0..D7 */
#define IDX_CAM_DATA0 133
#define IDX_CAM_PCLK  149
#define IDX_CAM_HREF  150
#define IDX_CAM_VSYNC 152

extern int cache_invalidate_addr(uint32_t addr, uint32_t size);

#define CAM_NDESC ((CAM_FRAME_BYTES + 4091) / 4092)
static struct esp32s3_dmadesc_s g_camdesc[CAM_NDESC + 2];
static uint8_t *g_cambuf;
static int g_camchan = -1;

static void cam_pins_init(void)
{
  int k;
  esp32s3_configgpio(CAM_VSYNC, INPUT);
  esp32s3_gpio_matrix_in(CAM_VSYNC, IDX_CAM_VSYNC, false);
  esp32s3_configgpio(CAM_HREF, INPUT);
  esp32s3_gpio_matrix_in(CAM_HREF, IDX_CAM_HREF, false);
  esp32s3_configgpio(CAM_PCLK, INPUT);
  esp32s3_gpio_matrix_in(CAM_PCLK, IDX_CAM_PCLK, false);
  for (k=0;k<8;k++)
    {
      esp32s3_configgpio(cam_dpin[k], INPUT);
      esp32s3_gpio_matrix_in(cam_dpin[k], IDX_CAM_DATA0 + k, false);
    }
}

static void cam_lcdcam_init(void)
{
  /* enable LCD_CAM peripheral clock + release reset */
  sreg(R_SYS_CLK_EN1, B_SYS_LCDCAM);
  creg(R_SYS_RST_EN1, B_SYS_LCDCAM);
  /* CAM_CTRL: clk_sel=3, clkm_div_num=8 (160/20), vsync_filter_thres=4,
   * vs_eof_en=1: since capture is armed at a frame START (cam_wait_vsync),
   * the DMA runs to the NEXT VSYNC = exactly one aligned frame. */
  wreg(R_CAM_CTRL, (3u<<29) | (8u<<9) | (4u<<1) | B_CAM_VS_EOF_EN);
  /* CAM_CTRL1: rec_data_bytelen max, vsync_filter_en, 8-bit data */
  wreg(R_CAM_CTRL1, 0xffffu | B_CAM_VSFILT_EN);
  wreg(R_CAM_RGB_YUV, 0);
  sreg(R_CAM_CTRL, B_CAM_UPDATE);
}

static int cam_dma_init(void)
{
  esp32s3_dma_init();   /* idempotent; ensures GDMA is up */
  g_camchan = esp32s3_dma_request(ESP32S3_DMA_PERIPH_LCDCAM, 0, 1, true);
  if (g_camchan < 0) { syslog(LOG_ERR,"[cam] dma_request failed\n"); return -1; }
  esp32s3_dma_set_ext_memblk(g_camchan, false, ESP32S3_DMA_EXT_MEMBLK_64B);
  g_cambuf = kmm_memalign(64, CAM_FRAME_BYTES);
  if (!g_cambuf) { syslog(LOG_ERR,"[cam] buf alloc failed\n"); return -1; }
  memset(g_cambuf, 0, CAM_FRAME_BYTES);
  esp32s3_dma_setup(g_camdesc, CAM_NDESC + 2, g_cambuf, CAM_FRAME_BYTES,
                    false, g_camchan);
  syslog(LOG_ERR,"[cam] dma chan=%d buf=%p ndesc=%d\n",
         g_camchan, g_cambuf, CAM_NDESC);
  return 0;
}

#define GPIO_IN_REG 0x6000403CU
static inline int cam_vsync_level(void){ return (rreg(GPIO_IN_REG) >> CAM_VSYNC) & 1; }

/* Block until a fresh frame boundary. VSYNC assumed active-high pulse
 * (OV5640 default): wait for the pulse (high) then its end (low)=frame start.
 * Bounded so a stuck line can't hang boot. */
static void cam_wait_vsync(void)
{
  volatile int i;
  for(i=0;i<4000000;i++){ if(cam_vsync_level()) break; }   /* -> high (pulse) */
  for(i=0;i<4000000;i++){ if(!cam_vsync_level()) break; }  /* -> low (frame start) */
}

/* Capture one VSYNC-aligned frame into g_cambuf (internal). */
static void cam_capture_once(void)
{
  /* start camera streaming */
  creg(R_CAM_CTRL1, B_CAM_START);
  sreg(R_CAM_CTRL1, B_CAM_RESET); creg(R_CAM_CTRL1, B_CAM_RESET);
  sreg(R_CAM_CTRL, B_CAM_UPDATE);
  sreg(R_CAM_CTRL1, B_CAM_START);
  /* align to frame start, then arm the DMA -> one clean aligned frame */
  cam_wait_vsync();
  sreg(R_CAM_CTRL1, B_CAM_AFIFO_RST); creg(R_CAM_CTRL1, B_CAM_AFIFO_RST);
  wreg(R_CAM_INT_CLR, ~0u);
  esp32s3_dma_disable(g_camchan, false);   /* clean state before re-arm */
  esp32s3_dma_load(g_camdesc, g_camchan, false);
  esp32s3_dma_enable(g_camchan, false);
  /* wait for the frame-end VSYNC so we read a COMPLETE, single frame (a fixed
   * delay races the frame time and gives torn/half frames when the scene moves) */
  cam_wait_vsync();
  up_mdelay(2);    /* let the DMA flush the frame tail from the AFIFO to PSRAM */
  cache_invalidate_addr((uint32_t)(uintptr_t)g_cambuf, CAM_FRAME_BYTES);

  /* CAMERA WEDGE FIX: make each capture fully self-contained. Leaving the
   * GDMA channel enabled and the LCD_CAM streaming between captures corrupts
   * the descriptor/AFIFO state after ~N frames and wedges the whole SoC
   * (seen as a console "semaphore timeout"). Stop the RX DMA and halt camera
   * streaming after every frame; the next capture re-arms cleanly. */
  esp32s3_dma_disable(g_camchan, false);
  creg(R_CAM_CTRL1, B_CAM_START);   /* clear START -> stop streaming */
}

/* ---- public API used by the velapaw camera HAL (flat build) ---- */

/* Configure the OV5640 + LCD_CAM + GDMA once (call at boot, after XCLK). */
int board_ov5640_init(struct i2c_master_s *i2c)
{
  syslog(LOG_ERR,"[cam] init OV5640 QVGA RGB565\n");
  ov5640_config_qvga(i2c, 0);        /* real camera; testbar diagnostic done */
  cam_pins_init();
  cam_lcdcam_init();
  return cam_dma_init();
}

/* Grab a fresh frame; returns the QVGA RGB565 buffer + dimensions. */
int board_ov5640_capture(uint8_t **buf, int *w, int *h)
{
  if (g_cambuf == NULL) return -1;
  cam_capture_once();
  if (buf) *buf = g_cambuf;
  if (w)   *w = CAM_W;
  if (h)   *h = CAM_H;
  return 0;
}

/* ==========================================================================
 * SPI CAPTURE / LOGIC-ANALYZER DIAGNOSTIC MODE
 *
 * Set SPI_CAPTURE_BUILD to 1 to build a DEDICATED diagnostic image: it does NOT
 * run the UI. It bit-bangs the panel init (proven), paints a RED full screen as
 * a visible "this build is alive" marker (no console in this mode), then drives
 * PURE hardware SPI2 at 1 MHz -- slow enough that a 24 MHz analyzer resolves
 * every edge -- looping a full-screen BLUE fill (historically works) and a
 * 100x100 BLUE square (the case that fails).
 *
 * Probe pins (the real SPI pins 1/5/3 are not on the header):
 *   GPIO40 = MOSI mirror (FSPID_OUT 103)   -> analyzer CH0
 *   GPIO41 = CLK  mirror (FSPICLK_OUT 101)  -> analyzer CH1
 *   GPIO42 = DC   mirror                    -> analyzer CH2
 *   GPIO47 = TRIG: HIGH during the FAILING partial transaction -> CH3 (trigger)
 *   plus analyzer GND -> board GND.
 *
 * Set back to 0 (or restore the golden board file) to return to the product.
 * ========================================================================== */
#define SPI_CAPTURE_BUILD 0
/* Set to 1 to drive the 4 probe pins as clean, SLOW, DISTINCT square waves
 * instead of the SPI test -- a ground-truth to verify the analyzer wiring +
 * channel<->pin mapping before trusting any SPI capture. Each pin toggles at
 * half the rate of the previous, so a good capture shows 4 nested squares:
 *   GPIO40(D0)=fastest ... GPIO47(D3)=slowest. Any channel that reads noise
 * instead of a clean square is not actually connected. */
#define GPIO_GROUND_TEST  0

#if SPI_CAPTURE_BUILD
#define PIN_MOSI_MIRROR 40
#define PIN_CLK_MIRROR  41
#define PIN_DC_MIRROR   42
#define PIN_TRIG        47

static void cap_cmd(uint8_t c){ uint8_t b=c; spi_idle();
  esp32s3_gpiowrite(PIN_DC,false); esp32s3_gpiowrite(PIN_DC_MIRROR,false);
  SPI_SNDBLOCK(g_spi,&b,1); }
static void cap_dat(uint8_t d){ uint8_t b=d; spi_idle();
  esp32s3_gpiowrite(PIN_DC,true);  esp32s3_gpiowrite(PIN_DC_MIRROR,true);
  SPI_SNDBLOCK(g_spi,&b,1); }
static void cap_window(int x0,int x1,int y0,int y1){
  cap_cmd(0x2A);cap_dat(x0>>8);cap_dat(x0&0xff);cap_dat(x1>>8);cap_dat(x1&0xff);
  cap_cmd(0x2B);cap_dat(y0>>8);cap_dat(y0&0xff);cap_dat(y1>>8);cap_dat(y1&0xff);
  cap_cmd(0x2C); }
static void cap_fill(uint8_t hi,uint8_t lo,int width_px,int rows){
  int i; for(i=0;i<width_px;i++){ g_swap[i*2]=hi; g_swap[i*2+1]=lo; }
  spi_idle(); esp32s3_gpiowrite(PIN_DC,true); esp32s3_gpiowrite(PIN_DC_MIRROR,true);
  for(i=0;i<rows;i++){ SPI_SNDBLOCK(g_spi,g_swap,(size_t)width_px*2); } }

static void spi_capture_run(void)   /* never returns */
{
  /* panel is already bit-bang-initialised + shown a solid colour above; just
   * switch the pads to SPI2 and drive pure hardware SPI. */
  g_spi = esp32s3_spibus_initialize(2);
  /* The pad must be a GPIO output BEFORE routing a matrix signal to it (same
   * order the servo/camera use). Without this the mirror pins never drive and
   * read as flat, even though the SPI transaction is really happening. */
  esp32s3_configgpio(PIN_MOSI_MIRROR, OUTPUT);
  esp32s3_configgpio(PIN_CLK_MIRROR,  OUTPUT);
  esp32s3_gpio_matrix_out(PIN_MOSI_MIRROR, 103, 0, 0);   /* FSPID_OUT  -> 40 */
  esp32s3_gpio_matrix_out(PIN_CLK_MIRROR,  101, 0, 0);   /* FSPICLK_OUT-> 41 */

  /* THE SUSPECT FIX. The analyzer proves SPI2 emits a byte-perfect CASET/RASET/
   * RAMWR + pixels on the mirrors -- yet the panel renders nothing. So the
   * signal is not reaching the panel's own pins. panel_init() above bit-bangs
   * first, leaving GPIO1/5 as PLAIN GPIO outputs; if spibus_initialize() does
   * not fully re-take the pads, they stay stuck in GPIO mode and the SPI signal
   * never gets out. The mirrors proved a pad MUST be configgpio(OUTPUT) BEFORE
   * a matrix signal will drive it -- earlier attempts forced the routing but
   * never the pad config, which is likely why they failed. */
  esp32s3_configgpio(PIN_MOSI, OUTPUT);
  esp32s3_configgpio(PIN_CLK,  OUTPUT);
  esp32s3_gpio_matrix_out(PIN_MOSI, 103, 0, 0);   /* FSPID_OUT  -> GPIO1 (panel) */
  esp32s3_gpio_matrix_out(PIN_CLK,  101, 0, 0);   /* FSPICLK_OUT -> GPIO5 (panel) */

  /* ---- THE FIX: re-sync the panel AFTER the pad handover ------------------
   * Proven by the analyzer: SPI2 emits a byte-perfect CASET/RASET/RAMWR, and
   * (proven by a bit-bang write having no effect) GPIO1/5 really are driven by
   * it. Yet the panel ignores everything. The panel's CS is tied LOW, so it can
   * NEVER re-sync its bit counter on a CS edge -- it just counts clock edges
   * forever. The bit-bang -> SPI pad handover glitches CLK by one edge, so from
   * then on the panel is off by a bit: every command decodes as garbage, RAMWR
   * is never recognised, no pixels are ever written, and the screen simply
   * stays as bit-bang left it. That is exactly what we see.
   *
   * A software reset cannot fix this (SWRESET would itself be misread), so the
   * panel needs a HARDWARE reset once SPI already owns the pins -- then the
   * whole init is redone over SPI, with no bit-bang involved afterwards. */
  if (g_ioe)
    {
      IOEXP_WRITEPIN(g_ioe, 1, false); nxsig_usleep(120000);  /* assert reset  */
      IOEXP_WRITEPIN(g_ioe, 1, true);  nxsig_usleep(150000);  /* release, wait */
    }

  /* Full ST7796 init, now entirely over hardware SPI. */
  cap_cmd(0x01); up_mdelay(150);                   /* SWRESET */
  cap_cmd(0x11); up_mdelay(120);                   /* SLPOUT  */
  cap_cmd(0xF0); cap_dat(0xC3);
  cap_cmd(0xF0); cap_dat(0x96);
  cap_cmd(0x36); cap_dat(0x28);                    /* MADCTL: landscape, BGR */
  cap_cmd(0x3A); cap_dat(0x55);                    /* COLMOD: 16bpp */
  cap_cmd(0xF0); cap_dat(0x3C);
  cap_cmd(0xF0); cap_dat(0x69);
  cap_cmd(0x21);                                   /* INVON  */
  cap_cmd(0x29); up_mdelay(120);                   /* DISPON */
  esp32s3_configgpio(PIN_DC,        OUTPUT);
  esp32s3_configgpio(PIN_DC_MIRROR, OUTPUT);
  esp32s3_configgpio(PIN_TRIG,      OUTPUT);
  esp32s3_gpiowrite(PIN_TRIG, false);
  SPI_LOCK(g_spi,true); SPI_SETMODE(g_spi,SPIDEV_MODE0); SPI_SETBITS(g_spi,8);
  /* 100 kHz: at a 24 MHz analyzer that is ~240 samples per bit -- impossible to
   * miss an edge. (1 MHz did not take: the wire showed ~8 MHz, too fast for the
   * analyzer.) Re-set right before each op below in case the first request is
   * cached/ignored. */
  SPI_SETFREQUENCY(g_spi, 100000);
  syslog(LOG_ERR,"[cap] HW-SPI @100kHz: short window+16px, full then partial\n");
  /* The analyzer already proved the wire is byte-perfect, so stop optimising
   * for it: run at a normal 10 MHz and paint REAL fills we can actually see.
   * (At 100 kHz a full screen would take ~25 s.) */
  SPI_SETFREQUENCY(g_spi, 10000000);
  for(;;){
    esp32s3_gpiowrite(PIN_TRIG, false);
    cap_window(0, XRES-1, 0, YRES-1);      /* full screen -> should go BLUE  */
    cap_fill(0x00,0x1F, XRES, YRES);
    up_mdelay(1500);
    esp32s3_gpiowrite(PIN_TRIG, true);
    cap_window(0, XRES-1, 0, YRES-1);      /* full screen -> back to RED     */
    cap_fill(0xF8,0x00, XRES, YRES);
    up_mdelay(1500);
    esp32s3_gpiowrite(PIN_TRIG, false);
    cap_window(40, 139, 40, 139);          /* the partial window: 100x100    */
    cap_fill(0x00,0x1F, 100, 100);         /* -> a BLUE SQUARE on red        */
    up_mdelay(1500);
  }
}
#endif /* SPI_CAPTURE_BUILD */

int board_lcd_initialize(void)
{
  struct i2c_master_s *i2c; struct ioexpander_dev_s *ioe;
  syslog(LOG_ERR,"[wslcd] init\n");
  esp32s3_configgpio(PIN_BL,OUTPUT);esp32s3_gpiowrite(PIN_BL,true);
  i2c=esp32s3_i2cbus_initialize(0);
  if(i2c){
    /* Sync the system clock from the on-board PCF85063 RTC so time survives
     * resets/power-off (direct I2C; no CONFIG_RTC, so the USB console lives). */
    g_rtc_i2c = i2c;
    { time_t _t;
      if (board_rtc_gettime(&_t) == 0)
        { struct timespec _ts; _ts.tv_sec = _t; _ts.tv_nsec = 0;
          clock_settime(CLOCK_REALTIME, &_ts);
          syslog(LOG_ERR,"[wslcd] RTC ok: synced epoch=%ld\n", (long)_t); }
      else
        { syslog(LOG_ERR,"[wslcd] RTC unset/absent - set it in the UI\n"); } }
    axp_power(i2c);ioe=pca9557_initialize(i2c,&g_tca);g_ioe=ioe;
    if(ioe){IOEXP_SETDIRECTION(ioe,0,IOEXPANDER_DIRECTION_OUT);IOEXP_SETDIRECTION(ioe,1,IOEXPANDER_DIRECTION_OUT);
      IOEXP_WRITEPIN(ioe,0,false);IOEXP_WRITEPIN(ioe,1,false);nxsig_usleep(100000);
      IOEXP_WRITEPIN(ioe,1,true);nxsig_usleep(120000);}
#ifdef CONFIG_AUDIO_ES8311
    /* Speaker amp on.  Only meaningful once the codec is enabled; the ear test
     * (nxlooper -> loopback) is silent without it.  See board_speaker_enable(). */

    board_speaker_enable(true);
    syslog(LOG_ERR,"[velapaw] speaker amp enabled (PA_CTRL = EXIO7)\n");
#endif
    {struct i2c_msg_s _m[2]; uint8_t _r=0xA3,_id=0xFF; int _ret;
 _m[0].frequency=400000;_m[0].addr=0x38;_m[0].flags=0;_m[0].buffer=&_r;_m[0].length=1;
 _m[1].frequency=400000;_m[1].addr=0x38;_m[1].flags=I2C_M_READ;_m[1].buffer=&_id;_m[1].length=1;
 _ret=I2C_TRANSFER(i2c,_m,2);
 syslog(LOG_ERR,"[wslcd] FT6336 id(0xA3)=0x%02x ret=%d\n",_id,_ret);}
 ft5x06_register(i2c,&g_ts,0); syslog(LOG_ERR,"[wslcd] touch registered\n"); cam_probe(i2c);}
  panel_init_spi();                 /* HARDWARE SPI display. The old bit-bang
                                     * panel_init() is kept above for reference
                                     * but must NOT run: bit-banging these pins
                                     * and then handing them to SPI desyncs the
                                     * CS-tied-low panel forever. */
#if SPI_CAPTURE_BUILD
  bb_window(0,XRES-1,0,YRES-1); fill_be(0xF8,0x00,(long)XRES*YRES); /* RED = alive */
  up_mdelay(1500);
#if GPIO_GROUND_TEST
  /* Wiring ground-truth: 4 nested square waves. 100us per step -> GPIO40 is a
   * 5 kHz square (2400 analyzer samples per half-period), each next pin half
   * the rate. A clean capture proves the wiring; noise on a channel = that
   * probe isn't connected. */
  esp32s3_configgpio(PIN_MOSI_MIRROR,OUTPUT); esp32s3_configgpio(PIN_CLK_MIRROR,OUTPUT);
  esp32s3_configgpio(PIN_DC_MIRROR,OUTPUT);   esp32s3_configgpio(PIN_TRIG,OUTPUT);
  { uint32_t k=0;
    for(;;){
      esp32s3_gpiowrite(PIN_MOSI_MIRROR, (k>>0)&1);
      esp32s3_gpiowrite(PIN_CLK_MIRROR,  (k>>1)&1);
      esp32s3_gpiowrite(PIN_DC_MIRROR,   (k>>2)&1);
      esp32s3_gpiowrite(PIN_TRIG,        (k>>3)&1);
      k++; up_udelay(100);
    } }
#endif
  spi_capture_run();                /* diagnostic mode: never returns, no UI */
#endif
  board_feeder_init();              /* 28BYJ-48 stepper via ULN2003 (GPIO9/10/11/43) */
  if(i2c) board_ov5640_init(i2c);   /* configure camera; velapaw drives capture */
  g_lcd.dev.getvideoinfo=lcd_getvideoinfo;
  g_lcd.dev.getplaneinfo=lcd_getplaneinfo;
  g_lcd.dev.getpower=lcd_getpower;
  g_lcd.dev.setpower=lcd_setpower;
  g_lcd.power=1;
  return OK;
}
struct lcd_dev_s *board_lcd_getdev(int devno){ return &g_lcd.dev; }
void board_lcd_uninitialize(void){}
