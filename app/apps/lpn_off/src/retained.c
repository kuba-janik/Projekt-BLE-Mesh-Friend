#include "retained.h"

#include <stddef.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_DECLARE(lpn_off);

/* "LPNO" - druga (po CRC) bariera przed przypadkowymi smieciami w RAM */
#define RETAINED_MAGIC 0x4c504e4f

static const struct device *const retained_dev = DEVICE_DT_GET(DT_ALIAS(retainedmemdevice));

struct retained_data retained;

/* CRC liczone po wszystkim, co lezy przed polem crc */
#define RETAINED_CRC_OFFSET   offsetof(struct retained_data, crc)
#define RETAINED_CHECKED_SIZE (RETAINED_CRC_OFFSET + sizeof(retained.crc))

bool retained_load(void)
{
    if (!device_is_ready(retained_dev)) {
        LOG_ERR("Urzadzenie retencyjne niegotowe");
        memset(&retained, 0, sizeof(retained));
        return false;
    }

    int err = retained_mem_read(retained_dev, 0, (uint8_t *)&retained, sizeof(retained));
    if (err) {
        LOG_ERR("Odczyt bloku retencyjnego nieudany (err %d)", err);
        memset(&retained, 0, sizeof(retained));
        return false;
    }

    /* Reszta CRC-32/ISO-HDLC liczonego po wiadomosci z doklejonym CRC */
    const uint32_t residue = 0x2144df1c;
    bool valid = (crc32_ieee((const uint8_t *)&retained, RETAINED_CHECKED_SIZE) == residue) &&
                 (retained.magic == RETAINED_MAGIC);

    if (!valid) {
        /* Zimny start albo retencja wylaczona - startujemy od zera */
        memset(&retained, 0, sizeof(retained));
    }

    return valid;
}

void retained_save(void)
{
    if (!device_is_ready(retained_dev)) {
        return;
    }

    retained.magic = RETAINED_MAGIC;
    retained.crc = sys_cpu_to_le32(crc32_ieee((const uint8_t *)&retained, RETAINED_CRC_OFFSET));

    int err = retained_mem_write(retained_dev, 0, (const uint8_t *)&retained, sizeof(retained));
    if (err) {
        LOG_ERR("Zapis bloku retencyjnego nieudany (err %d)", err);
    }
}
