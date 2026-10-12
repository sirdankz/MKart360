extern "C" void x360_log(const char*);
#define X360_EXPECTED_ROM_CRC 0x434389C1U
#define X360_BR_ROM_CRC 0x3B0D98C1U
#include <xtl.h>
#include <string.h>
extern "C" {
#include <ultra64.h>
#include "xbox360/assets.h"
}
extern "C" {
__declspec(align(16)) unsigned char x360_rom[0xC00000];
__declspec(align(16)) unsigned char x360_game_heap[0x1000000];
}
static bool romReady;
#ifndef X360_ROM_PATH
#define X360_ROM_PATH "game:\\baserom.br.z64"
#endif
extern "C" int x360_load_local_rom(void) {
    romReady=false;
    HANDLE f=CreateFileA(X360_ROM_PATH,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if(f==INVALID_HANDLE_VALUE){x360_log("MK64: baserom.br.z64 must be beside the XEX\n");return 0;}
    DWORD got=0;bool ok=GetFileSize(f,NULL)==sizeof(x360_rom) && ReadFile(f,x360_rom,sizeof(x360_rom),&got,NULL) && got==sizeof(x360_rom);CloseHandle(f);
    if(!ok){x360_log("MK64: ROM read or size validation failed\n");return 0;}
    unsigned int crc=0xFFFFFFFFU;
    for(unsigned int i=0;i<sizeof(x360_rom);++i){crc^=x360_rom[i];for(int j=0;j<8;++j)crc=(crc>>1)^((0U-(crc&1))&0xEDB88320U);}
    unsigned int finalCrc=(crc^0xFFFFFFFFU);
    if(finalCrc!=X360_EXPECTED_ROM_CRC && finalCrc!=X360_BR_ROM_CRC){x360_log("MK64: unsupported or corrupt local ROM\n");return 0;}
    romReady=true;
    if(finalCrc==X360_BR_ROM_CRC)x360_log("MK64: local BR ROM validated\n");
    else x360_log("MK64: local US ROM validated\n");
    return 1;
}
extern "C" s32 osPiStartDma(OSIoMesg *io,s32 priority,s32 direction,uintptr_t device,void *ram,size_t size,OSMesgQueue *queue) {
    (void)priority;
    if(!io || !ram || !queue || !romReady || direction!=OS_READ)return -1;
    uintptr_t address=(uintptr_t)device,base=(uintptr_t)x360_rom;
    if(address>=base && address-base<sizeof(x360_rom) && size>sizeof(x360_rom)-(address-base))return -1;
    memcpy(ram,(const void*)address,size);
    io->hdr.status=0;io->hdr.retQueue=queue;io->dramAddr=ram;io->devAddr=device;io->size=size;
    return osSendMesg(queue,(OSMesg)io,OS_MESG_BLOCK);
}
extern "C" void osCreatePiManager(OSPri,OSMesgQueue *q,OSMesg *messages,s32 count) {osCreateMesgQueue(q,messages,count);}
/* Transfers above complete on the CPU before the completion message is sent.
 * No N64 hardware DMA or N64 instruction cache exists in this native path. */
extern "C" void osInvalDCache(void*,size_t) {MemoryBarrier();}
extern "C" void osInvalICache(void*,size_t) {MemoryBarrier();}
extern "C" void osWritebackDCacheAll(void) {MemoryBarrier();}
