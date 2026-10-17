/* This file is part of Snes9x. See LICENSE file. */

#ifndef USE_BLARGG_APU

#include "snes9x.h"
#include "spc700.h"
#include "apu.h"
#include "soundux.h"
#include "cpuexec.h"

extern const int32_t NoiseFreq[32];

/* P4OS: see S9xSetAPUDSP(). */
static uint8_t KeyOn;
static uint8_t KeyOnPrev;

/* P4OS: the split sound, see S9xSetAPUDSP(). */
#define DSP_QUEUE_MAX 16384     /* more than the SPC700 can store in a frame */
static bool     dsp_deferred;
static uint8_t  dsp_view [0x80];
static uint8_t  dsp_view_envx [8];
static uint8_t  dsp_view_outx [8];
static uint16_t *dsp_queue;
static int32_t  dsp_queue_n;
static uint32_t dsp_queue_lost;

bool S9xInitAPU()
{
   IAPU.RAM = (uint8_t*) malloc(0x10000);

   if (!IAPU.RAM)
   {
      S9xDeinitAPU();
      return false;
   }

   return true;
}

void S9xDeinitAPU()
{
   if (IAPU.RAM)
   {
      free(IAPU.RAM);
      IAPU.RAM = NULL;
   }
}

void S9xResetAPU()
{
   int32_t i, j;
   Settings.APUEnabled = true;
   KeyOn = KeyOnPrev = 0; /* P4OS */
   dsp_deferred = false;  /* P4OS */
   dsp_queue_n = 0;
   memset(IAPU.RAM, 0, 0x100);
   memset(IAPU.RAM + 0x20, 0xFF, 0x20);
   memset(IAPU.RAM + 0x60, 0xFF, 0x20);
   memset(IAPU.RAM + 0xA0, 0xFF, 0x20);
   memset(IAPU.RAM + 0xE0, 0xFF, 0x20);

   for (i = 1; i < 256; i++)
      memcpy(IAPU.RAM + (i << 8), IAPU.RAM, 0x100);

   memset(APU.OutPorts, 0, sizeof(APU.OutPorts));
   IAPU.DirectPage = IAPU.RAM;
   /* memmove converted: Different mallocs [Neb]
    * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
   memcpy(&IAPU.RAM [0xffc0], APUROM, sizeof(APUROM));
   /* memmove converted: Different mallocs [Neb]
    * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
   memcpy(APU.ExtraRAM, APUROM, sizeof(APUROM));
   IAPU.PC = IAPU.RAM + IAPU.RAM [0xfffe] + (IAPU.RAM [0xffff] << 8);
   APU.Cycles = 0;
   IAPU.Registers.YA.W = 0;
   IAPU.Registers.X = 0;
   IAPU.Registers.S = 0xef;
   IAPU.Registers.P = 0x02;
   S9xAPUUnpackStatus();
   IAPU.Registers.PC = 0;
   IAPU.APUExecuting = Settings.APUEnabled;
   IAPU.WaitAddress1 = NULL;
   IAPU.WaitAddress2 = NULL;
   IAPU.WaitCounter = 1;
   APU.ShowROM = true;
   IAPU.RAM [0xf1] = 0x80;

   for (i = 0; i < 3; i++)
   {
      APU.TimerEnabled [i] = false;
      APU.TimerTarget [i] = 0;
      APU.Timer [i] = 0;
   }
   for (j = 0; j < 0x80; j++)
      APU.DSP [j] = 0;

   IAPU.TwoCycles = IAPU.OneCycle * 2;

   for (i = 0; i < 256; i++)
      S9xAPUCycles [i] = S9xAPUCycleLengths [i] * IAPU.OneCycle;

   APU.DSP [APU_ENDX] = 0;
   APU.DSP [APU_KOFF] = 0;
   APU.DSP [APU_KON] = 0;
   APU.DSP[APU_FLG] = APU_SOFT_RESET | APU_MUTE;
   APU.KeyedChannels = 0;

   S9xResetSound(true);
   S9xSetEchoEnable(0);
   S9xMixRAMSync(true); /* P4OS: the RAM was just filled */
}

/* P4OS: what rand() gave below (libc's rand is not in the firmware's
 * table): a small generator of its own, only used with the APU stopped. */
static uint32_t port_noise = 1;
static int32_t S9xPortNoise(void)
{
   port_noise = port_noise * 1103515245u + 12345u;
   return (int32_t) ((port_noise >> 16) & 0x7fff);
}

uint8_t S9xAPUReadPort(int32_t Address)
{
   IAPU.APUExecuting = Settings.APUEnabled;
   IAPU.WaitCounter++;

   if (Settings.APUEnabled)
      return APU.OutPorts [Address & 3];

   CPU.BranchSkip = true;

   if ((Address & 3) < 2)
   {
      int32_t r = S9xPortNoise(); /* P4OS */
      if (r & 2)
      {
         if (r & 4)
            return (Address & 3) == 1 ? 0xaa : 0xbb;
         else
            return (r >> 3) & 0xff;
      }
   }
   else
   {
      int32_t r = S9xPortNoise(); /* P4OS */
      if (r & 2)
         return (r >> 3) & 0xff;
   }
   return Memory.FillRAM[Address];
}

void S9xAPUWritePort(int32_t Address, uint8_t Byte)
{
   Memory.FillRAM [Address] = Byte;
   IAPU.RAM [(Address & 3) + 0xf4] = Byte;
   IAPU.APUExecuting = Settings.APUEnabled;
   IAPU.WaitCounter++;
}

/* P4OS: the RAM the mixer reads its samples from. It is the sound CPU's
 * own RAM unless the sound is split (see S9xDSPBeginDeferred()): then it
 * is a copy, brought up to date between frames by S9xMixRAMSync() from a
 * map of the 32-byte pieces written since (S9xAPUMarkDirty(), called by
 * every store of the sound CPU above the first two pages; those two, its
 * zero page, I/O and stack, are copied whole each time). */
uint8_t *S9xMixRAM;
uint32_t S9xAPUDirty [64];
static uint8_t *mix_ram_copy;

bool S9xMixRAMSplit(bool on)
{
   if (on && !mix_ram_copy)
      mix_ram_copy = (uint8_t *) malloc(0x10000 + 16);
   if (!on)
   {
      free(mix_ram_copy);
      mix_ram_copy = NULL;
   }
   S9xMixRAM = mix_ram_copy ? mix_ram_copy : IAPU.RAM;
   S9xMixRAMSync(true);
   return !on || mix_ram_copy != NULL;
}

void S9xMixRAMSync(bool all)
{
   int32_t w;
   if (!mix_ram_copy || !IAPU.RAM)
   {
      S9xMixRAM = IAPU.RAM;
      memset(S9xAPUDirty, 0, sizeof(S9xAPUDirty));
      return;
   }
   if (all)
   {
      memcpy(mix_ram_copy, IAPU.RAM, 0x10000);
      memset(mix_ram_copy + 0x10000, 0, 16);
      memset(S9xAPUDirty, 0, sizeof(S9xAPUDirty));
      return;
   }
   memcpy(mix_ram_copy, IAPU.RAM, 0x200);
   for (w = 0; w < 64; w++)
   {
      uint32_t bits = S9xAPUDirty [w];
      if (!bits)
         continue;
      S9xAPUDirty [w] = 0;
      for (int32_t b = 0; bits; b++, bits >>= 1)
      {
         if (bits & 1)
         {
            uint32_t at = ((uint32_t) w << 10) | ((uint32_t) b << 5);
            memcpy(mix_ram_copy + at, IAPU.RAM + at, 32);
         }
      }
   }
}

/* P4OS: KeyOn and KeyOnPrev were statics inside S9xSetAPUDSP(): at file
 * scope, so S9xResetAPU() clears them for the next game and the save
 * states carry them (S9xAPUGetKeyOnState()). */
void S9xAPUGetKeyOnState(uint8_t *both)
{
   both[0] = KeyOn;
   both[1] = KeyOnPrev;
}

void S9xAPUSetKeyOnState(const uint8_t *both)
{
   KeyOn = both[0];
   KeyOnPrev = both[1];
}

/* P4OS: the split sound. While the mixer works on the last frame on the
 * other core (S9xDSPBeginDeferred() .. S9xDSPEndDeferred(), see apu.h),
 * the sound CPU's writes to the DSP are queued instead of reaching the
 * mixer's state, and its reads are answered from a picture of the DSP
 * taken when the frame began, kept up to date with its own writes. At the
 * end the writes are made, in order, as they would have been. */
/* (the state is at the top of the file) */

static void S9xSetAPUDSPReg(uint8_t reg, uint8_t byte);

static uint8_t S9xDSPReadOUTX(int32_t c)
{
   if (SoundData.channels [c].state == SOUND_SILENT)
      return 0;
   return (SoundData.channels [c].sample >> 8) | (SoundData.channels [c].sample & 0xff);
}

static uint8_t S9xDSPReadENVX(int32_t c)
{
   int32_t eVal = SoundData.channels [c].envx;
   return (eVal > 0x7F) ? 0x7F : (eVal < 0 ? 0 : eVal);
}

bool S9xDSPInitDeferred(void)
{
   if (!dsp_queue)
      dsp_queue = (uint16_t *) malloc(DSP_QUEUE_MAX * sizeof(uint16_t));
   dsp_queue_n = 0;
   dsp_queue_lost = 0;
   dsp_deferred = false;
   return dsp_queue != NULL;
}

void S9xDSPDeinitDeferred(void)
{
   free(dsp_queue);
   dsp_queue = NULL;
   dsp_queue_n = 0;
   dsp_deferred = false;
}

void S9xDSPBeginDeferred(void)
{
   int32_t c;
   memcpy(dsp_view, APU.DSP, sizeof(dsp_view));
   for (c = 0; c < 8; c++)
   {
      dsp_view_envx [c] = S9xDSPReadENVX(c);
      dsp_view_outx [c] = S9xDSPReadOUTX(c);
   }
   dsp_queue_n = 0;
   dsp_deferred = dsp_queue != NULL;
}

void S9xDSPEndDeferred(void)
{
   int32_t i;
   uint8_t f2 = IAPU.RAM [0xf2];
   dsp_deferred = false;
   for (i = 0; i < dsp_queue_n; i++)
      S9xSetAPUDSPReg((uint8_t) (dsp_queue [i] >> 8), (uint8_t) dsp_queue [i]);
   dsp_queue_n = 0;
   IAPU.RAM [0xf2] = f2;
}

uint32_t S9xDSPLostWrites(void)
{
   return dsp_queue_lost;
}

/* What the sound CPU sees of its own write while it is queued: the plain
 * store, and the side effects on ENDX, KON, KOFF and FLG it can read back. */
static void S9xDSPViewWrite(uint8_t reg, uint8_t byte)
{
   int32_t c;
   if (reg >= 0x80)
      return;
   switch (reg)
   {
   case APU_FLG:
      if (byte & APU_SOFT_RESET)
      {
         dsp_view [reg] = APU_MUTE | APU_ECHO_DISABLED | (byte & 0x1f);
         dsp_view [APU_ENDX] = 0;
         dsp_view [APU_KOFF] = 0;
         dsp_view [APU_KON] = 0;
         return;
      }
      break;
   case APU_ENDX:
      byte = 0;
      break;
   case APU_KON:
      for (c = 0; c < 8; c++)
         if ((byte & (1 << c)) && !(dsp_view [APU_KOFF] & (1 << c)))
            dsp_view [APU_ENDX] &= ~(1 << c);
      return;
   default:
      break;
   }
   dsp_view [reg] = byte;
}

void S9xSetAPUDSP(uint8_t byte)
{
   uint8_t reg = IAPU.RAM [0xf2];
   if (dsp_deferred)
   {
      if (dsp_queue_n < DSP_QUEUE_MAX)
         dsp_queue [dsp_queue_n++] = (uint16_t) ((reg << 8) | byte);
      else
         dsp_queue_lost++;
      S9xDSPViewWrite(reg, byte);
      return;
   }
   S9xSetAPUDSPReg(reg, byte);
}

static void S9xSetAPUDSPReg(uint8_t reg, uint8_t byte)
{
   int32_t i;

   switch (reg)
   {
   case APU_FLG:
      if (byte & APU_SOFT_RESET)
      {
         APU.DSP [reg] = APU_MUTE | APU_ECHO_DISABLED | (byte & 0x1f);
         APU.DSP [APU_ENDX] = 0;
         APU.DSP [APU_KOFF] = 0;
         APU.DSP [APU_KON] = 0;
         S9xSetEchoWriteEnable(false);

         /* Kill sound */
         S9xResetSound(false);
      }
      else
      {
         S9xSetEchoWriteEnable(!(byte & APU_ECHO_DISABLED));
         so.mute_sound = !!(byte & APU_MUTE);
         SoundData.noise_hertz = NoiseFreq [byte & 0x1f];
         for (i = 0; i < 8; i++)
            if (SoundData.channels [i].type == SOUND_NOISE)
               S9xSetSoundFrequency(i, SoundData.noise_hertz);
      }
      break;
   case APU_NON:
      if (byte != APU.DSP [APU_NON])
      {
         int32_t c;
         uint8_t mask = 1;
         for (c = 0; c < 8; c++, mask <<= 1)
         {
            int32_t type;

            if (byte & mask)
               type = SOUND_NOISE;
            else
               type = SOUND_SAMPLE;

            S9xSetSoundType(c, type);
         }
      }
      break;
   case APU_MVOL_LEFT:
      if (byte != APU.DSP [APU_MVOL_LEFT])
         S9xSetMasterVolume((int8_t) byte, (int8_t) APU.DSP [APU_MVOL_RIGHT]);
      break;
   case APU_MVOL_RIGHT:
      if (byte != APU.DSP [APU_MVOL_RIGHT])
         S9xSetMasterVolume((int8_t) APU.DSP [APU_MVOL_LEFT], (int8_t) byte);
      break;
   case APU_EVOL_LEFT:
      if (byte != APU.DSP [APU_EVOL_LEFT])
         S9xSetEchoVolume((int8_t) byte, (int8_t) APU.DSP [APU_EVOL_RIGHT]);
      break;
   case APU_EVOL_RIGHT:
      if (byte != APU.DSP [APU_EVOL_RIGHT])
         S9xSetEchoVolume((int8_t) APU.DSP [APU_EVOL_LEFT], (int8_t) byte);
      break;
   case APU_ENDX:
      byte = 0;
      break;
   case APU_KOFF:
   {
      int32_t c;
      uint8_t mask = 1;
      for (c = 0; c < 8; c++, mask <<= 1)
      {
         if ((byte & mask) != 0)
         {
            if (APU.KeyedChannels & mask)
            {
               KeyOnPrev &= ~mask;
               APU.KeyedChannels &= ~mask;
               APU.DSP [APU_KON] &= ~mask;
               S9xSetSoundKeyOff(c);
            }
         }
         else if ((KeyOnPrev & mask) != 0)
         {
            KeyOnPrev &= ~mask;
            APU.KeyedChannels |= mask;
            APU.DSP [APU_KOFF] &= ~mask;
            APU.DSP [APU_ENDX] &= ~mask;
            S9xPlaySample(c);
         }
      }

      APU.DSP [APU_KOFF] = byte;
      return;
   }
   case APU_KON:
      if (byte)
      {
         int32_t c;
         uint8_t mask = 1;
         for (c = 0; c < 8; c++, mask <<= 1)
         {
            if ((byte & mask) != 0)
            {
               /* Pac-In-Time requires that channels can be key-on
                * regardeless of their current state. */
               if ((APU.DSP [APU_KOFF] & mask) == 0)
               {
                  KeyOnPrev &= ~mask;
                  APU.KeyedChannels |= mask;
                  APU.DSP [APU_ENDX] &= ~mask;
                  S9xPlaySample(c);
               }
               else
                  KeyOn |= mask;
            }
         }
      }
      return;
   case APU_VOL_LEFT + 0x00:
   case APU_VOL_LEFT + 0x10:
   case APU_VOL_LEFT + 0x20:
   case APU_VOL_LEFT + 0x30:
   case APU_VOL_LEFT + 0x40:
   case APU_VOL_LEFT + 0x50:
   case APU_VOL_LEFT + 0x60:
   case APU_VOL_LEFT + 0x70:
      S9xSetSoundVolume(reg >> 4, (int8_t) byte, (int8_t) APU.DSP [reg + 1]);
      break;
   case APU_VOL_RIGHT + 0x00:
   case APU_VOL_RIGHT + 0x10:
   case APU_VOL_RIGHT + 0x20:
   case APU_VOL_RIGHT + 0x30:
   case APU_VOL_RIGHT + 0x40:
   case APU_VOL_RIGHT + 0x50:
   case APU_VOL_RIGHT + 0x60:
   case APU_VOL_RIGHT + 0x70:
      S9xSetSoundVolume(reg >> 4, (int8_t) APU.DSP [reg - 1], (int8_t) byte);
      break;
   case APU_P_LOW + 0x00:
   case APU_P_LOW + 0x10:
   case APU_P_LOW + 0x20:
   case APU_P_LOW + 0x30:
   case APU_P_LOW + 0x40:
   case APU_P_LOW + 0x50:
   case APU_P_LOW + 0x60:
   case APU_P_LOW + 0x70:
      S9xSetSoundHertz(reg >> 4, (((int16_t) byte + ((int16_t) APU.DSP [reg + 1] << 8)) & FREQUENCY_MASK) * 8);
      break;
   case APU_P_HIGH + 0x00:
   case APU_P_HIGH + 0x10:
   case APU_P_HIGH + 0x20:
   case APU_P_HIGH + 0x30:
   case APU_P_HIGH + 0x40:
   case APU_P_HIGH + 0x50:
   case APU_P_HIGH + 0x60:
   case APU_P_HIGH + 0x70:
      S9xSetSoundHertz(reg >> 4, ((((int16_t) byte << 8) + (int16_t) APU.DSP [reg - 1]) & FREQUENCY_MASK) * 8);
      break;
   case APU_ADSR1 + 0x00:
   case APU_ADSR1 + 0x10:
   case APU_ADSR1 + 0x20:
   case APU_ADSR1 + 0x30:
   case APU_ADSR1 + 0x40:
   case APU_ADSR1 + 0x50:
   case APU_ADSR1 + 0x60:
   case APU_ADSR1 + 0x70:
      if(byte != APU.DSP [reg])
         S9xFixEnvelope(reg >> 4, APU.DSP [reg + 2], byte, APU.DSP [reg + 1]);
      break;
   case APU_ADSR2 + 0x00:
   case APU_ADSR2 + 0x10:
   case APU_ADSR2 + 0x20:
   case APU_ADSR2 + 0x30:
   case APU_ADSR2 + 0x40:
   case APU_ADSR2 + 0x50:
   case APU_ADSR2 + 0x60:
   case APU_ADSR2 + 0x70:
      if(byte != APU.DSP [reg])
         S9xFixEnvelope(reg >> 4, APU.DSP [reg + 1], APU.DSP [reg - 1], byte);
      break;
   case APU_GAIN + 0x00:
   case APU_GAIN + 0x10:
   case APU_GAIN + 0x20:
   case APU_GAIN + 0x30:
   case APU_GAIN + 0x40:
   case APU_GAIN + 0x50:
   case APU_GAIN + 0x60:
   case APU_GAIN + 0x70:
      if(byte != APU.DSP [reg])
         S9xFixEnvelope(reg >> 4, byte, APU.DSP [reg - 2], APU.DSP [reg - 1]);
      break;
   case APU_PMON:
      if(byte != APU.DSP [APU_PMON])
         S9xSetFrequencyModulationEnable(byte);
      break;
   case APU_EON:
      if(byte != APU.DSP [APU_EON])
         S9xSetEchoEnable(byte);
      break;
   case APU_EFB:
      S9xSetEchoFeedback((int8_t) byte);
      break;
   case APU_EDL:
      S9xSetEchoDelay(byte & 0xf);
      break;
   case APU_C0:
   case APU_C1:
   case APU_C2:
   case APU_C3:
   case APU_C4:
   case APU_C5:
   case APU_C6:
   case APU_C7:
      S9xSetFilterCoefficient(reg >> 4, (int8_t) byte);
      break;
   default:
      break;
   }

   KeyOnPrev |= KeyOn;
   KeyOn = 0;

   if (reg < 0x80)
      APU.DSP [reg] = byte;
}

void S9xFixEnvelope(int32_t channel, uint8_t gain, uint8_t adsr1, uint8_t adsr2)
{
   if (adsr1 & 0x80) /* ADSR mode */
   {
      /* XXX: can DSP be switched to ADSR mode directly from GAIN/INCREASE/
       * DECREASE mode? And if so, what stage of the sequence does it start
       * at? */
      if(S9xSetSoundMode(channel, MODE_ADSR))
         S9xSetSoundADSR(channel, adsr1 & 0xf, (adsr1 >> 4) & 7, adsr2 & 0x1f, (adsr2 >> 5) & 7, 8);
   } /* Gain mode */
   else if (!(gain & 0x80))
   {
      if (S9xSetSoundMode(channel, MODE_GAIN))
      {
         S9xSetEnvelopeRate(channel, 0, 0, gain & 0x7f, 0);
         S9xSetEnvelopeHeight(channel, gain & 0x7f);
      }
   }
   else if (gain & 0x40)
   {
      /* Increase mode */
      if(S9xSetSoundMode(channel, (gain & 0x20) ? MODE_INCREASE_BENT_LINE : MODE_INCREASE_LINEAR))
         S9xSetEnvelopeRate(channel, gain, 1, 127, (3 << 28) | gain);
   }
   else if (gain & 0x20)
   {
      if(S9xSetSoundMode(channel, MODE_DECREASE_EXPONENTIAL))
         S9xSetEnvelopeRate(channel, gain, -1, 0, (4 << 28) | gain);
   }
   else
   {
      if (S9xSetSoundMode(channel, MODE_DECREASE_LINEAR))
         S9xSetEnvelopeRate(channel, gain, -1, 0, (3 << 28) | gain);
   }
}

void S9xSetAPUControl(uint8_t byte)
{
   if ((byte & 1) && !APU.TimerEnabled [0])
   {
      APU.Timer [0] = 0;
      IAPU.RAM [0xfd] = 0;
      if ((APU.TimerTarget [0] = IAPU.RAM [0xfa]) == 0)
         APU.TimerTarget [0] = 0x100;
   }
   if ((byte & 2) && !APU.TimerEnabled [1])
   {
      APU.Timer [1] = 0;
      IAPU.RAM [0xfe] = 0;
      if ((APU.TimerTarget [1] = IAPU.RAM [0xfb]) == 0)
         APU.TimerTarget [1] = 0x100;
   }
   if ((byte & 4) && !APU.TimerEnabled [2])
   {
      APU.Timer [2] = 0;
      IAPU.RAM [0xff] = 0;
      if ((APU.TimerTarget [2] = IAPU.RAM [0xfc]) == 0)
         APU.TimerTarget [2] = 0x100;
   }
   APU.TimerEnabled [0] = !!(byte & 1);
   APU.TimerEnabled [1] = !!(byte & 2);
   APU.TimerEnabled [2] = !!(byte & 4);

   if (byte & 0x10)
      IAPU.RAM [0xF4] = IAPU.RAM [0xF5] = 0;

   if (byte & 0x20)
      IAPU.RAM [0xF6] = IAPU.RAM [0xF7] = 0;

   if (byte & 0x80)
   {
      if (!APU.ShowROM)
      {
         /* memmove converted: Different mallocs [Neb]
          * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
         memcpy(&IAPU.RAM [0xffc0], APUROM, sizeof(APUROM));
         S9xAPUMarkDirty(0xffc0); /* P4OS */
         S9xAPUMarkDirty(0xffe0);
         APU.ShowROM = true;
      }
   }
   else if (APU.ShowROM)
   {
      APU.ShowROM = false;
      /* memmove converted: Different mallocs [Neb]
       * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
      memcpy(&IAPU.RAM [0xffc0], APU.ExtraRAM, sizeof(APUROM));
      S9xAPUMarkDirty(0xffc0); /* P4OS */
      S9xAPUMarkDirty(0xffe0);
   }
   IAPU.RAM [0xf1] = byte;
}

uint8_t S9xGetAPUDSP()
{
   uint8_t reg = IAPU.RAM [0xf2] & 0x7f;
   uint8_t byte = APU.DSP [reg];

   if (dsp_deferred) /* P4OS: the split sound, see S9xSetAPUDSP() */
   {
      if ((reg & 0x0f) == APU_OUTX)
         return dsp_view_outx [reg >> 4];
      if ((reg & 0x0f) == APU_ENVX)
         return dsp_view_envx [reg >> 4];
      return dsp_view [reg];
   }

   switch (reg)
   {
   case APU_OUTX + 0x00:
   case APU_OUTX + 0x10:
   case APU_OUTX + 0x20:
   case APU_OUTX + 0x30:
   case APU_OUTX + 0x40:
   case APU_OUTX + 0x50:
   case APU_OUTX + 0x60:
   case APU_OUTX + 0x70:
      if(SoundData.channels [reg >> 4].state == SOUND_SILENT)
         return 0;
      return (SoundData.channels [reg >> 4].sample >> 8) | (SoundData.channels [reg >> 4].sample & 0xff);
   case APU_ENVX + 0x00:
   case APU_ENVX + 0x10:
   case APU_ENVX + 0x20:
   case APU_ENVX + 0x30:
   case APU_ENVX + 0x40:
   case APU_ENVX + 0x50:
   case APU_ENVX + 0x60:
   case APU_ENVX + 0x70:
   {
      int32_t eVal = SoundData.channels [reg >> 4].envx;
      return (eVal > 0x7F) ? 0x7F : (eVal < 0 ? 0 : eVal);
   }
   default:
      break;
   }
   return byte;
}

#endif
