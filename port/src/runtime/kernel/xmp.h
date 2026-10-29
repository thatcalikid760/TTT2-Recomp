// XMP (XAM app 0xFA, the music player): the messages TEKKEN TUNES sends (docs/tekken_tunes_plan.md).
// Today: XMPGetMediaSources answers no sources, every app-0xFA message is logged, and with env
// TTT2_TUNES_STUB=1 a stub source with three silent songs lets the menu and fight traffic be seen
// (plan Task 1).
#pragma once
#include <cstdint>

// XMsgStartIORequest(Ex) for app 0xFA (xlive.cpp). Returns the HRESULT; a message the host does not
// serve gets E_FAIL, as XLiveDispatch answers it.
uint32_t XmpStartIO(uint8_t* base, uint32_t msg, uint32_t overlapped, uint32_t buffer, uint32_t length, uint32_t lr);

// XMsgInProcessCall for app 0xFA (kernel_xam.cpp). Returns the HRESULT: 0xFFFFFFFF for 0x7001B
// (XMPGetPlaybackController, unchanged), otherwise 0.
uint32_t XmpInProcess(uint8_t* base, uint32_t msg, uint32_t param, uint32_t lr);

// The XMP song enumerator (XamCreateEnumeratorHandle with app 0xFA, xlive.cpp): `priv` is its private
// block. XmpEnumerate serves XamEnumerate on it; `done` is the enumerator's "records already sent".
void XmpEnumCreated(uint32_t handle, uint32_t open, uint32_t close, uint32_t extra, uint32_t items, uint32_t priv);
void XmpEnumerate(uint8_t* base, uint32_t handle, uint32_t priv, uint32_t buffer, uint32_t bytes, bool& done,
                  uint32_t& count, uint32_t& result);
void XmpEnumClosed(uint32_t handle);
