/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* cdb_lle.c - CD block run by the CD-block emulator library
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

/*
 * The CD block's SH-1 board, run by the CD-block emulator library with the
 * real CD-block firmware, in place of cdb.c's high-level emulation. The board
 * runs with the library's clock and device timing on, and with its own disc
 * drive, which reads the loaded disc through a sector provider. An MPEG card
 * sits in its slot when the MPEG card option names a ROM. The card has no
 * decoder behind it, so it shows no picture.
 *
 * Timing: the board runs in batches of SH-1 cycles from the CD block's event,
 * and catches up before the host's accesses where an answer depends on it. A
 * word through the data port runs the board ahead by the bus's time, paid back
 * from the host time after it. Beetle's buses cannot hold an access, so a data
 * port access the FIFO cannot serve runs the board ahead until it can.
 *
 * Not modelled: CD audio (silent), disc changes (the drive reads the new disc
 * as the old one's), save states (the CD block is left out of them).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <boolean.h>
#include <libretro.h>
#include <streams/file_stream.h>

#include <cdblock.h>

#include "ss.h"
#include "scu.h"
#include "cdb_lle.h"

#include "../settings.h"
#include "../general.h"
#include "../cdrom/CDUtility.h"
#include "../cdrom/cdromif.h"

extern retro_log_printf_t log_cb;

/* The CD-block ROM's file in the system directory. */
#define ROM_NAME "cdblock.bin"
#define ROM_SIZE 0x10000
#define CARD_ROM_MAX 0x80000

/* The board's clock, the SH-1's 20 MHz, against the 11.2896 MHz clock
 * CDB_SetClockRatio's ratio counts. */
#define SH1_HZ 20000000ULL
#define HLE_HZ 11289600ULL

/* SH-1 cycles the board runs at a time without a host access. */
#define BATCH_CYCLES 4096ULL

/* SH-1 cycles a word through the data port runs the board ahead: the 40
 * cycles the main bus takes for an access to the CD block, at about 0.7 SH-1
 * cycles each. Run in steps, so a host DMA that moves a sector at once cannot
 * finish it while the SH-1 stands still. */
#define DATA_WORD_CYCLES 28ULL
#define AHEAD_STEP_CYCLES 256ULL

/* SH-1 cycles the board may stay behind when the host reads the flags or a
 * response word, so a polling host does not run it a few cycles at a time. */
#define POLL_CYCLES 512ULL

/* SH-1 cycles a data port access may run the board ahead waiting for the FIFO,
 * about 13 ms, past which it reads the last word again or drops the write. */
#define FIFO_WAIT_CYCLES 262144ULL

bool CDBLLE_Active = false;

static cdblock_board *Board;
static bool Stopped; /* a failure stopped the board, until the next reset */
static bool Held;    /* CDRES holds the board in reset */

static uint64_t SH1Ratio = (((1ULL << 32) * 20000000ULL) / 28636363ULL); /* SH-1 cycles per SH-2 cycle, 32.32 */
static int32_t BatchSH2 = 5865; /* SH-2 cycles in a batch, rounded up */
static int32_t LastTS;
static uint64_t Fraction; /* the SH-1 cycle's fraction not run yet, low 32 bits */
static uint64_t Owed;     /* SH-1 cycles not run yet */
static uint64_t Ahead;    /* SH-1 cycles the data port ran ahead, not paid back yet */

/* Host side of the interface chip */
static uint16_t CR[4]; /* CR1..CR3 as written, sent with CR4 */
static uint16_t HIRQMask;
static uint16_t LastData; /* the last word read from the data port */

static CDIF *Disc;
static uint32_t DiscEnd; /* the disc's last frame address, 0 without a disc */

static uint8_t *CardROM;
static size_t CardROMSize;

static uint8_t RingBody[2352];
static bool RingBodyMade;

/* Debug, from the environment:
 * - CDB_LLE_TRANSCRIPT names a file for the host's commands and the responses
 *   it reads, by SH-1 cycle. CDB_LLE_TRANSCRIPT_DATA adds every data port
 *   word the host reads.
 * - CDB_LLE_LENIENT logs the strict checks that fail instead of stopping the
 *   board. */
static FILE *Transcript;
static bool TranscriptData;
static uint16_t RR[4]; /* the response words the host read */

static void Trace(bool flush, const char *format, ...)
{
   va_list ap;

   if(!Transcript)
      return;
   fprintf(Transcript, "%012llu ", Board ? (unsigned long long)cdblock_clock_cycles(Board) : 0ULL);
   va_start(ap, format);
   vfprintf(Transcript, format, ap);
   va_end(ap);
   fputc('\n', Transcript);
   if(flush)
      fflush(Transcript);
}

static void Log(enum retro_log_level level, const char *format, ...)
{
   char text[512];
   va_list ap;

   if(!log_cb)
      return;

   va_start(ap, format);
   vsnprintf(text, sizeof(text), format, ap);
   va_end(ap);

   log_cb(level, "CDB-LLE: %s\n", text);
}

static uint8_t *ReadFirmware(const char *name, size_t max, size_t *size)
{
   char path[4096];
   RFILE *fp;
   int64_t len;
   uint8_t *data;

   *size = 0;
   if(!MDFN_MakeFName(path, sizeof(path), MDFNMKF_FIRMWARE, 0, name))
      return NULL;
   fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if(!fp)
      return NULL;

   filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_END);
   len = filestream_tell(fp);
   filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_START);

   if(len <= 0 || (uint64_t)len > max)
   {
      Log(RETRO_LOG_ERROR, "%s is %lld bytes, at most %u expected", path, (long long)len, (unsigned)max);
      filestream_close(fp);
      return NULL;
   }

   data = (uint8_t *)malloc((size_t)len);
   if(!data || filestream_read(fp, data, len) != len)
   {
      Log(RETRO_LOG_ERROR, "could not read %s", path);
      free(data);
      filestream_close(fp);
      return NULL;
   }

   filestream_close(fp);
   *size = (size_t)len;
   return data;
}

static void RecalcIRQ(void)
{
   const bool out = Board && !Stopped && !Held && (cdblock_host_flags(Board) & HIRQMask);

   SCU_SetInt(16, out);
}

static void Fail(const char *what)
{
   Log(RETRO_LOG_ERROR, "%s: %s. The CD block stops until the next reset", what,
       cdblock_last_error(Board));
   Stopped = true;
   RecalcIRQ();
}

static void ProcessLog(void)
{
   const size_t count = cdblock_log_count(Board);
   size_t i;

   if(!count)
      return;

   for(i = 0; i < count; i++)
   {
      cdblock_log_entry entry;

      memset(&entry, 0, sizeof(entry));
      if(cdblock_log_get(Board, i, &entry) < 0)
         continue;
      if(entry.kind == CDBLOCK_LOG_VIOLATION)
         Log(RETRO_LOG_WARN, "check at PC %08X: %s", entry.pc, entry.text ? entry.text : "");
   }
   cdblock_clear_log(Board);
}

static void CatchUp(void)
{
   uint64_t end, now;

   if(!Owed)
      return;
   if(!Board || Stopped || Held)
   {
      Owed = 0;
      return;
   }

   /* One instruction is one cycle of the board's clock. A SLEEP lets the
    * clock run to its next event. */
   end = cdblock_clock_cycles(Board) + Owed;
   Owed = 0;
   for(now = cdblock_clock_cycles(Board); now < end; now = cdblock_clock_cycles(Board))
   {
      const int reason = cdblock_run(Board, end - now, NULL);

      if(reason == CDBLOCK_EXIT_ERROR)
      {
         ProcessLog();
         Fail("The board failed");
         return;
      }
      if(reason == CDBLOCK_EXIT_SLEEP)
      {
         const uint64_t at = cdblock_clock_cycles(Board);
         const uint64_t next = cdblock_clock_next(Board);
         const uint64_t skip = (next > 0 && next < end - at) ? next : end - at;

         if(cdblock_clock_advance(Board, skip) < 0)
         {
            Fail("The board's clock stopped");
            return;
         }
      }
   }

   ProcessLog();
   RecalcIRQ();
}

/* Host time up to the timestamp, in SH-1 cycles owed, less what the data port
 * ran ahead. Time going back, as a lagging slave SH-2's, is ignored. */
static void Sync(int32_t timestamp)
{
   uint64_t total, cycles, repaid;

   if(timestamp <= LastTS)
      return;

   total = (uint64_t)(timestamp - LastTS) * SH1Ratio + Fraction;
   LastTS = timestamp;
   Fraction = total & 0xFFFFFFFFULL;
   cycles = total >> 32;

   repaid = (Ahead < cycles) ? Ahead : cycles;
   Ahead -= repaid;
   Owed += cycles - repaid;
}

static void RunAhead(uint64_t cycles)
{
   Ahead += cycles;
   Owed += cycles;
   if(Owed >= AHEAD_STEP_CYCLES)
      CatchUp();
}

/* Whether the FIFO serves a data port access now: the transfer enabled in its
 * direction, and a word or the room for one there. */
static bool FIFOServes(bool write)
{
   uint16_t control = 0;
   const size_t words = cdblock_host_fifo(Board, &control);

   if(!(control & 0x4) || ((control & 0x1) != 0) != write)
      return false;
   return write ? words < CDBLOCK_FIFO_WORDS : words > 0;
}

/* The interface chip holds a data port access while the transfer is enabled
 * in its direction and the FIFO cannot serve it. Beetle's buses cannot, so the
 * board runs ahead instead, paid back from the host time after the access. */
static void WaitFIFO(bool write)
{
   uint64_t waited = 0;

   while(!Stopped)
   {
      uint16_t control = 0;

      cdblock_host_fifo(Board, &control);
      if(!(control & 0x4) || ((control & 0x1) != 0) != write || FIFOServes(write))
         return;
      if(waited >= FIFO_WAIT_CYCLES)
      {
         Trace(true, "fifo %s gave up, control %04X", write ? "write" : "read", control);
         Log(RETRO_LOG_DEBUG, "data port %s with the FIFO %s after %llu cycles, transfer control %04X",
             write ? "write" : "read", write ? "full" : "empty", (unsigned long long)waited, control);
         return;
      }
      RunAhead(AHEAD_STEP_CYCLES);
      waited += AHEAD_STEP_CYCLES;
   }
}

static uint16_t ReadData(void)
{
   /* Toward the host only, bit 0 of the transfer control clear. With the
    * FIFO empty the last word reads again. */
   uint16_t control = 0;
   uint16_t value = 0;
   long moved;

   cdblock_host_fifo(Board, &control);
   if(control & 0x1)
      return 0;

   moved = cdblock_host_transfer(Board, 1, NULL, &value);
   if(moved < 0)
   {
      Fail("Data port read failed");
      return 0;
   }
   if(moved > 0)
      LastData = value;
   if(TranscriptData)
      Trace(false, "data %04X", LastData);
   return LastData;
}

static void WriteData(uint16_t value)
{
   /* From the host only, bit 0 of the transfer control set. A word the full
    * FIFO cannot take is dropped. */
   uint16_t control = 0;

   cdblock_host_fifo(Board, &control);
   if(!(control & 0x1))
      return;
   if(cdblock_host_transfer(Board, 1, &value, NULL) < 0)
      Fail("Data port write failed");
}

/* The security ring sits past the disc's end, where images hold nothing. The
 * firmware's logo check scrambles the user data again and compares each byte
 * with a8 for a set logo bit or 59 for a clear one, and a column passes with
 * half its bytes matching. So user data that scrambles back to a8 and 59 in
 * turn passes every row. Framed as the disc's own ring: mode 2 form 2,
 * subheader 00 00 28 00, EDC 0. */
static void MakeRingBody(void)
{
   /* ECMA-130's scrambler: x^15 + x + 1 from 1, over bytes 12 on, each byte's
    * bits taken from bit 0 first. */
   uint16_t lfsr = 1;
   unsigned i, bit;

   for(i = 12; i < sizeof(RingBody); i++)
   {
      uint8_t s = 0;

      for(bit = 0; bit < 8; bit++)
      {
         s |= (uint8_t)((lfsr & 1) << bit);
         lfsr = (uint16_t)((lfsr >> 1) | (((lfsr ^ (lfsr >> 1)) & 1) << 14));
      }
      RingBody[i] = s ^ ((i & 1) ? 0x59 : 0xA8);
   }
   RingBodyMade = true;
}

static uint8_t BCD(unsigned value)
{
   return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static void WriteRingSector(uint32_t frame, uint8_t *raw)
{
   static const uint8_t subheader[8] = { 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x28, 0x00 };

   if(!RingBodyMade)
      MakeRingBody();

   raw[0] = 0x00;
   memset(raw + 1, 0xFF, 10);
   raw[11] = 0x00;
   raw[12] = BCD(frame / 75 / 60);
   raw[13] = BCD(frame / 75 % 60);
   raw[14] = BCD(frame % 75);
   raw[15] = 0x02;
   memcpy(raw + 16, subheader, sizeof(subheader));
   memcpy(raw + 24, RingBody + 24, 2348 - 24);
   memset(raw + 2348, 0, 4);
}

static int ProvideSector(void *user, uint32_t frame, uint8_t *raw)
{
   uint8_t sector[2352 + 96];

   (void)user;
   if(!Disc || !DiscEnd)
      return 0;
   if(frame > DiscEnd)
   {
      WriteRingSector(frame, raw);
      return 0;
   }
   /* A frame the image cannot give keeps the drive's own sector, sync and a
    * mode 1 header. */
   if(CDIF_ReadRawSector(Disc, sector, (int32_t)frame - 150))
      memcpy(raw, sector, 2352);
   return 0;
}

static void SetUpDisc(void)
{
   static TOC toc;
   cdblock_track tracks[99];
   size_t count = 0;
   unsigned number;
   uint32_t lead_out;

   DiscEnd = 0;
   if(!Board || !Disc)
      return;

   CDIF_ReadTOC(Disc, &toc);
   for(number = toc.first_track; number && number <= toc.last_track && number <= 99; number++)
   {
      if(!toc.tracks[number].valid)
         continue;
      tracks[count].start = toc.tracks[number].lba + 150;
      tracks[count].control = (uint8_t)((toc.tracks[number].control << 4) | (toc.tracks[number].adr & 0x0F));
      count++;
   }
   lead_out = toc.tracks[100].lba + 150;

   if(cdblock_drive_disc_tracks(Board, tracks, count, lead_out) < 0)
   {
      Log(RETRO_LOG_ERROR, "the disc's tracks do not fit the drive: %s", cdblock_last_error(Board));
      return;
   }
   DiscEnd = lead_out - 1;
   Log(RETRO_LOG_INFO, "disc of %u tracks, lead-out at frame %u", (unsigned)count, lead_out);
}

static void ResetBoard(void)
{
   Owed = 0;
   Ahead = 0;
   memset(CR, 0, sizeof(CR));
   HIRQMask = 0;
   LastData = 0;
   if(!Board)
      return;
   if(cdblock_reset(Board) < 0)
   {
      Fail("Reset failed");
      return;
   }
   cdblock_clear_log(Board);
   Stopped = false;
   RecalcIRQ();
}

bool CDBLLE_Init(void)
{
   cdblock_options options;
   size_t rom_size = 0;
   uint8_t *rom;

   if(Board)
      CDBLLE_Kill();

   rom = ReadFirmware(ROM_NAME, ROM_SIZE, &rom_size);
   if(!rom)
   {
      Log(RETRO_LOG_INFO, "no %s in the system directory, the CD block stays high-level", ROM_NAME);
      return false;
   }

   if(MDFN_GetSettingB("ss.mpeg_card"))
   {
      const char *name = MDFN_GetSettingS("ss.mpeg_card_path");

      if(name && name[0])
         CardROM = ReadFirmware(name, CARD_ROM_MAX, &CardROMSize);
   }

   memset(&options, 0, sizeof(options));
   options.size = sizeof(options);
   options.flags = CDBLOCK_DEVICE_TIMING | CDBLOCK_CLOCK;
   options.card_rom = CardROM;
   options.card_rom_size = CardROMSize;

   Board = cdblock_create(rom, rom_size, &options);
   free(rom);
   if(!Board)
   {
      Log(RETRO_LOG_ERROR, "could not make the board: %s", cdblock_last_error(NULL));
      free(CardROM);
      CardROM = NULL;
      CardROMSize = 0;
      return false;
   }

   cdblock_log_kinds(Board, 1u << CDBLOCK_LOG_VIOLATION);
   if(cdblock_drive_select_disc(Board, NULL) < 0 ||
      cdblock_drive_disc_sector_provider(Board, ProvideSector, NULL) < 0)
   {
      Log(RETRO_LOG_ERROR, "could not set up the disc drive: %s", cdblock_last_error(Board));
      cdblock_destroy(Board);
      Board = NULL;
      free(CardROM);
      CardROM = NULL;
      CardROMSize = 0;
      return false;
   }

   if(!Transcript && getenv("CDB_LLE_TRANSCRIPT"))
      Transcript = fopen(getenv("CDB_LLE_TRANSCRIPT"), "w");
   TranscriptData = getenv("CDB_LLE_TRANSCRIPT_DATA") != NULL;
   if(getenv("CDB_LLE_LENIENT"))
      cdblock_set_lenient(Board, 1);

   CDBLLE_Active = true;
   Stopped = false;
   Held = false;
   SetUpDisc();
   ResetBoard();

   Log(RETRO_LOG_INFO, "CD block run by the CD-block emulator library from %s%s", ROM_NAME,
       CardROM ? ", MPEG card ROM in the card slot" : "");
   return true;
}

void CDBLLE_Kill(void)
{
   if(Transcript)
      fclose(Transcript);
   Transcript = NULL;
   if(Board)
      cdblock_destroy(Board);
   Board = NULL;
   free(CardROM);
   CardROM = NULL;
   CardROMSize = 0;
   CDBLLE_Active = false;
}

void CDBLLE_SetDisc(bool tray_open, CDIF *cdif)
{
   CDIF *next = tray_open ? NULL : cdif;

   if(Board && Disc && next && next != Disc)
      Log(RETRO_LOG_WARN, "disc change is not modelled, the drive reads the new disc as the old one's");
   Disc = next;
   if(Board)
      SetUpDisc();
}

void CDBLLE_Reset(bool powering_up)
{
   /* SYSRES is the one reset that is not a power-on. The SMPC drives CDRES low
    * for it as at power-on and releases it, which also ends a CDOFF hold. */
   (void)powering_up;
   Held = false;
   ResetBoard();
}

void CDBLLE_SetResetLine(bool asserted)
{
   if(asserted == Held)
      return;
   if(asserted)
      ResetBoard();
   Held = asserted;
   RecalcIRQ();
}

int32_t CDBLLE_Update(int32_t timestamp)
{
   Sync(timestamp);
   CatchUp();
   return timestamp + BatchSH2;
}

void CDBLLE_ResetTS(void)
{
   LastTS = 0;
}

void CDBLLE_SetClockRatio(uint32_t ratio)
{
   if(!ratio)
      return;
   SH1Ratio = ((uint64_t)ratio * SH1_HZ) / HLE_HZ;
   BatchSH2 = (int32_t)(((BATCH_CYCLES << 32) + SH1Ratio - 1) / SH1Ratio);
}

uint16_t CDBLLE_Read(uint32_t offset)
{
   uint16_t value = 0;

   if(!Board || Stopped || Held)
      return 0;
   Sync(SH7095_mem_timestamp);

   switch(offset)
   {
      case 0x0: /* data port */
         if(Owed >= AHEAD_STEP_CYCLES || !FIFOServes(false))
            CatchUp();
         WaitFIFO(false);
         if(Stopped)
            return 0;
         value = ReadData();
         RunAhead(DATA_WORD_CYCLES);
         return value;

      case 0x2: /* HIRQ */
         if(Owed >= POLL_CYCLES)
            CatchUp();
         return Stopped ? 0 : cdblock_host_flags(Board);

      case 0x3: /* HIRQ mask */
         return HIRQMask;

      case 0x6: /* CR1..CR4 */
      case 0x7:
      case 0x8:
      case 0x9:
         if(Owed >= POLL_CYCLES)
            CatchUp();
         if(Stopped)
            return 0;
         if(cdblock_host_response_word(Board, offset - 0x6, &value) < 0)
            Fail("Response read failed");
         RR[offset - 0x6] = value;
         if(offset == 0x9)
            Trace(true, "rsp %04X %04X %04X %04X", RR[0], RR[1], RR[2], RR[3]);
         return value;
   }

   return 0;
}

void CDBLLE_Write(uint32_t offset, uint16_t DB, uint16_t mask)
{
   if(!Board || Stopped || Held)
      return;
   Sync(SH7095_mem_timestamp);

   switch(offset)
   {
      case 0x0: /* data port */
         if(Owed >= AHEAD_STEP_CYCLES || !FIFOServes(true))
            CatchUp();
         WaitFIFO(true);
         if(Stopped)
            return;
         WriteData(DB);
         RunAhead(DATA_WORD_CYCLES);
         break;

      case 0x2: /* HIRQ: a 0 clears its flag */
         CatchUp();
         if(!Stopped && cdblock_host_clear_flags(Board, (uint16_t)(DB | ~mask)) < 0)
            Fail("Flag clear failed");
         RecalcIRQ();
         break;

      case 0x3: /* HIRQ mask */
         CatchUp();
         HIRQMask = (uint16_t)((HIRQMask & ~mask) | (DB & mask));
         RecalcIRQ();
         break;

      case 0x6: /* CR1..CR3, kept until CR4 */
      case 0x7:
      case 0x8:
         CR[offset - 0x6] = (uint16_t)((CR[offset - 0x6] & ~mask) | (DB & mask));
         break;

      case 0x9: /* CR4 sends the command */
         CR[3] = (uint16_t)((CR[3] & ~mask) | (DB & mask));
         if(mask != 0xFFFF)
            break;
         CatchUp();
         Trace(true, "cmd %04X %04X %04X %04X", CR[0], CR[1], CR[2], CR[3]);
         if(!Stopped && cdblock_host_command(Board, CR) < 0)
            Fail("Command failed");
         break;

      default:
         CatchUp();
         break;
   }
}
