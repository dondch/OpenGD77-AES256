/* Host-test stand-ins for dmr_prov_apply.c, so test_sms_rx.c can run with the provisioning
 * hooks compiled into dmr_sms.c (an unpaired radio: the hooks must change nothing). */
#include "functions/dmr_prov.h"
#include <string.h>
int  dmrProvApplyChannel(uint16_t i, const uint8_t *r) { (void)i; (void)r; return 1; }
int  dmrProvApplyZone(uint8_t z, const uint8_t *r) { (void)z; (void)r; return 1; }
int  dmrProvApplyVfo(uint8_t v, const uint8_t *r) { (void)v; (void)r; return 1; }
int  dmrProvApplySetting(uint8_t f, uint32_t v) { (void)f; (void)v; return 1; }
int  dmrProvApplySettingsSave(void) { return 1; }
int  dmrProvApplyStoreKey(uint8_t s, const uint8_t *k) { (void)s; (void)k; return 1; }
void dmrProvApplyReboot(void) { }
int  dmrProvApplyReadBack(uint8_t op, const uint8_t *b, int bl, uint8_t *o, int m) { (void)op; (void)b; (void)bl; (void)o; (void)m; return 0; }
int  dmrProvApplyTxAllowed(void) { return 1; }
void dmrProvApplyNotify(const char *t) { (void)t; }
uint16_t dmrProvApplyBatteryCentivolts(void) { return 0; }
const char *dmrProvApplyGitHash(void) { return "test"; }
int  dmrProvApplyBlockRead(uint8_t *b) { (void)b; return 0; }
int  dmrProvApplyBlockWrite(const uint8_t *b) { (void)b; return 1; }
static uint8_t s_arena[DMR_PROV_ARENA_BYTES];
uint8_t *dmrProvApplyArenaClaim(void) { return s_arena; }
void dmrProvApplyArenaRelease(void) { }
