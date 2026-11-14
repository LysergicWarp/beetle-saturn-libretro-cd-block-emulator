/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* cdb_lle.h - CD block run by the CD-block emulator library
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef __MDFN_SS_CDB_LLE_H
#define __MDFN_SS_CDB_LLE_H

#include <stdint.h>
#include <boolean.h>

#include "../cdrom/cdromif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set once CDBLLE_Init made the board. cdb.c's CDB_* calls then come here
 * instead of running the high-level CD block. */
extern bool CDBLLE_Active;

/* Makes the board from the CD-block ROM in the system directory. False, with
 * the CD block left to cdb.c, when the ROM is missing or the board cannot be
 * made. */
bool CDBLLE_Init(void);
void CDBLLE_Kill(void);

/* The core's tray and disc, which the drive follows: a disc changes with the
 * tray open. Kept while the board is not made, so a disc given before
 * CDBLLE_Init reaches the drive. CDBLLE_Kill forgets it. */
void CDBLLE_SetDisc(bool tray_open, CDIF *cdif);

/* The next stereo pair of the CD audio the drive plays, into outbuf[0] and
 * outbuf[1]. Leaves them as they are when there is none. */
void CDBLLE_GetCDDA(uint16_t *outbuf);

/* Power-on and SYSRES: the SMPC pulses CDRES for both, so the board restarts
 * and CDRES ends released. */
void CDBLLE_Reset(bool powering_up);
/* The SMPC's CDRES: asserted (CDOFF) resets the board and holds it there,
 * released (CDON) lets it start. */
void CDBLLE_SetResetLine(bool asserted);

int32_t CDBLLE_Update(int32_t timestamp);
void CDBLLE_ResetTS(void);
void CDBLLE_SetClockRatio(uint32_t ratio);

uint16_t CDBLLE_Read(uint32_t offset);
void CDBLLE_Write(uint32_t offset, uint16_t DB, uint16_t mask);

#ifdef __cplusplus
}
#endif

#endif
