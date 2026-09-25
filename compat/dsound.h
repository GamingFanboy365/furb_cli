#pragma once
#include <windows.h>
#include <mmsystem.h>
typedef struct { DWORD dwSize, dwFlags, dwBufferBytes, dwReserved; LPWAVEFORMATEX lpwfxFormat; GUID guid3DAlgorithm; } DSBUFFERDESC;
enum { DSBCAPS_PRIMARYBUFFER = 1, DSBCAPS_GLOBALFOCUS = 0x8000, DSBCAPS_GETCURRENTPOSITION2 = 0x10000,
	DSBCAPS_CTRLVOLUME = 0x80, DSBCAPS_LOCSOFTWARE = 8, DSSCL_PRIORITY = 2, DSSCL_NORMAL = 1,
	DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBLOCK_ENTIREBUFFER = 2, DSBLOCK_FROMWRITECURSOR = 1 };
#define DSERR_BUFFERLOST ((HRESULT)(int32_t)0x88780096)

// DirectSound as Sound.cpp uses it.  furb_cli's is capture-only (host_cli.cpp:
// handed out only when recording --wav/--avi, the play cursor always "far
// ahead" so Sound::Run never waits); the GUI's plays through SDL (gui/dx.cpp).
struct IDirectSoundBuffer {
	virtual HRESULT SetFormat(const WAVEFORMATEX *f) = 0;
	virtual HRESULT Play(DWORD r1, DWORD r2, DWORD flags) = 0;
	virtual HRESULT Stop(void) = 0;
	virtual HRESULT GetCurrentPosition(DWORD *play, DWORD *write) = 0;
	virtual HRESULT Lock(DWORD offset, DWORD bytes, void **ptr1, DWORD *len1, void **ptr2, DWORD *len2, DWORD flags) = 0;
	virtual HRESULT Unlock(void *ptr1, DWORD len1, void *ptr2, DWORD len2) = 0;
	virtual HRESULT GetStatus(DWORD *s) = 0;
	virtual ULONG Release(void) = 0;
	virtual ~IDirectSoundBuffer() {}
};
struct IDirectSound {
	virtual HRESULT SetCooperativeLevel(HWND h, DWORD level) = 0;
	virtual HRESULT CreateSoundBuffer(const DSBUFFERDESC *desc, IDirectSoundBuffer **out, IUnknown *outer) = 0;
	virtual ULONG Release(void) = 0;
	virtual ~IDirectSound() {}
};
typedef IDirectSound *LPDIRECTSOUND, *LPDIRECTSOUND8;
typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER, *LPDIRECTSOUNDBUFFER8;
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *);
#define DirectSoundCreate8 DirectSoundCreate
