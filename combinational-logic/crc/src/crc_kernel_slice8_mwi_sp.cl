#include "/home/tjkenney/OpenDwarfs/combinational-logic/crc/inc/eth_crc32_lut.h"

#define BITS_PER_GLOBAL_LD 512
#define BYTES_PER_GLOBAL_LD 64
#define INTS_PER_GLOBAL_LD 16

__kernel void crc32_slice8(__global const uint* restrict data, uint length_bytes, const uint length_ints ,__global uint* restrict res)
{
  __private uint crc;
  __private uchar* currentChar;
  __private uint one,two;
  __private size_t i,j,gid;
  __private uint scratch_pad[INTS_PER_GLOBAL_LD];
  
  crc = 0xFFFFFFFF;
  gid = get_global_id(0);
  i = gid * length_ints;
  
  //printf("WI #%zd: first word=%X\n",gid+1,data[i]);
  
  while (length_bytes >= BYTES_PER_GLOBAL_LD) //load 512 bits at a time into local memory
  {
  	for(j=0; j<INTS_PER_GLOBAL_LD; j++)
  		scratch_pad[j] = data[i++];
  		 
    for(j=0; j<INTS_PER_GLOBAL_LD; j+=2) // process data in 8 chunks of 8 bytes
    {
	    one = scratch_pad[j] ^ crc;
	    two = scratch_pad[j+1];
	    crc = crc32Lookup[7][ one      & 0xFF] ^
	          crc32Lookup[6][(one>> 8) & 0xFF] ^
	          crc32Lookup[5][(one>>16) & 0xFF] ^
	          crc32Lookup[4][ one>>24        ] ^
	          crc32Lookup[3][ two      & 0xFF] ^
	          crc32Lookup[2][(two>> 8) & 0xFF] ^
	          crc32Lookup[1][(two>>16) & 0xFF] ^
	          crc32Lookup[0][ two>>24        ];
	}
          
    length_bytes -= BYTES_PER_GLOBAL_LD;
  }
  
  while(length_bytes) // remaining 1 to 63 bytes
  {
	  one = data[i++];
	 currentChar = (unsigned char*) &one;
	  j=0;
	  while (length_bytes && j < 4) 
	  {
	  	length_bytes = length_bytes - 1;
	    crc = (crc >> 8) ^ crc32Lookup[0][(crc & 0xFF) ^ currentChar[j]];
	    j = j + 1;
	  }
  }
  //printf("WI #%zd: crc=%X\n",gid+1,~crc);
  res[gid] = ~crc;
}
