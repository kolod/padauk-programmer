/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "header.h"

#include <string.h>

#include "proto/crc32.h"
#include "proto/le.h"
#include "proto/proto_defs.h"

uint32_t pkg_mask_offset(uint16_t image_words)
{
    return PKG_HEADER_SIZE + 2u * image_words;
}

uint32_t pkg_expected_size(uint16_t image_words)
{
    uint32_t mask_bytes = 2u * ((image_words + 15u) / 16u);
    return pkg_mask_offset(image_words) + mask_bytes + 4u;
}

/* NUL-padded field: content, then only zeros. Optionally requires non-empty valid UTF-8 without
 * control characters. */
static bool text_field(const uint8_t *p, size_t field, bool required, bool ascii_only, char *out)
{
    size_t n = 0;
    while (n < field && p[n])
        n++;
    for (size_t i = n; i < field; i++)
        if (p[i])
            return false;
    if (required && n == 0)
        return false;
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        size_t len;
        if (c < 0x20 || c == 0x7F)
            return false;
        if (c < 0x80)
            len = 1;
        else if (ascii_only)
            return false;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4)
            len = 4;
        else
            return false;
        if (i + len > n)
            return false;
        for (size_t k = 1; k < len; k++)
            if ((p[i + k] & 0xC0) != 0x80)
                return false;
        i += len;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

uint16_t pkg_header_parse(const uint8_t *raw, bool from_host, uint32_t max_package_size,
                          pkg_header_t *h, const chip_info_t **chip)
{
    memset(h, 0, sizeof *h);
    if (memcmp(raw, "PDKP", 4) != 0 || le16(raw + 4) != PKG_FORMAT_VERSION || le16(raw + 6) != PKG_HEADER_SIZE)
        return PDK_PKG_HEADER_INVALID;
    if (crc32_compute(raw, 124) != le32(raw + 124))
        return PDK_PKG_HEADER_INVALID;

    h->package_size = le32(raw + 8);
    h->chip_id12 = le16(raw + 12);
    h->image_words = le16(raw + 14);
    h->flags = le32(raw + 16);
    h->fuse = le16(raw + 20);
    h->run_vdd_mV = le16(raw + 22);
    h->source_crc32 = le32(raw + 40);
    h->created_unix = le32(raw + 44);

    uint32_t allowed = from_host ? PKG_F_HOST_ALLOWED : PKG_F_INTERNAL_ALLOWED;
    if (h->flags & ~allowed)
        return PDK_PKG_HEADER_INVALID;
    for (int i = 24; i < 40; i++)
        if (raw[i])
            return PDK_PKG_HEADER_INVALID;
    if (!text_field(raw + 48, 16, true, true, h->chip_name) ||
        !text_field(raw + 64, 32, true, false, h->name) ||
        !text_field(raw + 96, 28, false, false, h->label))
        return PDK_PKG_HEADER_INVALID;
    if (h->image_words == 0 || h->package_size != pkg_expected_size(h->image_words))
        return PDK_PKG_HEADER_INVALID;

    const chip_info_t *c = chip_by_id(h->chip_id12);
    if (!c || (h->chip_id12 & ~0xFFFu) || !chip_name_matches(c, h->chip_name))
        return PDK_PKG_CHIP_UNSUPPORTED;
    if (!chip_can_write(c))
        return PDK_PKG_CHIP_READ_ONLY;
    if (h->image_words > c->code_words || h->package_size > max_package_size)
        return PDK_PKG_TOO_LARGE;
    if (!chip_is_flash(c) && (h->flags & PKG_F_SKIP_BLANK_CHECK))
        return PDK_PKG_OTP_BLANK_CHECK_REQUIRED;
    if ((h->flags & PKG_F_FUSE_PRESENT) && pkg_fuse_is_secure(h->fuse) && !(h->flags & PKG_F_ALLOW_SECURITY_FUSE))
        return PDK_PKG_SECURITY_NOT_ALLOWED;
    *chip = c;
    return PDK_OK;
}
