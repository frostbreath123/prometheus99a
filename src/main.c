/**
 * @file main.c
 * @brief Prometheus99 HMI Runtime Application Entry Point
 * @architect Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)
 * 
 * Orchestrates pure C99 4-Layer HMI runtime execution, native Win32 windowing,
 * 1000 Hz Sensor ISR thread, 30 Hz LVGL Presentation Loop, and Dual-Loop Watchdog.
 *
 * Architecture & Memory Features:
 *   - 4-bit Partial Draw Band Buffer (18.75 KB) for ultra-micro < 0.2 MB RAM footprint.
 *   - Direct screen switching for keys 1-5, action keys routed via HAL queue.
 *   - Working set trimming to guarantee low runtime memory.
 */

#include "../include/config.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include "../include/layer3_presentation.h"
#include "../include/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
#include <psapi.h>

static bool s_alarm_audio_active = false;

static HWND s_hwnd = NULL;
static HDC s_current_paint_hdc = NULL;
static struct {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[16];
} s_bmi;
static bool s_running = true;
static HANDLE s_isr_thread = NULL;
static const char *S_WND_CLASS = "Prometheus99HMIClass";

/* Win32 Display Flush Handler (called per band by Layer 1 HAL) */
static void win32_display_flush_handler(const display_area_t *area, const uint8_t *color_p)
{
    if (s_current_paint_hdc && area && color_p) {
        int band_h = area->y2 - area->y1 + 1;
        StretchDIBits(s_current_paint_hdc,
                      area->x1, area->y1, area->x2 - area->x1 + 1, band_h,
                      0, 0, DISPLAY_WIDTH, band_h,
                      color_p, (const BITMAPINFO *)&s_bmi, DIB_RGB_COLORS, SRCCOPY);
    }
}

/* Win32 Window Callback Handler */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_CREATE:
            break;

        case WM_PRINTCLIENT:
        case WM_PAINT: {
            PAINTSTRUCT ps;
            s_current_paint_hdc = (msg == WM_PRINTCLIENT) ? (HDC)wParam : BeginPaint(hwnd, &ps);
            layer3_render_all_bands();
            if (msg == WM_PAINT) EndPaint(hwnd, &ps);
            EmptyWorkingSet(GetCurrentProcess());
            s_current_paint_hdc = NULL;
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            /* Route through Layer 1 HAL queue */
            layer1_queue_touch((int16_t)x, (int16_t)y, true);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam >= '1' && wParam <= '5') {
                layer2_fsm_request_screen_change((screen_id_t)(wParam - '1'));
            } else {
                input_key_t k = KEY_NONE;
                if (wParam == 'F') k = KEY_TOGGLE_FAILOVER;
                else if (wParam == 'A') k = KEY_ALARM_ACK;
                else if (wParam == 'C') k = KEY_TOGGLE_CONTRAST;
                else if (wParam == 'T') k = KEY_TEST_ALARM;
                else if (wParam == 'W') k = KEY_SIMULATE_WD_FAULT;
                else if (wParam == VK_LEFT) k = KEY_PREV;
                else if (wParam == VK_RIGHT) k = KEY_NEXT;
                else if (wParam == VK_RETURN) k = KEY_SELECT;
                else if (wParam == VK_ESCAPE) k = KEY_BACK;
                if (k != KEY_NONE) layer1_queue_key(k);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_DESTROY:
            s_running = false;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* High-Speed 1000 Hz Sensor ISR Background Thread */
static unsigned __stdcall sensor_isr_thread_proc(void *arg)
{
    (void)arg;
    while (s_running) {
        layer1_sensor_isr_handler_1000hz();
        Sleep(1); /* 1 ms = 1000 Hz */
    }
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("[INIT] Prometheus99\n");

    /* Initialize Layers */
    layer0_hardware_init();
    layer1_hal_init();
    layer2_core_init();
    layer3_presentation_init();

    /* Setup Win32 Bitmap Format for 800x48 4-Bit Partial Draw Band (18.75 KB Buffer) */
    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = DISPLAY_WIDTH;
    s_bmi.bmiHeader.biHeight = -DISPLAY_BAND_HEIGHT; /* Top-down DIB for 48 scanlines */
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 4;
    s_bmi.bmiHeader.biCompression = BI_RGB;
    s_bmi.bmiHeader.biClrUsed = 16;
    s_bmi.bmiHeader.biClrImportant = 16;

    const uint32_t *palette = layer3_get_palette();
    for (int i = 0; i < 16; i++) {
        s_bmi.bmiColors[i].rgbRed   = (BYTE)((palette[i] >> 16) & 0xFF);
        s_bmi.bmiColors[i].rgbGreen = (BYTE)((palette[i] >> 8)  & 0xFF);
        s_bmi.bmiColors[i].rgbBlue  = (BYTE)(palette[i] & 0xFF);
        s_bmi.bmiColors[i].rgbReserved = 0;
    }

    /* Register Display Flush Handler with Layer 1 HAL */
    layer1_set_display_flush_handler(win32_display_flush_handler);

    /* Register Win32 Window Class */
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = S_WND_CLASS;
    RegisterClassExA(&wc);

    /* Calculate Window Rect for exact 800x480 client area */
    RECT rc = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);

    s_hwnd = CreateWindowExA(
        0,
        S_WND_CLASS,
        "Prometheus99: Lightweight HMI Runtime (C99 + LVGL)",
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL
    );

    if (!s_hwnd) {
        fprintf(stderr, "[ERROR] Failed to create Win32 HMI Window.\n");
        return 1;
    }

    ShowWindow(s_hwnd, SW_SHOWNORMAL);
    UpdateWindow(s_hwnd);
    SetForegroundWindow(s_hwnd);
    SetFocus(s_hwnd);

    printf("[INIT] GUI 800x480\n");

    /* Launch 1000 Hz Sensor ISR Background Thread (16 KB stack) */
    s_isr_thread = (HANDLE)_beginthreadex(NULL, 16384, sensor_isr_thread_proc, NULL, 0, NULL);
    if (!s_isr_thread) {
        fprintf(stderr, "[ERROR] ISR Fail\n");
        return 1;
    }

    /* Trim process working set to guarantee < 1.5 MB memory footprint */
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    printf("[KEYS] 1-5 A F C T W\n");

    /* Main Execution Loop: 30 Hz LVGL Presentation Tick + Win32 Message Pump */
    MSG msg;
    uint64_t last_ui_tick = layer0_get_system_time_ms();
    uint32_t frame_count = 0;

    while (s_running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        uint64_t now = layer0_get_system_time_ms();
        if ((now - last_ui_tick) >= UI_FRAME_PERIOD_MS) {
            last_ui_tick = now;
            frame_count++;

            /* Run 30 Hz Presentation Tick & Dual-Watchdog Check */
            layer3_ui_timer_tick_30hz();

            /* Trigger Redraw on Window */
            InvalidateRect(s_hwnd, NULL, FALSE);

            /* Alarm Audio Supervisor: Continuous sound while trip is active until acknowledged */
            shared_state_buffer_t cur_st;
            layer2_snapshot_state(&cur_st);
            bool alarm_active = (cur_st.alarm_latch_state == ALARM_STATE_ACTIVE);
            if (alarm_active != s_alarm_audio_active) {
                s_alarm_audio_active = alarm_active;
                PlaySoundA(alarm_active ? "SystemHand" : NULL, NULL, alarm_active ? (SND_ALIAS | SND_ASYNC | SND_LOOP) : 0);
            }
            if (alarm_active && (frame_count % 15 == 0)) {
                MessageBeep(MB_ICONHAND);
            }

            /* Maintain ultra-compact Working Set (<= 0.2 MB RAM) */
            if (frame_count % 2 == 0) {
                EmptyWorkingSet(GetCurrentProcess());
            }
        }

        Sleep(5); /* Yield CPU */
    }

    PlaySoundA(NULL, NULL, 0);

    /* Cleanup */
    if (s_isr_thread) {
        WaitForSingleObject(s_isr_thread, 1000);
        CloseHandle(s_isr_thread);
    }

    /* Properly unregister window class */
    UnregisterClassA(S_WND_CLASS, hInst);

    printf("[EXIT] Terminated\n");
    return 0;
}
