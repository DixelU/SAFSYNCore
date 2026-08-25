// dllmain.cpp : Windows-specific wiring — WASAPI audio output + WinMM driver surface.
// Everything here is WinAPI; synthesis logic lives in engine.cpp.
#include "pch.h"
#include "core.h"

// TODO: add these headers when implementing WASAPI
// #include <mmdeviceapi.h>
// #include <audioclient.h>
// #include <mmddk.h>   // for modMessage / MIDIOPENDESC / MOD_* constants

// ============================================================
//  WASAPI audio engine  (render thread + client lifecycle)
// ============================================================

// TODO: declare module-level WASAPI objects
//   static IMMDeviceEnumerator* g_enumerator = nullptr;
//   static IAudioClient*        g_audio_client = nullptr;
//   static IAudioRenderClient*  g_render_client = nullptr;
//   static HANDLE               g_render_thread = nullptr;
//   static HANDLE               g_render_event  = nullptr;   // event-driven mode
//   static volatile bool        g_render_running = false;
//   static std::unique_ptr<safsyn::SynthEngine> g_engine;

// TODO: implement audio_render_thread()
//   Called on a dedicated high-priority thread. Must NOT allocate or block.
//
//   DWORD WINAPI audio_render_thread(LPVOID)
//   {
//       SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
//       // AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index) for MMCSS
//
//       while (g_render_running)
//       {
//           WaitForSingleObject(g_render_event, INFINITE);
//           if (!g_render_running) break;
//
//           UINT32 padding = 0;
//           g_audio_client->GetCurrentPadding(&padding);
//           UINT32 avail = buffer_frames - padding;
//           if (avail == 0) continue;
//
//           BYTE* data = nullptr;
//           g_render_client->GetBuffer(avail, &data);
//           g_engine->render_audio(reinterpret_cast<float*>(data), avail);
//           g_render_client->ReleaseBuffer(avail, 0);
//       }
//       return 0;
//   }

// TODO: implement wasapi_open()
//   1. CoInitializeEx(nullptr, COINIT_MULTITHREADED)
//   2. CoCreateInstance(CLSID_MMDeviceEnumerator, ...) → g_enumerator
//   3. g_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)
//   4. device->Activate(IID_IAudioClient, ...) → g_audio_client
//   5. Build WAVEFORMATEX: 32-bit float, 2ch, 48 kHz (or query mix format)
//   6. g_audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, ...)
//   7. g_audio_client->SetEventHandle(g_render_event)
//   8. g_audio_client->GetService(IID_IAudioRenderClient, ...) → g_render_client
//   9. Construct a SynthEngine with the actual sample rate and desired capacity
//  10. g_audio_client->Start()
//  11. CreateThread(..., audio_render_thread, ...) → g_render_thread

// TODO: implement wasapi_close()
//   1. g_render_running = false; SetEvent(g_render_event)
//   2. WaitForSingleObject(g_render_thread, 2000); CloseHandle(g_render_thread)
//   3. g_audio_client->Stop(); release COM objects in reverse order
//   4. CoUninitialize()

// ============================================================
//  WinMM MIDI output driver surface
// ============================================================
//
// To expose this DLL as a Windows MIDI output device the host (WinMM / midiOutOpen)
// calls the exported modMessage() function.  The .def file must export it.
//
// TODO: add SAFSYN.def (or use __declspec(dllexport)) with:
//   EXPORTS
//       modMessage  @1
//
// TODO: implement modMessage():
//
//   DWORD APIENTRY modMessage(UINT device_id, UINT msg,
//                             DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
//   {
//       switch (msg)
//       {
//       case MODM_GETNUMDEVS:
//           return 1;
//
//       case MODM_GETDEVCAPS:
//           // Fill MIDIOUTCAPS at (MIDIOUTCAPS*)param1
//           // Set wTechnology = MOD_SWSYNTH, wVoices/wNotes from
//           // g_engine->voice_capacity(), wChannelMask = 0xFFFF, dwSupport = 0
//           return MMSYSERR_NOERROR;
//
//       case MODM_OPEN:
//           // param1 = MIDIOPENDESC*, param2 = flags
//           // Call wasapi_open(); store callback in MIDIOPENDESC if needed
//           // Reply to host via DriverCallback(...)
//           return MMSYSERR_NOERROR;
//
//       case MODM_CLOSE:
//           // wasapi_close();
//           return MMSYSERR_NOERROR;
//
//       case MODM_DATA:
//           // param1 = packed short MIDI message → forward directly
//           g_engine->consume_short_message((uint32_t)param1);
//           return MMSYSERR_NOERROR;
//
//       case MODM_LONGDATA:
//           // param1 = MIDIHDR* (SysEx) — handle if needed, then MIM_LONGDONE callback
//           return MMSYSERR_NOERROR;
//
//       case MODM_RESET:
//           // All notes off on all channels, reset controllers
//           for (uint8_t ch = 0; ch < 16; ++ch)
//               g_engine->control_change(ch, 123, 0);
//           return MMSYSERR_NOERROR;
//
//       case MODM_SETVOLUME:
//       case MODM_GETVOLUME:
//           return MMSYSERR_NOTSUPPORTED;
//
//       default:
//           return MMSYSERR_NOTSUPPORTED;
//       }
//   }

// ============================================================
//  DLL entry point
// ============================================================

bool APIENTRY DllMain(HMODULE   hModule,
                      DWORD     ul_reason_for_call,
                      LPVOID    lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // TODO: call wasapi_open() here (or defer to MODM_OPEN)
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr)
        {
            // TODO: call wasapi_close() — only safe when not terminating (lpReserved == null)
        }
        break;
    }

    return true;
}

