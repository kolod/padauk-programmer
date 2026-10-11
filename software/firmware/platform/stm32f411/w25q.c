/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Winbond W25Q SPI NOR flash on SPI1 (mode 0, 24 MHz), standard single-line commands only.
 * A sector erase can run in the background; reads and page programs of other sectors meanwhile
 * suspend it (75h) and resume it (7Ah) afterwards. */
#include "f411.h"
#include "hal_time.h"

#define CMD_WRITE_ENABLE 0x06
#define CMD_READ_STATUS1 0x05
#define CMD_READ_DATA 0x03
#define CMD_PAGE_PROGRAM 0x02
#define CMD_SECTOR_ERASE 0x20
#define CMD_RELEASE_POWER_DOWN 0xAB
#define CMD_JEDEC_ID 0x9F
#define CMD_ERASE_SUSPEND 0x75
#define CMD_ERASE_RESUME 0x7A
#define STATUS_BUSY 0x01
#define PAGE 256u

#define SECTOR_ERASE_TIMEOUT_MS 500u   /* datasheet max 400 ms */
#define PAGE_PROGRAM_TIMEOUT_MS 5u     /* max 3 ms */
#define SUSPEND_TIMEOUT_MS 2u          /* tSUS max 20 us */
#define MIN_RUN_AFTER_RESUME_US 500u   /* let the erase progress between suspends */

static bool present;
static bool erasing;          /* a background sector erase was started and not yet seen finished */
static bool suspended;
static bool timed_out;          /* the last erase never finished: flash is faulty */
static uint32_t resumed_at, erase_started_ms;

static void cs(bool active) { GPIOA->BSRR = active ? (1u << (4 + 16)) : (1u << 4); }

static uint8_t xfer(uint8_t b)
{
    while (!(SPI1->SR & SPI_SR_TXE)) {
    }
    *(volatile uint8_t *)&SPI1->DR = b;
    while (!(SPI1->SR & SPI_SR_RXNE)) {
    }
    return *(volatile uint8_t *)&SPI1->DR;
}

static void end(void)
{
    while (SPI1->SR & SPI_SR_BSY) {
    }
    cs(false);
}

static void command(uint8_t cmd)
{
    cs(true);
    xfer(cmd);
    end();
}

static void command_addr(uint8_t cmd, uint32_t addr)
{
    cs(true);
    xfer(cmd);
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
}

static bool busy(void)
{
    cs(true);
    xfer(CMD_READ_STATUS1);
    uint8_t st = xfer(0xFF);
    end();
    return (st & STATUS_BUSY) != 0;
}

static bool wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = hal_time_ms();
    while (busy())
        if (hal_time_ms() - t0 > timeout_ms)
            return false;
    return true;
}

/* Makes the array accessible: a running background erase is suspended (or found finished). */
static void suspend(void)
{
    if (!erasing || suspended)
        return;
    if (!busy()) {
        erasing = false;   /* finished on its own */
        return;
    }
    while (hal_time_us() - resumed_at < MIN_RUN_AFTER_RESUME_US) {
    }
    command(CMD_ERASE_SUSPEND);
    wait_ready(SUSPEND_TIMEOUT_MS);
    suspended = true;
}

static void resume(void)
{
    if (!suspended)
        return;
    command(CMD_ERASE_RESUME);
    suspended = false;
    resumed_at = hal_time_us();
}

bool w25q_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    GPIOA->BSRR = 1u << 4;   /* CS high before it becomes an output */
    GPIOA->MODER = (GPIOA->MODER & ~(0xFFu << 8)) | (1u << 8) | (2u << 10) | (2u << 12) | (2u << 14);
    GPIOA->OSPEEDR |= 0xFFu << 8;
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 12)) | (1u << 12);   /* MISO pull-up: reads 0xFF if no chip */
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFFFu << 20)) | (5u << 20) | (5u << 24) | (5u << 28);   /* AF5 */

    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (1u << SPI_CR1_BR_Pos);   /* 96 MHz / 4 */
    SPI1->CR1 |= SPI_CR1_SPE;

    command(CMD_RELEASE_POWER_DOWN);
    hal_delay_us(50);   /* tRES1 = 3 µs */
    cs(true);
    xfer(CMD_JEDEC_ID);
    uint8_t mfr = xfer(0xFF), type = xfer(0xFF), cap = xfer(0xFF);
    end();
    (void)cap;
    present = mfr == 0xEF && (type == 0x40 || type == 0x60 || type == 0x70);   /* Winbond W25Q */
    return present && wait_ready(SECTOR_ERASE_TIMEOUT_MS);
}

bool w25q_present(void) { return present; }

void w25q_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (!present) {
        for (uint32_t i = 0; i < len; i++)
            buf[i] = 0xFF;
        return;
    }
    suspend();
    command_addr(CMD_READ_DATA, addr);
    while (len--)
        *buf++ = xfer(0xFF);
    end();
    resume();
}

bool w25q_erase_done(void)
{
    if (erasing && !suspended) {
        if (!busy())
            erasing = false;
        else if (hal_time_ms() - erase_started_ms > SECTOR_ERASE_TIMEOUT_MS) {
            erasing = false;   /* give up; the page stays dirty and is blank-checked anyway */
            timed_out = true;
        }
    }
    return !erasing;
}

bool w25q_erase_timed_out(void)
{
    bool t = timed_out;
    timed_out = false;
    return t;
}

bool w25q_erase_start(uint32_t addr)
{
    if (!present || (addr & 0xFFFu))
        return false;
    while (!w25q_erase_done()) {   /* one erase at a time */
    }
    command(CMD_WRITE_ENABLE);
    command_addr(CMD_SECTOR_ERASE, addr);
    end();
    erasing = true;
    timed_out = false;
    erase_started_ms = hal_time_ms();
    return true;
}

bool w25q_erase_sector(uint32_t addr)
{
    if (!w25q_erase_start(addr))
        return false;
    while (!w25q_erase_done()) {
    }
    return !w25q_erase_timed_out();
}

bool w25q_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!present)
        return false;
    suspend();   /* page program is allowed while an erase of another sector is suspended */
    bool ok = true;
    while (len && ok) {
        uint32_t n = PAGE - (addr % PAGE);
        if (n > len)
            n = len;
        command(CMD_WRITE_ENABLE);
        command_addr(CMD_PAGE_PROGRAM, addr);
        for (uint32_t i = 0; i < n; i++)
            xfer(data[i]);
        end();
        ok = wait_ready(PAGE_PROGRAM_TIMEOUT_MS);
        addr += n;
        data += n;
        len -= n;
    }
    resume();
    return ok;
}
