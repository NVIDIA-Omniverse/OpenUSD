//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Sobol direction numbers are derived from:
//   S. Joe and F. Y. Kuo, "Constructing Sobol sequences with better
//   two-dimensional projections", SIAM J. Sci. Comput. 30, 2635-2654 (2008).
// FastOwen scrambling is based on pbrt-v4 (Apache 2.0 license).
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_SAMPLING_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_SAMPLING_H

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/token.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cfloat>
#include <type_traits>

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
#include <oqmc/oqmc.h>
#include <variant>
#endif

PXR_NAMESPACE_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Bit utilities
// ---------------------------------------------------------------------------

inline uint32_t
HdEmbree_ReverseBits32(uint32_t v)
{
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
    v = ((v >> 8) & 0x00ff00ffu) | ((v & 0x00ff00ffu) << 8);
    v = (v >> 16) | (v << 16);
    return v;
}

inline uint32_t
HdEmbree_MixBits(uint32_t v)
{
    v ^= v >> 17;
    v *= 0xbf58476du;
    v ^= v >> 13;
    v *= 0x94d049bbu;
    v ^= v >> 16;
    return v;
}

// ---------------------------------------------------------------------------
// FastOwen scrambler (from pbrt-v4)
// ---------------------------------------------------------------------------

inline uint32_t
HdEmbree_FastOwenScramble(uint32_t v, uint32_t seed)
{
    v = HdEmbree_ReverseBits32(v);
    v ^= v * 0x3d20adeau;
    v += seed;
    v *= (seed >> 16) | 1;
    v ^= v * 0x05526c56u;
    v ^= v * 0x53a22864u;
    return HdEmbree_ReverseBits32(v);
}

// ---------------------------------------------------------------------------
// Sobol matrices (32 dimensions x 32 entries)
// ---------------------------------------------------------------------------

static constexpr int kSobolNumDimensions = 32;
static constexpr int kSobolMatrixSize = 32;

// Direction numbers from Joe-Kuo (new-joe-kuo-6.21201), extracted from pbrt-v4.
static constexpr uint32_t kSobolMatrices[kSobolNumDimensions * kSobolMatrixSize] = {
    // Dimension 0
    0x80000000, 0x40000000, 0x20000000, 0x10000000, 0x08000000, 0x04000000,
    0x02000000, 0x01000000, 0x00800000, 0x00400000, 0x00200000, 0x00100000,
    0x00080000, 0x00040000, 0x00020000, 0x00010000, 0x00008000, 0x00004000,
    0x00002000, 0x00001000, 0x00000800, 0x00000400, 0x00000200, 0x00000100,
    0x00000080, 0x00000040, 0x00000020, 0x00000010, 0x00000008, 0x00000004,
    0x00000002, 0x00000001,
    // Dimension 1
    0x80000000, 0xc0000000, 0xa0000000, 0xf0000000, 0x88000000, 0xcc000000,
    0xaa000000, 0xff000000, 0x80800000, 0xc0c00000, 0xa0a00000, 0xf0f00000,
    0x88880000, 0xcccc0000, 0xaaaa0000, 0xffff0000, 0x80008000, 0xc000c000,
    0xa000a000, 0xf000f000, 0x88008800, 0xcc00cc00, 0xaa00aa00, 0xff00ff00,
    0x80808080, 0xc0c0c0c0, 0xa0a0a0a0, 0xf0f0f0f0, 0x88888888, 0xcccccccc,
    0xaaaaaaaa, 0xffffffff,
    // Dimension 2
    0x80000000, 0xc0000000, 0x60000000, 0x90000000, 0xe8000000, 0x5c000000,
    0x8e000000, 0xc5000000, 0x68800000, 0x9cc00000, 0xee600000, 0x55900000,
    0x80680000, 0xc09c0000, 0x60ee0000, 0x90550000, 0xe8808000, 0x5cc0c000,
    0x8e606000, 0xc5909000, 0x6868e800, 0x9c9c5c00, 0xeeee8e00, 0x5555c500,
    0x8000e880, 0xc0005cc0, 0x60008e60, 0x9000c590, 0xe8006868, 0x5c009c9c,
    0x8e00eeee, 0xc5005555,
    // Dimension 3
    0x80000000, 0xc0000000, 0x20000000, 0x50000000, 0xf8000000, 0x74000000,
    0xa2000000, 0x93000000, 0xd8800000, 0x25400000, 0x59e00000, 0xe6d00000,
    0x78080000, 0xb40c0000, 0x82020000, 0xc3050000, 0x208f8000, 0x51474000,
    0xfbea2000, 0x75d93000, 0xa0858800, 0x914e5400, 0xdbe79e00, 0x25db6d00,
    0x58800080, 0xe54000c0, 0x79e00020, 0xb6d00050, 0x800800f8, 0xc00c0074,
    0x200200a2, 0x50050093,
    // Dimension 4
    0x80000000, 0x40000000, 0x20000000, 0xb0000000, 0xf8000000, 0xdc000000,
    0x7a000000, 0x9d000000, 0x5a800000, 0x2fc00000, 0xa1600000, 0xf0b00000,
    0xda880000, 0x6fc40000, 0x81620000, 0x40bb0000, 0x22878000, 0xb3c9c000,
    0xfb65a000, 0xddb2d000, 0x78022800, 0x9c0b3c00, 0x5a0fb600, 0x2d0ddb00,
    0xa2878080, 0xf3c9c040, 0xdb65a020, 0x6db2d0b0, 0x800228f8, 0x400b3cdc,
    0x200fb67a, 0xb00ddb9d,
    // Dimension 5
    0x80000000, 0x40000000, 0x60000000, 0x30000000, 0xc8000000, 0x24000000,
    0x56000000, 0xfb000000, 0xe0800000, 0x70400000, 0xa8600000, 0x14300000,
    0x9ec80000, 0xdf240000, 0xb6d60000, 0x8bbb0000, 0x48008000, 0x64004000,
    0x36006000, 0xcb003000, 0x2880c800, 0x54402400, 0xfe605600, 0xef30fb00,
    0x7e48e080, 0xaf647040, 0x1eb6a860, 0x9f8b1430, 0xd6c81ec8, 0xbb249f24,
    0x80d6d6d6, 0x40bbbbbb,
    // Dimension 6
    0x80000000, 0xc0000000, 0xa0000000, 0xd0000000, 0x58000000, 0x94000000,
    0x3e000000, 0xe3000000, 0xbe800000, 0x23c00000, 0x1e200000, 0xf3100000,
    0x46780000, 0x67840000, 0x78460000, 0x84670000, 0xc6788000, 0xa784c000,
    0xd846a000, 0x5467d000, 0x9e78d800, 0x33845400, 0xe6469e00, 0xb7673300,
    0x20f86680, 0x104477c0, 0xf8668020, 0x4477c010, 0x668020f8, 0x77c01044,
    0x8020f866, 0xc0104477,
    // Dimension 7
    0x80000000, 0x40000000, 0xa0000000, 0x50000000, 0x88000000, 0x24000000,
    0x12000000, 0x2d000000, 0x76800000, 0x9e400000, 0x08200000, 0x64100000,
    0xb2280000, 0x7d140000, 0xfea20000, 0xba490000, 0x1a248000, 0x491b4000,
    0xc4b5a000, 0xe3739000, 0xf6800800, 0xde400400, 0xa8200a00, 0x34100500,
    0x3a280880, 0x59140240, 0xeca20120, 0x974902d0, 0x6ca48768, 0xd75b49e4,
    0xcc95a082, 0x87639641,
    // Dimension 8
    0x80000000, 0x40000000, 0xa0000000, 0x50000000, 0x28000000, 0xd4000000,
    0x6a000000, 0x71000000, 0x38800000, 0x58400000, 0xea200000, 0x31100000,
    0x98a80000, 0x08540000, 0xc22a0000, 0xe5250000, 0xf2b28000, 0x79484000,
    0xfaa42000, 0xbd731000, 0x18a80800, 0x48540400, 0x622a0a00, 0xb5250500,
    0xdab28280, 0xad484d40, 0x90a426a0, 0xcc731710, 0x20280b88, 0x10140184,
    0x880a04a2, 0x84350611,
    // Dimension 9
    0x80000000, 0x40000000, 0xe0000000, 0xb0000000, 0x98000000, 0x94000000,
    0x8a000000, 0x5b000000, 0x33800000, 0xd9c00000, 0x72200000, 0x3f100000,
    0xc1b80000, 0xa6ec0000, 0x53860000, 0x29f50000, 0x0a3a8000, 0x1b2ac000,
    0xd392e000, 0x69ff7000, 0xea380800, 0xab2c0400, 0x4ba60e00, 0xfde50b00,
    0x60028980, 0xf006c940, 0x7834e8a0, 0x241a75b0, 0x123a8b38, 0xcf2ac99c,
    0xb992e922, 0x82ff78f1,
    // Dimension 10
    0x80000000, 0x40000000, 0xa0000000, 0x10000000, 0x08000000, 0x6c000000,
    0x9e000000, 0x23000000, 0x57800000, 0xadc00000, 0x7fa00000, 0x91d00000,
    0x49880000, 0xced40000, 0x880a0000, 0x2c0f0000, 0x3e0d8000, 0x3317c000,
    0x5fb06000, 0xc1f8b000, 0xe18d8800, 0xb2d7c400, 0x1e106a00, 0x6328b100,
    0xf7858880, 0xbdc3c2c0, 0x77ba63e0, 0xfdf7b330, 0xd7800df8, 0xedc0081c,
    0xdfa0041a, 0x81d00a2d,
    // Dimension 11
    0x80000000, 0x40000000, 0x20000000, 0x30000000, 0x58000000, 0xac000000,
    0x96000000, 0x2b000000, 0xd4800000, 0x09400000, 0xe2a00000, 0x52500000,
    0x4e280000, 0xc71c0000, 0x629e0000, 0x12670000, 0x6e138000, 0xf731c000,
    0x3a98a000, 0xbe449000, 0xf83b8800, 0xdc2dc400, 0xee06a200, 0xb7239300,
    0x1aa80d80, 0x8e5c0ec0, 0xa03e0b60, 0x703701b0, 0x783b88c8, 0x9c2dca54,
    0xce06a74a, 0x87239795,
    // Dimension 12
    0x80000000, 0xc0000000, 0xa0000000, 0x50000000, 0xf8000000, 0x8c000000,
    0xe2000000, 0x33000000, 0x0f800000, 0x21400000, 0x95a00000, 0x5e700000,
    0xd8080000, 0x1c240000, 0xba160000, 0xef370000, 0x15868000, 0x9e6fc000,
    0x781b6000, 0x4c349000, 0x420e8800, 0x630bcc00, 0xf7ad6a00, 0xad739500,
    0x77800780, 0x6d4004c0, 0xd7a00420, 0x3d700630, 0x2f880f78, 0xb1640ad4,
    0xcdb6077a, 0x824706d7,
    // Dimension 13
    0x80000000, 0xc0000000, 0x60000000, 0x90000000, 0x38000000, 0xc4000000,
    0x42000000, 0xa3000000, 0xf1800000, 0xaa400000, 0xfce00000, 0x85100000,
    0xe0080000, 0x500c0000, 0x58060000, 0x54090000, 0x7a038000, 0x670c4000,
    0xb3842000, 0x094a3000, 0x0d6f1800, 0x2f5aa400, 0x1ce7ce00, 0xd5145100,
    0xb8000080, 0x040000c0, 0x22000060, 0x33000090, 0xc9800038, 0x6e4000c4,
    0xbee00042, 0x261000a3,
    // Dimension 14
    0x80000000, 0x40000000, 0x20000000, 0xf0000000, 0xa8000000, 0x54000000,
    0x9a000000, 0x9d000000, 0x1e800000, 0x5cc00000, 0x7d200000, 0x8d100000,
    0x24880000, 0x71c40000, 0xeba20000, 0x75df0000, 0x6ba28000, 0x35d14000,
    0x4ba3a000, 0xc5d2d000, 0xe3a16800, 0x91db8c00, 0x79aef200, 0x0cdf4100,
    0x672a8080, 0x50154040, 0x1a01a020, 0xdd0dd0f0, 0x3e83e8a8, 0xaccacc54,
    0xd52d529a, 0xd91d919d,
    // Dimension 15
    0x80000000, 0xc0000000, 0x20000000, 0xd0000000, 0xd8000000, 0xc4000000,
    0x46000000, 0x85000000, 0xa5800000, 0x76c00000, 0xada00000, 0x6ab00000,
    0x2da80000, 0xaabc0000, 0x0daa0000, 0x7ab10000, 0xd5a78000, 0xbebd4000,
    0x93a3e000, 0x3bb51000, 0x3629b800, 0x4d727c00, 0x9b836200, 0x27c4d700,
    0xb629b880, 0x8d727cc0, 0xbb836220, 0xf7c4d7d0, 0x6e29b858, 0x49727c04,
    0xfd836266, 0x72c4d755,
    // Dimension 16
    0x80000000, 0x40000000, 0x20000000, 0xf0000000, 0x38000000, 0x14000000,
    0xf6000000, 0x67000000, 0x8f800000, 0x50400000, 0x8aa00000, 0x0ff00000,
    0x12a80000, 0xabf40000, 0xfcaa0000, 0x28fb0000, 0xbd298000, 0x0bba4000,
    0x4e06e000, 0x330c3000, 0x59861800, 0xc74d3400, 0x3d2cb200, 0x4bb2cb00,
    0x6e061880, 0xc30d3440, 0x618cb220, 0xd342cbf0, 0xcb2e18b8, 0x2cb93454,
    0xe186b2d6, 0x9349cb97,
    // Dimension 17
    0x80000000, 0xc0000000, 0x20000000, 0xf0000000, 0x68000000, 0x64000000,
    0x36000000, 0x6d000000, 0x41800000, 0xe0400000, 0xd2e00000, 0x9bf00000,
    0x0ce80000, 0x52fc0000, 0x5b6a0000, 0x2fb30000, 0xa00c8000, 0x30054000,
    0x4807e000, 0x940f9000, 0x5e01f800, 0x090e9400, 0x778a5600, 0x8d416b00,
    0x9369f880, 0x7bb294c0, 0xde005620, 0xc9026bf0, 0x578d78e8, 0x7d4bd4a4,
    0xfb6db616, 0x1fbefb9d,
    // Dimension 18
    0x80000000, 0x40000000, 0xa0000000, 0x50000000, 0x98000000, 0xf4000000,
    0xae000000, 0xbb000000, 0xe7800000, 0x95c00000, 0x1c200000, 0xd0300000,
    0xdba80000, 0x55f40000, 0xff820000, 0x21c10000, 0x12238000, 0x3b3a4000,
    0xa42b6000, 0x3430f000, 0x4da69800, 0x4af3ec00, 0x2e043a00, 0xfb0a1f00,
    0x47851880, 0xc5c9ac40, 0x842f5aa0, 0x243aef50, 0x75a38018, 0xeefa40b4,
    0x180b600e, 0xb400f0eb,
    // Dimension 19
    0x80000000, 0xc0000000, 0xe0000000, 0xb0000000, 0xb8000000, 0x3c000000,
    0xce000000, 0x41000000, 0x21800000, 0x51c00000, 0x09600000, 0x85700000,
    0xf2780000, 0x8e9c0000, 0x60020000, 0x70030000, 0x58038000, 0x8c02c000,
    0x7602e000, 0x7d00f000, 0xef833800, 0x10c10400, 0x28e08600, 0xd4b14700,
    0xfb182580, 0x0bee15c0, 0x9279c9e0, 0xfe9d3a70, 0x38000008, 0xfc00000c,
    0x2e00000e, 0xf100000b,
    // Dimension 20
    0x80000000, 0xc0000000, 0xe0000000, 0xd0000000, 0x68000000, 0x3c000000,
    0x8a000000, 0x51000000, 0xa9800000, 0xddc00000, 0x5ba00000, 0x39d00000,
    0x95f80000, 0x56d40000, 0x0a020000, 0x91030000, 0x49838000, 0x0dc34000,
    0x33a1a000, 0x05d0f000, 0x1ffa2800, 0x07d54400, 0xa380a600, 0x4cc07700,
    0x1222ee80, 0x3413a740, 0xa65bf7e0, 0x5305ab50, 0x15f80008, 0x96d4000c,
    0xea02000e, 0x4103000d,
    // Dimension 21
    0x80000000, 0x40000000, 0x60000000, 0xd0000000, 0x38000000, 0x8c000000,
    0x7e000000, 0x71000000, 0xc8800000, 0x04c00000, 0x1ba00000, 0xbb700000,
    0x4a980000, 0xc3bc0000, 0xa6020000, 0x6d010000, 0xee818000, 0x29c34000,
    0x9520e000, 0x42b23000, 0xe7b9f800, 0x0d0dc400, 0x3fb92200, 0x110d1300,
    0x19bbee80, 0x3c0cadc0, 0x973a4a60, 0xc5cf7ef0, 0x3a180008, 0x0b7c0004,
    0xa3a20006, 0x7771000d,
    // Dimension 22
    0x80000000, 0xc0000000, 0xa0000000, 0x90000000, 0x08000000, 0x64000000,
    0x6a000000, 0x89000000, 0xa5800000, 0xcb400000, 0x18200000, 0xad900000,
    0xaf880000, 0x72f40000, 0x25820000, 0x0b430000, 0xb8228000, 0x3d924000,
    0xa7882000, 0x16f59000, 0x4f83a800, 0x82412400, 0x1da01600, 0xf6d16d00,
    0xbfa84080, 0xbb672640, 0xe0091620, 0xf0b4efd0, 0x38228008, 0xfd92400c,
    0x0788200a, 0x86f59009,
    // Dimension 23
    0x80000000, 0xc0000000, 0x20000000, 0xd0000000, 0x48000000, 0x8c000000,
    0xd6000000, 0x39000000, 0xd5800000, 0x32400000, 0xb2a00000, 0x72100000,
    0x53d80000, 0x82cc0000, 0xcb820000, 0x47430000, 0x91208000, 0xa9534000,
    0x7cf92000, 0x4e9e3000, 0xfcf95800, 0x8e9fe400, 0xdcf9d600, 0x5e9c8900,
    0x94f96a80, 0xd29fb840, 0x42f9b760, 0xeb9c9f30, 0x97788008, 0xd9df400c,
    0x25db2002, 0xabcd300d,
    // Dimension 24
    0x80000000, 0xc0000000, 0x20000000, 0x50000000, 0xd8000000, 0xf4000000,
    0x3e000000, 0x95000000, 0x8f800000, 0x3d400000, 0xf3200000, 0x2ef00000,
    0xadc80000, 0x0a0c0000, 0x8b220000, 0x4af30000, 0x6bc88000, 0x3b0d4000,
    0xe2a16000, 0x16b0d000, 0x29687800, 0xbdbf1400, 0x33cb5e00, 0x0f0c2500,
    0xfca1b480, 0xd3b0afc0, 0x7eeb6920, 0x74fe4d30, 0xfee87808, 0xb4ff140c,
    0xdeeb5e02, 0xe4fc2505,
    // Dimension 25
    0x80000000, 0x40000000, 0xa0000000, 0xb0000000, 0x98000000, 0xa4000000,
    0x7a000000, 0xd5000000, 0x02800000, 0x60400000, 0x51e00000, 0x88700000,
    0x8c280000, 0x47c40000, 0x0be20000, 0xad710000, 0xb6aa8000, 0x3386c000,
    0xb8006000, 0x54039000, 0x42036800, 0xc1019400, 0xe0826a00, 0x11431100,
    0x2960af80, 0x3d3175c0, 0xdf4a3aa0, 0xaff49e10, 0xd62b6808, 0x62c59404,
    0x31606a0a, 0xd932110b,
    // Dimension 26
    0x80000000, 0xc0000000, 0xa0000000, 0x30000000, 0x18000000, 0x34000000,
    0x8a000000, 0x9d000000, 0x67800000, 0x82400000, 0x40e00000, 0x60f00000,
    0x91480000, 0x29440000, 0x2d620000, 0xbfb30000, 0x162a8000, 0xfbf4c000,
    0xe4ca6000, 0xc207d000, 0x2002a800, 0xf001b400, 0xb8037e00, 0x04021900,
    0x92034b80, 0xa90327c0, 0xed81f320, 0x1f40d810, 0x27602808, 0xe2b1740c,
    0xd1ab1e0a, 0x49b6c903,
    // Dimension 27
    0x80000000, 0x40000000, 0xe0000000, 0xd0000000, 0x08000000, 0x4c000000,
    0x02000000, 0xb5000000, 0x36800000, 0xc2c00000, 0x14200000, 0x07500000,
    0x1bf80000, 0x50340000, 0x48a20000, 0xac910000, 0xd35b8000, 0xbca74000,
    0x7bfa2000, 0xc0343000, 0xa0a18800, 0x30909400, 0xd95b7a00, 0x45a57b00,
    0x4f7a7880, 0xb7f6f940, 0x82013de0, 0xf502dfd0, 0xd6820808, 0x12c3d404,
    0x1c235a0e, 0x4b504b0d,
    // Dimension 28
    0x80000000, 0xc0000000, 0xe0000000, 0x50000000, 0x68000000, 0x4c000000,
    0x76000000, 0xf7000000, 0x36800000, 0xd7400000, 0x87e00000, 0xef300000,
    0xa3a80000, 0xd5440000, 0x23aa0000, 0x15470000, 0xc3a98000, 0x45464000,
    0xaba82000, 0x09477000, 0xdda9f800, 0xfe44ac00, 0xeb292200, 0x2907f100,
    0x6ccb3d80, 0xc6344dc0, 0xcf61b320, 0x137318d0, 0xeccb3d88, 0x06344dcc,
    0x2f61b32e, 0x437318d5,
    // Dimension 29
    0x80000000, 0x40000000, 0x60000000, 0x90000000, 0xc8000000, 0x74000000,
    0x52000000, 0x03000000, 0xeb800000, 0x6f400000, 0x64600000, 0xdaf00000,
    0x17980000, 0x297c0000, 0xa59a0000, 0xfa7d0000, 0xe61b8000, 0x713f4000,
    0x1878a000, 0xdcce9000, 0xb661e800, 0x99f29c00, 0x9c184600, 0xd63e2100,
    0x09fa5780, 0x548e0ac0, 0xa380a9e0, 0x5b413f30, 0x56625788, 0x49f20ac4,
    0x341aa9e6, 0x323c3f39,
    // Dimension 30
    0x80000000, 0xc0000000, 0xa0000000, 0xd0000000, 0xb8000000, 0x04000000,
    0x6e000000, 0x97000000, 0xf2800000, 0xedc00000, 0x13600000, 0x5c900000,
    0xdb580000, 0x31e40000, 0x09da0000, 0xcc270000, 0x02b88000, 0x44b44000,
    0x0fe26000, 0xe6505000, 0x9ab9d800, 0x50b50c00, 0x79e29200, 0xa552fb00,
    0xbe38bf80, 0x2e77d940, 0xf6000ae0, 0x830112d0, 0x84803f88, 0xaec3994c,
    0x37e26aea, 0x225142dd,
    // Dimension 31
    0x80000000, 0xc0000000, 0xe0000000, 0x30000000, 0x68000000, 0xec000000,
    0x22000000, 0x2b000000, 0x36800000, 0x9d400000, 0x6a200000, 0x16700000,
    0x4de80000, 0x330c0000, 0x936a0000, 0x824f0000, 0x3b498000, 0x8f3fc000,
    0x28202000, 0xcd707000, 0xf36aa800, 0x724fdc00, 0xb34bf200, 0x533e6900,
    0x62207a80, 0x0a7140c0, 0xe7ea6520, 0xc40d90f0, 0xefe9fa88, 0xd80e80cc,
    0x45ea452e, 0x2f0de0f3,
};

// ---------------------------------------------------------------------------
// Sobol sample generation
// ---------------------------------------------------------------------------

/// Generate a single Sobol sample for the given index and dimension,
/// applying FastOwen scrambling for decorrelation.
inline float
HdEmbree_SobolSample(uint32_t sampleIndex, int dimension, uint32_t seed)
{
    // Use the original (unwrapped) dimension for the scrambling seed so that
    // dimensions 0 and 32 (etc.) do not produce identical values.
    uint32_t dimSeed = HdEmbree_MixBits(
        seed ^ (static_cast<uint32_t>(dimension) * 0x9e3779b9u));

    // Wrap dimension for the matrix table lookup.
    int wrappedDim = dimension % kSobolNumDimensions;

    // Generate unscrambled Sobol sample via matrix multiplication
    uint32_t v = 0;
    uint32_t idx = sampleIndex;
    for (int i = wrappedDim * kSobolMatrixSize; idx != 0; idx >>= 1, ++i) {
        if (idx & 1) {
            v ^= kSobolMatrices[i];
        }
    }

    // Apply FastOwen scrambling with per-dimension seed
    v = HdEmbree_FastOwenScramble(v, dimSeed);

    // Convert to [0, 1)
    return std::min(v * 0x1p-32f, 1.0f - FLT_EPSILON);
}

// ---------------------------------------------------------------------------
// Sampler sequence selection
// ---------------------------------------------------------------------------

enum class HdEmbreeSamplerSequence : uint8_t
{
    Random,
    Sobol,
    OpenQMCSobol,
    OpenQMCSobolBN,
    OpenQMCPMJ,
    OpenQMCPMJBN,
    OpenQMCLattice,
    OpenQMCLatticeBN
};

inline TfToken
HdEmbreeGetSamplerSequenceToken(HdEmbreeSamplerSequence sequence)
{
    switch (sequence) {
    case HdEmbreeSamplerSequence::Random:
        return TfToken("random");
    case HdEmbreeSamplerSequence::Sobol:
        return TfToken("sobol");
    case HdEmbreeSamplerSequence::OpenQMCSobol:
        return TfToken("openqmc_sobol");
    case HdEmbreeSamplerSequence::OpenQMCSobolBN:
        return TfToken("openqmc_sobolbn");
    case HdEmbreeSamplerSequence::OpenQMCPMJ:
        return TfToken("openqmc_pmj");
    case HdEmbreeSamplerSequence::OpenQMCPMJBN:
        return TfToken("openqmc_pmjbn");
    case HdEmbreeSamplerSequence::OpenQMCLattice:
        return TfToken("openqmc_lattice");
    case HdEmbreeSamplerSequence::OpenQMCLatticeBN:
        return TfToken("openqmc_latticebn");
    }
    return TfToken("sobol");
}

inline HdEmbreeSamplerSequence
HdEmbreeGetSamplerSequenceFromToken(TfToken const& token)
{
    if (token == TfToken("random")) {
        return HdEmbreeSamplerSequence::Random;
    }
    if (token == TfToken("openqmc_sobol")) {
        return HdEmbreeSamplerSequence::OpenQMCSobol;
    }
    if (token == TfToken("openqmc_sobolbn")) {
        return HdEmbreeSamplerSequence::OpenQMCSobolBN;
    }
    if (token == TfToken("openqmc_pmj")) {
        return HdEmbreeSamplerSequence::OpenQMCPMJ;
    }
    if (token == TfToken("openqmc_pmjbn")) {
        return HdEmbreeSamplerSequence::OpenQMCPMJBN;
    }
    if (token == TfToken("openqmc_lattice")) {
        return HdEmbreeSamplerSequence::OpenQMCLattice;
    }
    if (token == TfToken("openqmc_latticebn")) {
        return HdEmbreeSamplerSequence::OpenQMCLatticeBN;
    }
    return HdEmbreeSamplerSequence::Sobol;
}

inline bool
HdEmbreeSamplerSequenceNeedsOpenQMC(HdEmbreeSamplerSequence sequence)
{
    switch (sequence) {
    case HdEmbreeSamplerSequence::Random:
    case HdEmbreeSamplerSequence::Sobol:
        return false;
    case HdEmbreeSamplerSequence::OpenQMCSobol:
    case HdEmbreeSamplerSequence::OpenQMCSobolBN:
    case HdEmbreeSamplerSequence::OpenQMCPMJ:
    case HdEmbreeSamplerSequence::OpenQMCPMJBN:
    case HdEmbreeSamplerSequence::OpenQMCLattice:
    case HdEmbreeSamplerSequence::OpenQMCLatticeBN:
        return true;
    }
    return false;
}

inline bool
HdEmbreeSamplerSequenceIsSupported(HdEmbreeSamplerSequence sequence)
{
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    return true;
#else
    return !HdEmbreeSamplerSequenceNeedsOpenQMC(sequence);
#endif
}

inline HdEmbreeSamplerSequence
HdEmbreeGetDefaultSamplerSequence(bool useSobol)
{
    if (!useSobol) {
        return HdEmbreeSamplerSequence::Random;
    }

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    return HdEmbreeSamplerSequence::OpenQMCSobolBN;
#else
    return HdEmbreeSamplerSequence::Sobol;
#endif
}

// ---------------------------------------------------------------------------
// Domain-aware sampler API
// ---------------------------------------------------------------------------

enum class HdEmbreeSampleDomainKey : uint32_t
{
    Pixel = 0x0001u,
    CameraJitter = 0x0010u,
    AmbientOcclusion = 0x001fu,
    AmbientOcclusionSample = 0x0020u,
    AmbientOcclusionShuffle = 0x0021u,
    PathBounce = 0x0100u,
    Presence = 0x0110u,
    Wavelength = 0x0111u,
    BsdfSample = 0x0120u,
    DirectLighting = 0x012fu,
    DirectLightSelect = 0x0130u,
    DirectLightSample = 0x0131u,
    RussianRoulette = 0x0140u,
    MediumChannel = 0x0200u,
    MediumFreeFlight = 0x0201u,
    MediumDirectLighting = 0x020fu,
    MediumDirectLightSelect = 0x0210u,
    MediumDirectLightSample = 0x0211u,
    MediumRussianRoulette = 0x0220u,
    MediumPhase = 0x0230u,
    SssEntry = 0x0300u,
    SssEntryDirection = 0x0301u,
    SssBounce = 0x0310u,
    SssChannel = 0x0320u,
    SssGuideChoice = 0x0321u,
    SssBackwardChoice = 0x0322u,
    SssPhaseDirection = 0x0323u,
    SssFreeFlight = 0x0324u
};

inline uint32_t
HdEmbreeSampleDomainKeyValue(HdEmbreeSampleDomainKey key)
{
    return static_cast<uint32_t>(key);
}

inline uint32_t
HdEmbree_MixSampleDomain(uint32_t seed,
                         HdEmbreeSampleDomainKey key,
                         uint32_t a = 0u,
                         uint32_t b = 0u)
{
    uint32_t h = seed ^ 0xa511e9b3u;
    h ^= HdEmbree_MixBits(
        HdEmbreeSampleDomainKeyValue(key) + 0x9e3779b9u);
    h ^= HdEmbree_MixBits(a + 0x85ebca6bu);
    h ^= HdEmbree_MixBits(b + 0xc2b2ae35u);
    return HdEmbree_MixBits(h);
}

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
using HdEmbreeOpenQMCVariant = std::variant<
    std::monostate,
    oqmc::SobolSampler,
    oqmc::SobolBnSampler,
    oqmc::PmjSampler,
    oqmc::PmjBnSampler,
    oqmc::LatticeSampler,
    oqmc::LatticeBnSampler>;
#endif

struct HdEmbreeSampleDomain
{
    uint32_t seed = 0u;
    uint32_t sampleIndex = 0u;
    bool useRandom = false;
    HdEmbreeSamplerSequence sequence = HdEmbreeSamplerSequence::Sobol;

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    HdEmbreeOpenQMCVariant openQmcDomain;
#endif

    HdEmbreeSampleDomain() = default;

    HdEmbreeSampleDomain(uint32_t domainSeed,
                         uint32_t sampleIdx,
                         bool random,
                         HdEmbreeSamplerSequence samplerSequence)
        : seed(domainSeed)
        , sampleIndex(sampleIdx)
        , useRandom(random)
        , sequence(samplerSequence)
    {
    }

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    HdEmbreeSampleDomain(uint32_t domainSeed,
                         uint32_t sampleIdx,
                         bool random,
                         HdEmbreeSamplerSequence samplerSequence,
                         HdEmbreeOpenQMCVariant openQmcSampler)
        : seed(domainSeed)
        , sampleIndex(sampleIdx)
        , useRandom(random)
        , sequence(samplerSequence)
        , openQmcDomain(openQmcSampler)
    {
    }
#endif

    HdEmbreeSampleDomain Fork(HdEmbreeSampleDomainKey key) const
    {
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
        if (HdEmbreeSamplerSequenceNeedsOpenQMC(sequence)) {
            const int domainKey =
                static_cast<int>(HdEmbreeSampleDomainKeyValue(key));
            return std::visit(
                [this, key, domainKey](auto const& sampler)
                    -> HdEmbreeSampleDomain {
                    using SamplerT = std::decay_t<decltype(sampler)>;
                    if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                        return _LegacyChild(key);
                    } else {
                        return HdEmbreeSampleDomain(
                            HdEmbree_MixSampleDomain(seed, key),
                            sampleIndex,
                            useRandom,
                            sequence,
                            sampler.newDomain(domainKey));
                    }
                },
                openQmcDomain);
        }
#endif

        return _LegacyChild(key);
    }

    HdEmbreeSampleDomain Split(HdEmbreeSampleDomainKey key,
                               int size,
                               int index) const
    {
        const int safeSize = std::max(size, 1);
        const int safeIndex = std::max(index, 0);
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
        if (HdEmbreeSamplerSequenceNeedsOpenQMC(sequence)) {
            const int domainKey =
                static_cast<int>(HdEmbreeSampleDomainKeyValue(key));
            return std::visit(
                [this, key, domainKey, safeSize, safeIndex](auto const& sampler)
                    -> HdEmbreeSampleDomain {
                    using SamplerT = std::decay_t<decltype(sampler)>;
                    if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                        return _LegacyChild(
                            key,
                            static_cast<uint32_t>(safeSize),
                            static_cast<uint32_t>(safeIndex));
                    } else {
                        return HdEmbreeSampleDomain(
                            HdEmbree_MixSampleDomain(
                                seed,
                                key,
                                static_cast<uint32_t>(safeSize),
                                static_cast<uint32_t>(safeIndex)),
                            sampleIndex,
                            useRandom,
                            sequence,
                            sampler.newDomainSplit(
                                domainKey, safeSize, safeIndex));
                    }
                },
                openQmcDomain);
        }
#endif
        return _LegacyChild(
            key,
            static_cast<uint32_t>(safeSize),
            static_cast<uint32_t>(safeIndex));
    }

    HdEmbreeSampleDomain Distrib(HdEmbreeSampleDomainKey key,
                                 int index) const
    {
        const int safeIndex = std::max(index, 0);
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
        if (HdEmbreeSamplerSequenceNeedsOpenQMC(sequence)) {
            const int domainKey =
                static_cast<int>(HdEmbreeSampleDomainKeyValue(key));
            return std::visit(
                [this, key, domainKey, safeIndex](auto const& sampler)
                    -> HdEmbreeSampleDomain {
                    using SamplerT = std::decay_t<decltype(sampler)>;
                    if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                        return _LegacyChild(
                            key, static_cast<uint32_t>(safeIndex));
                    } else {
                        return HdEmbreeSampleDomain(
                            HdEmbree_MixSampleDomain(
                                seed,
                                key,
                                static_cast<uint32_t>(safeIndex)),
                            sampleIndex,
                            useRandom,
                            sequence,
                            sampler.newDomainDistrib(domainKey, safeIndex));
                    }
                },
                openQmcDomain);
        }
#endif
        return _LegacyChild(key, static_cast<uint32_t>(safeIndex));
    }

    HdEmbreeSampleDomain Chain(HdEmbreeSampleDomainKey key,
                               int index) const
    {
        const int safeIndex = std::max(index, 0);
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
        if (HdEmbreeSamplerSequenceNeedsOpenQMC(sequence)) {
            const int domainKey =
                static_cast<int>(HdEmbreeSampleDomainKeyValue(key));
            return std::visit(
                [this, key, domainKey, safeIndex](auto const& sampler)
                    -> HdEmbreeSampleDomain {
                    using SamplerT = std::decay_t<decltype(sampler)>;
                    if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                        return _LegacyChild(
                            key, static_cast<uint32_t>(safeIndex));
                    } else {
                        return HdEmbreeSampleDomain(
                            HdEmbree_MixSampleDomain(
                                seed,
                                key,
                                static_cast<uint32_t>(safeIndex)),
                            sampleIndex,
                            useRandom,
                            sequence,
                            sampler.newDomainChain(domainKey, safeIndex));
                    }
                },
                openQmcDomain);
        }
#endif
        return _LegacyChild(key, static_cast<uint32_t>(safeIndex));
    }

    float Draw1D() const
    {
        return _Draw<1>()[0];
    }

    GfVec2f Draw2D() const
    {
        const std::array<float, 2> sample = _Draw<2>();
        return GfVec2f(sample[0], sample[1]);
    }

    GfVec3f Draw3D() const
    {
        const std::array<float, 3> sample = _Draw<3>();
        return GfVec3f(sample[0], sample[1], sample[2]);
    }

    GfVec4f Draw4D() const
    {
        const std::array<float, 4> sample = _Draw<4>();
        return GfVec4f(sample[0], sample[1], sample[2], sample[3]);
    }

private:
    HdEmbreeSampleDomain _LegacyChild(HdEmbreeSampleDomainKey key,
                                      uint32_t a = 0u,
                                      uint32_t b = 0u) const
    {
        return HdEmbreeSampleDomain(
            HdEmbree_MixSampleDomain(seed, key, a, b),
            sampleIndex,
            useRandom,
            sequence);
    }

    template <int Size>
    std::array<float, Size> _Draw() const
    {
        static_assert(Size >= 1, "Draw size must be at least one.");
        static_assert(Size <= 4, "Draw size must be at most four.");

        std::array<float, Size> sample{};

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
        if (HdEmbreeSamplerSequenceNeedsOpenQMC(sequence)) {
            std::visit(
                [&sample](auto const& sampler) {
                    using SamplerT = std::decay_t<decltype(sampler)>;
                    if constexpr (!std::is_same_v<SamplerT, std::monostate>) {
                        sampler.template drawSample<Size>(sample.data());
                    }
                },
                openQmcDomain);
            return sample;
        }
#endif

        for (int i = 0; i < Size; ++i) {
            if (useRandom) {
                const uint32_t h = HdEmbree_MixBits(
                    seed ^
                    (sampleIndex * 0x9e3779b9u) ^
                    (static_cast<uint32_t>(i) * 0x517cc1b7u));
                sample[i] = std::min(h * 0x1p-32f, 1.0f - FLT_EPSILON);
            } else {
                sample[i] = HdEmbree_SobolSample(sampleIndex, i, seed);
            }
        }

        return sample;
    }
};

struct HdEmbreeSampler
{
    uint32_t seed = 0u;
    uint32_t sampleIndex = 0u;
    bool useRandom = false;
    HdEmbreeSamplerSequence sequence = HdEmbreeSamplerSequence::Sobol;

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    HdEmbreeOpenQMCVariant openQmcRoot;
#endif

    HdEmbreeSampler(uint32_t pixelSeed,
                    uint32_t pixelX,
                    uint32_t pixelY,
                    uint32_t sampleIdx,
                    HdEmbreeSamplerSequence samplerSequence)
        : seed(pixelSeed)
        , sampleIndex(sampleIdx)
        , useRandom(samplerSequence == HdEmbreeSamplerSequence::Random)
        , sequence(samplerSequence)
    {
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
        switch (sequence) {
        case HdEmbreeSamplerSequence::OpenQMCSobol:
            openQmcRoot = oqmc::SobolSampler(
                pixelX, pixelY, 0u, sampleIdx, _GetOpenQMCCache<oqmc::SobolSampler>());
            break;
        case HdEmbreeSamplerSequence::OpenQMCSobolBN:
            openQmcRoot = oqmc::SobolBnSampler(
                pixelX, pixelY, 0u, sampleIdx, _GetOpenQMCCache<oqmc::SobolBnSampler>());
            break;
        case HdEmbreeSamplerSequence::OpenQMCPMJ:
            openQmcRoot = oqmc::PmjSampler(
                pixelX, pixelY, 0u, sampleIdx, _GetOpenQMCCache<oqmc::PmjSampler>());
            break;
        case HdEmbreeSamplerSequence::OpenQMCPMJBN:
            openQmcRoot = oqmc::PmjBnSampler(
                pixelX, pixelY, 0u, sampleIdx, _GetOpenQMCCache<oqmc::PmjBnSampler>());
            break;
        case HdEmbreeSamplerSequence::OpenQMCLattice:
            openQmcRoot = oqmc::LatticeSampler(
                pixelX, pixelY, 0u, sampleIdx, _GetOpenQMCCache<oqmc::LatticeSampler>());
            break;
        case HdEmbreeSamplerSequence::OpenQMCLatticeBN:
            openQmcRoot = oqmc::LatticeBnSampler(
                pixelX, pixelY, 0u, sampleIdx, _GetOpenQMCCache<oqmc::LatticeBnSampler>());
            break;
        case HdEmbreeSamplerSequence::Random:
        case HdEmbreeSamplerSequence::Sobol:
            break;
        }
#else
        (void)pixelX;
        (void)pixelY;
#endif
    }

    HdEmbreeSampleDomain RootDomain() const
    {
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
        if (HdEmbreeSamplerSequenceNeedsOpenQMC(sequence)) {
            return HdEmbreeSampleDomain(
                seed, sampleIndex, useRandom, sequence, openQmcRoot);
        }
#endif
        return HdEmbreeSampleDomain(seed, sampleIndex, useRandom, sequence);
    }

private:
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    template <typename SamplerT>
    static char*
    _GetOpenQMCCache()
    {
        struct _Cache {
            std::array<char, SamplerT::cacheSize> bytes{};

            _Cache() { SamplerT::initialiseCache(bytes.data()); }
        };

        static _Cache cache;
        return cache.bytes.data();
    }
#endif
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_SAMPLING_H
