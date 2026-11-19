// Short-lived non-activating window and quiet audio for the Windows capture test.
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <cmath>

LRESULT CALLBACK paint(HWND window, UINT message, WPARAM w, LPARAM l)
{
	if (message == WM_PAINT) {
		PAINTSTRUCT ps;
		auto dc = BeginPaint(window, &ps);
		auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
		RECT left{0, 0, 48, 64}, right{48, 0, 96, 64};
		SetDCBrushColor(dc, RGB(255, 0, 0));
		FillRect(dc, &left, brush);
		SetDCBrushColor(dc, RGB(0, 255, 0));
		FillRect(dc, &right, brush);
		EndPaint(window, &ps);
		return 0;
	}
	if (message == WM_TIMER || message == WM_CLOSE) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, w, l);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
	WNDCLASSW wc{};
	wc.lpfnWndProc = paint;
	wc.hInstance = instance;
	wc.lpszClassName = L"CensorCaptureFixture";
	if (!RegisterClassW(&wc))
		return 1;
	auto window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"5050 local test", WS_POPUP, 10, 10,
				      96, 64, nullptr, nullptr, instance, nullptr);
	if (!window)
		return 1;
	ShowWindow(window, SW_SHOWNOACTIVATE);
	UpdateWindow(window);
	SetTimer(window, 1, 30000, nullptr);

	WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 1, 48000, 96000, 2, 16, 0};
	HWAVEOUT device = nullptr;
	if (waveOutOpen(&device, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
		return 2;
	std::array<short, 48000> samples{};
	// -66 dBFS. A half-second of tone alternates with a half-second of silence.
	for (size_t i = 0; i < samples.size() / 2; ++i)
		samples[i] = short(16 * std::sin(i * 2 * 3.14159265 * 440 / 48000));
	WAVEHDR header{};
	header.lpData = reinterpret_cast<char *>(samples.data());
	header.dwBufferLength = sizeof(samples);
	header.dwLoops = 32;
	header.dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
	if (waveOutPrepareHeader(device, &header, sizeof(header)) ||
	    waveOutWrite(device, &header, sizeof(header)))
		return 2;
	MSG msg{};
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	waveOutReset(device);
	waveOutUnprepareHeader(device, &header, sizeof(header));
	waveOutClose(device);
	DestroyWindow(window);
	return 0;
}
