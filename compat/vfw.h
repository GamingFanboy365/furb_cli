#pragma once
#include <mmsystem.h>
typedef WinCOM *PAVIFILE, *PAVISTREAM;
typedef struct { DWORD fccType, fccHandler, dwFlags, dwCaps; WORD wPriority, wLanguage; DWORD dwScale, dwRate, dwStart, dwLength,
	dwInitialFrames, dwSuggestedBufferSize, dwQuality, dwSampleSize; RECT rcFrame; DWORD dwEditCount, dwFormatChangeCount; WCHAR szName[64]; } AVISTREAMINFO;
typedef struct { DWORD fccType, fccHandler, dwKeyFrameEvery, dwQuality, dwBytesPerSecond, dwFlags; LPVOID lpFormat; DWORD cbFormat;
	LPVOID lpParms; DWORD cbParms, dwInterleaveEvery; } AVICOMPRESSOPTIONS, *LPAVICOMPRESSOPTIONS;
enum { OF_CREATE = 0x1000, OF_WRITE = 1, AVIIF_KEYFRAME = 0x10, ICMF_CHOOSE_KEYFRAME = 1, ICMF_CHOOSE_DATARATE = 2, AVIERR_OK = 0 };
#define streamtypeVIDEO mmioFOURCC('v','i','d','s')
#define streamtypeAUDIO mmioFOURCC('a','u','d','s')
#define AVIERR_BADFLAGS ((HRESULT)(int32_t)0x80044000)
#define AVIERR_BADFORMAT ((HRESULT)(int32_t)0x80044001)
#define AVIERR_BADHANDLE ((HRESULT)(int32_t)0x80044002)
#define AVIERR_BADPARAM ((HRESULT)(int32_t)0x80044003)
#define AVIERR_BADSIZE ((HRESULT)(int32_t)0x80044004)
#define AVIERR_BUFFERTOOSMALL ((HRESULT)(int32_t)0x80044005)
#define AVIERR_CANTCOMPRESS ((HRESULT)(int32_t)0x80044006)
#define AVIERR_COMPRESSOR ((HRESULT)(int32_t)0x80044007)
#define AVIERR_ERROR ((HRESULT)(int32_t)0x80044008)
#define AVIERR_FILEOPEN ((HRESULT)(int32_t)0x80044009)
#define AVIERR_FILEREAD ((HRESULT)(int32_t)0x8004400A)
#define AVIERR_FILEWRITE ((HRESULT)(int32_t)0x8004400B)
#define AVIERR_INTERNAL ((HRESULT)(int32_t)0x8004400C)
#define AVIERR_MEMORY ((HRESULT)(int32_t)0x8004400D)
#define AVIERR_NOCOMPRESSOR ((HRESULT)(int32_t)0x8004400E)
#define AVIERR_NODATA ((HRESULT)(int32_t)0x8004400F)
#define AVIERR_READONLY ((HRESULT)(int32_t)0x80044010)
#define AVIERR_UNSUPPORTED ((HRESULT)(int32_t)0x80044011)
#define AVIERR_USERABORT ((HRESULT)(int32_t)0x80044012)
