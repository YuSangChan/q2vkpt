/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

/*
========================================================================

.SP2 sprite file format

========================================================================
*/

#define SP2_IDENT       (('2'<<24)+('S'<<16)+('D'<<8)+'I')
#define SP2_VERSION     2

#define SP2_MAX_FRAMES      32
#define SP2_MAX_FRAMENAME   64

typedef struct {
    uint32_t    width, height;
    uint32_t    origin_x, origin_y;         // raster coordinates inside pic
    char        name[SP2_MAX_FRAMENAME];    // name of pcx file
} dsp2frame_t;

typedef struct {
    uint32_t    ident;
    uint32_t    version;
    uint32_t    numframes;
    // dsp2frame_t frames[1];              // variable sized
} dsp2header_t;

