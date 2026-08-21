#include "params.h"
#include "tec_control.h"
#include "laser_control.h"
#include "bsp_temp.h"
#include "stm32g4xx_hal.h"
#include <string.h>

#define PARAMS_MAGIC   0x4C415352u   /* "LASR" */
#define PARAMS_PAGE    63u
#define PARAMS_BASE    0x0801F800u
/* PARAMS_MAX * sizeof(FlashParams_t) must fit inside the 2 KB FLASH_PARAMS
 * region (STM32G431XX_FLASH.ld) -- which is also the last page of physical
 * flash on this chip, so overflowing it means writing past the end of flash
 * entirely, not just into unintended memory. sizeof(FlashParams_t) is
 * currently 104 bytes (13 doublewords); 2048/104 = 19.69, so 19 is the max
 * that fits. Re-check this by hand any time FlashParams_t grows. */
#define PARAMS_MAX     19u
#define DEBOUNCE_MS    20000u

static uint8_t  s_dirty;
static uint32_t s_dirty_tick;

/* ── CRC32 (IEEE 802.3 polynomial) ──────────────────────────────────────── */

static uint32_t crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ── Flash low-level ─────────────────────────────────────────────────────── */

static int flash_erase_params_page(void)
{
    FLASH_EraseInitTypeDef e = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks     = FLASH_BANK_1,
        .Page      = PARAMS_PAGE,
        .NbPages   = 1u,
    };
    uint32_t page_error;
    return (HAL_FLASHEx_Erase(&e, &page_error) == HAL_OK) ? 0 : -1;
}

static int flash_write_entry(uint32_t addr, const FlashParams_t *p)
{
    const uint64_t *src   = (const uint64_t *)(const void *)p;
    uint32_t        words = sizeof(FlashParams_t) / 8u;
    for (uint32_t i = 0; i < words; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              addr + i * 8u, src[i]) != HAL_OK)
            return -1;
    }
    return 0;
}

/* ── Slot scan ───────────────────────────────────────────────────────────── */

static int find_best_slot(uint32_t *seq_out)
{
    int      best = -1;
    uint32_t best_seq = 0u;

    for (int i = 0; i < (int)PARAMS_MAX; i++) {
        const FlashParams_t *p =
            (const FlashParams_t *)(PARAMS_BASE + (uint32_t)i * sizeof(FlashParams_t));
        if (p->magic != PARAMS_MAGIC) continue;
        uint32_t calc = crc32((const uint8_t *)p,
                               sizeof(FlashParams_t) - sizeof(uint32_t));
        if (calc != p->crc) continue;
        if (best < 0 || p->seq > best_seq) {
            best     = i;
            best_seq = p->seq;
        }
    }
    if (seq_out) *seq_out = best_seq;
    return best;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void Params_Load(void)
{
    int best = find_best_slot(NULL);
    if (best < 0) return;

    const FlashParams_t *p =
        (const FlashParams_t *)(PARAMS_BASE + (uint32_t)best * sizeof(FlashParams_t));

    g_crystal_pid.Kp       = p->crystal_kp;
    g_crystal_pid.Ki       = p->crystal_ki;
    g_crystal_pid.Kd       = p->crystal_kd;
    g_crystal_pid.setpoint = p->crystal_setpoint;
    g_crystal_wl_k         = p->crystal_wl_k;
    g_crystal_wl_m         = p->crystal_wl_m;

    g_laser_pid.Kp         = p->laser_kp;
    g_laser_pid.Ki         = p->laser_ki;
    g_laser_pid.Kd         = p->laser_kd;
    g_laser_pid.setpoint   = p->laser_setpoint;

    Laser_Control_SetMaxCurrent(p->laser_max_current_A);
    Laser_Control_SetThreshold(p->laser_threshold_A);
    Laser_Control_SetPower(p->laser_power_pct);

    g_laser_temp_max_dev_C = p->temp_max_dev_C;
    g_laser_temp_abs_max_C = p->temp_abs_max_C;
    g_laser_temp_timer_s   = p->temp_timer_s;

    g_ntc_vref_v      = p->ntc_vref_v;
    g_ntc_rseries_ohm = p->ntc_rseries_ohm;
    g_ntc_sh_a        = p->ntc_sh_a;
    g_ntc_sh_b        = p->ntc_sh_b;
    g_ntc_sh_c        = p->ntc_sh_c;
}

int Params_Save(void)
{
    uint32_t best_seq  = 0u;
    int      best      = find_best_slot(&best_seq);
    uint32_t next_slot = (best >= 0) ? (uint32_t)best + 1u : 0u;
    uint32_t next_seq  = (best >= 0) ? best_seq + 1u : 1u;

    HAL_FLASH_Unlock();

    if (best < 0 || next_slot >= PARAMS_MAX) {
        if (flash_erase_params_page() != 0) {
            HAL_FLASH_Lock();
            return -1;
        }
        next_slot = 0u;
    }

    __attribute__((aligned(8))) FlashParams_t p = {0};
    p.magic              = PARAMS_MAGIC;
    p.seq                = next_seq;
    p.crystal_kp         = g_crystal_pid.Kp;
    p.crystal_ki         = g_crystal_pid.Ki;
    p.crystal_kd         = g_crystal_pid.Kd;
    p.crystal_setpoint   = g_crystal_pid.setpoint;
    p.crystal_wl_k       = g_crystal_wl_k;
    p.crystal_wl_m       = g_crystal_wl_m;
    p.crystal_wavelength_nm = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
    p.laser_kp           = g_laser_pid.Kp;
    p.laser_ki           = g_laser_pid.Ki;
    p.laser_kd           = g_laser_pid.Kd;
    p.laser_setpoint     = g_laser_pid.setpoint;
    p.laser_max_current_A = g_laser_state.max_current_A;
    p.laser_threshold_A  = g_laser_state.threshold_A;
    p.laser_power_pct    = g_laser_state.power_pct;
    p.temp_max_dev_C     = g_laser_temp_max_dev_C;
    p.temp_abs_max_C     = g_laser_temp_abs_max_C;
    p.temp_timer_s       = g_laser_temp_timer_s;
    p.ntc_vref_v         = g_ntc_vref_v;
    p.ntc_rseries_ohm    = g_ntc_rseries_ohm;
    p.ntc_sh_a           = g_ntc_sh_a;
    p.ntc_sh_b           = g_ntc_sh_b;
    p.ntc_sh_c           = g_ntc_sh_c;
    p.crc = crc32((const uint8_t *)&p, sizeof(FlashParams_t) - sizeof(uint32_t));

    uint32_t addr   = PARAMS_BASE + next_slot * (uint32_t)sizeof(FlashParams_t);
    int      result = flash_write_entry(addr, &p);

    HAL_FLASH_Lock();
    s_dirty = 0;
    return result;
}

void Params_MarkDirty(void)
{
    s_dirty      = 1u;
    s_dirty_tick = HAL_GetTick();
}

void Params_Process(void)
{
    if (s_dirty && (HAL_GetTick() - s_dirty_tick >= DEBOUNCE_MS))
        Params_Save();
}
