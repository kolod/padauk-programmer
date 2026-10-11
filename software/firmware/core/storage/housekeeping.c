/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "housekeeping.h"

#include "hal_flash.h"
#include "hal_time.h"
#include "journal.h"
#include "slots.h"

typedef enum { JOB_NONE = 0, JOB_JOURNAL, JOB_SLOT } job_kind_t;

#define RETRY_AFTER_FAIL_MS 1000u   /* a failing flash is not hammered */

static struct {
    job_kind_t kind;
    uint32_t addr;
    bool backoff;
    uint32_t failed_at;
} job;

void housekeeping_init(void)
{
    job.kind = JOB_NONE;
    job.backoff = false;
}

static void complete(hal_erase_status_t st)
{
    bool ok = st == HAL_ERASE_DONE;
    if (!ok) {
        job.backoff = true;
        job.failed_at = hal_time_ms();
    }
    if (job.kind == JOB_JOURNAL)
        journal_spare_erased(job.addr, ok);
    else if (job.kind == JOB_SLOT)
        slots_page_erased(job.addr, ok);
    job.kind = JOB_NONE;
}

void housekeeping_finish(void)
{
    if (job.kind == JOB_NONE)
        return;
    hal_erase_status_t st;
    while ((st = hal_flash_erase_status()) == HAL_ERASE_BUSY) {
    }
    complete(st);
}

void housekeeping_poll(void)
{
    if (job.kind != JOB_NONE) {
        hal_erase_status_t st = hal_flash_erase_status();
        if (st == HAL_ERASE_BUSY)
            return;
        complete(st);
        return;   /* one step per call: the next erase starts on the next poll */
    }
    if (job.backoff && hal_time_ms() - job.failed_at < RETRY_AFTER_FAIL_MS)
        return;
    job.backoff = false;
    uint32_t addr;
    job_kind_t kind = JOB_NONE;
    if (journal_spare_dirty(&addr))   /* the journal first: a full page needs the spare one */
        kind = JOB_JOURNAL;
    else if (slots_next_dirty(&addr))
        kind = JOB_SLOT;
    if (kind == JOB_NONE)
        return;
    job.kind = kind;
    job.addr = addr;
    if (!hal_flash_erase_start(addr))
        complete(HAL_ERASE_FAILED);
}

bool housekeeping_idle(void)
{
    uint32_t addr;
    return job.kind == JOB_NONE && !journal_spare_dirty(&addr) && !slots_next_dirty(&addr);
}
