#include "dk_compat.h" /* P4OS: the C library as the board has it */
/*
 * P4OS - Duke Nukem 3D: one player, no network.
 *
 * Chocolate Duke3D's multiplayer was enet over UDP (engine/mmulti.c) and
 * Ken's serial and modem code before it; neither is ported. These are the
 * engine's dummy_multi.c variables (numplayers = 1, this player is 0) and
 * the transport's functions as nothing: no packet is ever sent or comes in.
 * The game takes that as a single player, which is what it is.
 */
#include "platform.h"
#include "build.h"
#include "network.h"

short numplayers = 1, myconnectindex = 0;
short connecthead, connectpoint2[MAXPLAYERS];
uint8_t syncstate = 0;

void initmultiplayers(uint8_t damultioption, uint8_t dacomrateoption, uint8_t dapriority)
{
    (void)damultioption; (void)dacomrateoption; (void)dapriority;
    numplayers = 1;
    myconnectindex = 0;
    connecthead = 0;
    connectpoint2[0] = -1;
}

void uninitmultiplayers(void) {}
void sendpacket(int32_t other, uint8_t *bufptr, int32_t messleng) { (void)other; (void)bufptr; (void)messleng; }
void setpackettimeout(int32_t datimeoutcount, int32_t daresendagaincount) { (void)datimeoutcount; (void)daresendagaincount; }
void sendlogon(void) {}
void sendlogoff(void) {}
int  getoutputcirclesize(void) { return 0; }
void setsocket(short newsocket) { (void)newsocket; }
short getpacket(short *other, uint8_t *bufptr) { (void)other; (void)bufptr; return 0; }
void flushpackets(void) {}
void genericmultifunction(int32_t other, char *bufptr, int32_t messleng, int32_t command)
{
    (void)other; (void)bufptr; (void)messleng; (void)command;
}
