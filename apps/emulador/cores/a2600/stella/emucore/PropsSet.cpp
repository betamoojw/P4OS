//============================================================================
//
//   SSSS    tt          lll  lll       
//  SS  SS   tt           ll   ll        
//  SS     tttttt  eeee   ll   ll   aaaa 
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2014 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//
// $Id: PropsSet.cxx 2838 2014-01-17 23:34:03Z stephena $
//============================================================================

// P4OS: the external and temporary lists (std::map) and the stella.pro
// reader and writer are gone, and the built-in database is the compact one
// tools/gen_defprops.py makes (DefPropsP4.hxx: no pointers, so no run-time
// relocations in the shared object).

#include "bspf.hxx"

#include "DefPropsP4.hxx"
#include "OSystem.hxx"
#include "Props.hxx"
#include "Settings.hxx"

#include "PropsSet.hxx"

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
PropertiesSet::PropertiesSet(OSystem* osystem)
  : myOSystem(osystem)
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
PropertiesSet::~PropertiesSet()
{
}

// P4OS: an MD5 in hex (either case) to its 16 bytes; false if it is not one.
static bool md5FromHex(const string& md5, unsigned char out[16])
{
  if(md5.length() != 32)
    return false;
  for(int i = 0; i < 32; ++i)
  {
    char c = md5[i];
    int v;
    if(c >= '0' && c <= '9')      v = c - '0';
    else if(c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if(c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return false;
    if(i & 1) out[i >> 1] |= (unsigned char)v;
    else      out[i >> 1] = (unsigned char)(v << 4);
  }
  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool PropertiesSet::getMD5(const string& md5, Properties& properties,
                           bool useDefaults) const
{
  properties.setDefaults();

  unsigned char key[16];
  if(!md5FromHex(md5, key))
    return false;

  // Binary search of the built-in database, sorted by MD5
  int low = 0, high = DEF_PROPS_SIZE - 1;
  while(low <= high)
  {
    int i = (low + high) / 2;
    int cmp = memcmp(key, DefPropsMD5[i], 16);
    if(cmp == 0)  // found it
    {
      // The record: the values from DEF_PROPS_FIRST on, separated by 0x1f
      const char* p = DefPropsData + DefPropsRec[i];
      for(int n = 0; n < DEF_PROPS_COUNT; ++n)
      {
        const char* e = p;
        while(*e && *e != '\x1f') ++e;
        if(e != p)
          properties.set((PropertyType)(DEF_PROPS_FIRST + n), string(p, e - p));
        p = *e ? e + 1 : e;
      }
      properties.set(Cartridge_MD5, md5);
      return true;
    }
    else if(cmp < 0)
      high = i - 1; // look at lower range
    else
      low = i + 1;  // look at upper range
  }
  return false;
}
