#include "usbc_port.h"
#include "bsp_config.h"
#include "main.h"

#ifdef BSP_USBC_VBUS_SWITCH

/* ── Timing ─────────────────────────────────────────────────────────────────
 * One step per USBC_STEP_MS. While unattached, each role is held for
 * USBC_ROLE_STEPS steps (the first is a settle step, then one comparator
 * read). An attach needs USBC_ATTACH_DEBOUNCE consecutive positive reads in
 * the same role (Type-C tCCDebounce is 100..200 ms); with the toggling that
 * lands in that window. A detach needs USBC_DETACH_DEBOUNCE reads, kept
 * short so a display unplug cuts VBUS long before a PC cable could be
 * plugged in its place. */
#define USBC_STEP_MS          10u
#define USBC_ROLE_STEPS       5u    /* 50 ms per role while toggling */
#define USBC_ATTACH_DEBOUNCE  3u
#define USBC_DETACH_DEBOUNCE  2u

typedef enum { ROLE_SOURCE, ROLE_SINK } Role_t;

static UsbcState_t s_state = USBC_UNATTACHED;
static Role_t      s_role  = ROLE_SINK;
static uint8_t     s_role_step;
static uint8_t     s_hits;
static uint8_t     s_misses;
static uint8_t     s_attached_cc;      /* 1 or 2 */
static uint32_t    s_last_step_ms;

static void vbus_5v(bool on)
{
    HAL_GPIO_WritePin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void set_role(Role_t r)
{
    s_role = r;
    if (r == ROLE_SOURCE) {
        LL_UCPD_SetSRCRole(UCPD1);
        LL_UCPD_SetRpResistor(UCPD1, LL_UCPD_RESISTOR_DEFAULT);
    } else {
        LL_UCPD_SetSNKRole(UCPD1);
    }
    s_role_step = 0;
    s_hits = 0;
}

/* Source role: which CC sees an Rd (a sink asking for power), 0 if none.
 * vRa (a cable's Ra, or nothing driven) and open do not count. */
static uint8_t source_sees_rd(void)
{
    if (LL_UCPD_GetTypeCVstateCC1(UCPD1) == LL_UCPD_SRC_CC1_VRD) return 1;
    if (LL_UCPD_GetTypeCVstateCC2(UCPD1) == LL_UCPD_SRC_CC2_VRD) return 2;
    return 0;
}

/* Sink role: which CC sees an Rp (a source offering power), 0 if none. */
static uint8_t sink_sees_rp(void)
{
    if (LL_UCPD_GetTypeCVstateCC1(UCPD1) != LL_UCPD_SNK_CC1_VOPEN) return 1;
    if (LL_UCPD_GetTypeCVstateCC2(UCPD1) != LL_UCPD_SNK_CC2_VOPEN) return 2;
    return 0;
}

static bool still_attached(void)
{
    if (s_state == USBC_ATTACHED_DISPLAY) {
        uint32_t v = (s_attached_cc == 1) ? LL_UCPD_GetTypeCVstateCC1(UCPD1) : LL_UCPD_GetTypeCVstateCC2(UCPD1);
        uint32_t rd = (s_attached_cc == 1) ? LL_UCPD_SRC_CC1_VRD : LL_UCPD_SRC_CC2_VRD;
        return v == rd;
    }
    /* PC: Rp still present on the attached pin */
    uint32_t v = (s_attached_cc == 1) ? LL_UCPD_GetTypeCVstateCC1(UCPD1) : LL_UCPD_GetTypeCVstateCC2(UCPD1);
    return v != 0u;
}

void USBC_Init(void)
{
    vbus_5v(false);                     /* never on before something asked for it */
    LL_UCPD_Disable(UCPD1);
    UCPD1->CFG1 = 0;                    /* defaults: no PD messaging used */
    LL_UCPD_Enable(UCPD1);
    LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC1CC2);
    set_role(ROLE_SINK);
    s_state = USBC_UNATTACHED;
    s_last_step_ms = HAL_GetTick();
}

void USBC_Process(void)
{
    uint32_t now = HAL_GetTick();
    if (now - s_last_step_ms < USBC_STEP_MS)
        return;
    s_last_step_ms = now;

    switch (s_state) {
    case USBC_UNATTACHED: {
        if (++s_role_step == 1u)
            return;                     /* settle step after a role change */
        uint8_t cc = (s_role == ROLE_SOURCE) ? source_sees_rd() : sink_sees_rp();
        if (cc != 0u) {
            if (++s_hits >= USBC_ATTACH_DEBOUNCE) {
                s_attached_cc = cc;
                s_misses = 0;
                if (s_role == ROLE_SOURCE) {
                    s_state = USBC_ATTACHED_DISPLAY;
                    vbus_5v(true);
                } else {
                    s_state = USBC_ATTACHED_PC;   /* 5V already off */
                }
            }
            return;                     /* hold this role while confirming */
        }
        s_hits = 0;
        if (s_role_step >= USBC_ROLE_STEPS)
            set_role(s_role == ROLE_SOURCE ? ROLE_SINK : ROLE_SOURCE);
        return;
    }
    case USBC_ATTACHED_DISPLAY:
    case USBC_ATTACHED_PC:
        if (still_attached()) {
            s_misses = 0;
            return;
        }
        if (++s_misses >= USBC_DETACH_DEBOUNCE) {
            vbus_5v(false);
            s_state = USBC_UNATTACHED;
            set_role(ROLE_SINK);
        }
        return;
    }
}

UsbcState_t USBC_GetState(void) { return s_state; }
bool        USBC_Is5VOn(void)   { return s_state == USBC_ATTACHED_DISPLAY; }
bool        USBC_Enabled(void)  { return true; }

void USBC_DebugCC(uint8_t *cc1, uint8_t *cc2, bool *source_role)
{
    *cc1 = (uint8_t)(LL_UCPD_GetTypeCVstateCC1(UCPD1) >> UCPD_SR_TYPEC_VSTATE_CC1_Pos);
    *cc2 = (uint8_t)(LL_UCPD_GetTypeCVstateCC2(UCPD1) >> UCPD_SR_TYPEC_VSTATE_CC2_Pos);
    *source_role = (s_role == ROLE_SOURCE);
}

#else  /* !BSP_USBC_VBUS_SWITCH: board 5V permanently on, as before */

void USBC_Init(void)
{
    HAL_GPIO_WritePin(M6_EN_GPIO_Port, M6_EN_Pin, GPIO_PIN_SET);
}
void        USBC_Process(void)  {}
UsbcState_t USBC_GetState(void) { return USBC_UNATTACHED; }
bool        USBC_Is5VOn(void)   { return HAL_GPIO_ReadPin(M6_EN_GPIO_Port, M6_EN_Pin) == GPIO_PIN_SET; }
bool        USBC_Enabled(void)  { return false; }
void USBC_DebugCC(uint8_t *cc1, uint8_t *cc2, bool *source_role) { *cc1 = *cc2 = 0; *source_role = false; }

#endif

const char *USBC_StateStr(void)
{
    if (!USBC_Enabled()) return "disabled (5V always on)";
    switch (USBC_GetState()) {
        case USBC_ATTACHED_DISPLAY: return "display attached, 5V on";
        case USBC_ATTACHED_PC:      return "PC attached, 5V off";
        default:                    return "nothing attached, 5V off";
    }
}
