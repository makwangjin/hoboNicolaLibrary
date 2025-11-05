/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * sekigon-gonnoc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

// This example runs both host and device concurrently. The USB host receive
// reports from HID device and print it out over USB Device CDC interface.
// For TinyUSB roothub port0 is native usb controller, roothub port1 is
// pico-pio-usb.

// ================================================================
// [수정] 누락된 헤더 파일 추가 (빌드 오류 해결)
// ================================================================
#include <stdint.h>     // for uint8_t, uint16_t, etc.
#include <stdbool.h>    // for bool, true, false
#include "pio_usb_configuration.h" // for pio_usb_configuration_t, PIO_USB_DEFAULT_CONFIG
// ================================================================


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string.h> // memcpy, memset 사용

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"

#include "pio_usb.h"
#include "tusb.h"

// [수정] 외부 라이브러리 함수 선언
extern void memcpy(void*, const void*, size_t);
extern void memset(void*, int, size_t);

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+

#define KEYBOARD_COLEMAK 
#define HID_KEYCODE_TO_ASCII 

#ifdef KEYBOARD_COLEMAK
const uint8_t colemak[128] = { /* ... */ };
#endif

// [수정] last_mouse_report 초기화 (빌드 경고 해결)
// TinyUSB hid_mouse_report_t는 버튼, X, Y, Wheel 4개의 필드만 가집니다.
static hid_mouse_report_t last_mouse_report = { 0 }; 
static uint8_t hid_mouse_count = 0;
static uint8_t hid_kbd_count = 0;

static uint8_t const keycode2ascii[128][2] =  { HID_KEYCODE_TO_ASCII };

// [수정] CH9329 필터링 및 리포트 저장용 구조체
typedef struct {
	uint8_t report_id;
	uint8_t dev_addr;
	uint8_t instance;
	bool mounted;
	uint8_t protocol; // HID_ITF_PROTOCOL_KEYBOARD or MOUSE
	
	// 키보드 필터링을 위한 이전 상태 저장
	uint8_t prev_kbd_report[8]; // Modifier + Reserved + 6 Keycodes
} device_info_t;

device_info_t device_info = { 0 };

void init_device_info() {
	memset(&device_info, 0, sizeof(device_info_t));
	device_info.report_id = 0xff;
	device_info.dev_addr = 0xff;
	device_info.instance = 0xff;
}


/*------------- MAIN (core0 & core1) -------------*/

// core1: handle host events
void core1_main() {
  sleep_ms(10); 

  // Use tuh_configure() to pass pio configuration to the host stack
  // Note: tuh_configure() must be called before
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG; 
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

  // To run USB SOF interrupt in core1, init host stack for pio_usb (roothub
  // port1) on core1
  tuh_init(1); 

  while (true) {
    tuh_task(); // tinyusb host task
  }
}

// core0: handle device events
int main(void) {
  // default 125MHz is not appropreate. Sysclock should be multiple of 12MHz.<br>
  set_sys_clock_khz(120000, true); 

  sleep_ms(10); 

  multicore_reset_core1(); 
  // all USB task run in core1
  multicore_launch_core1(core1_main); 

  // init device stack on native usb (roothub port0)
  tud_init(0); 
  
  // [추가] 장치 정보 초기화
  init_device_info();

  while (true) {
    tud_task(); // tinyusb device task
    // tud_cdc_write_flush(); // [유지] CDC 포트는 KVM에 필요 없으므로 주석 상태 유지
  }

  return 0;
}

// [제거] tud_cdc_rx_cb 함수는 CH9329 번역에 필요 없으므로 제거

//--------------------------------------------------------------------+
// Host HID - [CH9329의 복합 장치 필터링 로직]
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  (void)desc_report;
  (void)desc_len;

  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
  
  // [수정] 복합 장치 중 '표준 키보드'와 '상대 마우스'만 선택
  if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
  {
      hid_kbd_count++;
      // [필터링] 첫 번째 키보드 인터페이스만 사용
      if (hid_kbd_count == 1) {
          device_info.dev_addr = dev_addr;
          device_info.instance = instance;
          device_info.mounted = true;
          device_info.protocol = HID_ITF_PROTOCOL_KEYBOARD;

          if ( !tuh_hid_receive_report(dev_addr, instance) ) {
              // 에러 처리
          }
      }
  } 
  else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
  {
      hid_mouse_count++;
      // [필터링] 첫 번째 마우스 인터페이스(상대 마우스일 확률 높음)만 선택
      if (hid_mouse_count == 1) {
          device_info.dev_addr = dev_addr;
          device_info.instance = instance;
          device_info.mounted = true;
          device_info.protocol = HID_ITF_PROTOCOL_MOUSE;
          
          if ( !tuh_hid_receive_report(dev_addr, instance) ) {
              // 에러 처리
          }
      }
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  // [수정] 복합 장치 카운터 초기화
  hid_mouse_count = 0;
  hid_kbd_count = 0;
  // [수정] 장치 정보 초기화
  init_device_info();
}

// look up new key in previous keys (원본 함수, 사용되지 않음)
static inline bool find_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
  for(uint8_t i=0; i<6; i++)
  {
    if (report->keycode[i] == keycode)  return true;
  }
  return false;
}

// convert hid keycode to ascii and print via usb device CDC (ignore non-printable)
static void process_kbd_report(uint8_t dev_addr, hid_keyboard_report_t const *report)
{
  (void) dev_addr;
  
  // [수정] KVM (PIO Port)로 재전송 (핵심)
  // [주의] 이 예제는 6KRO를 6KRO로 단순 전달합니다. (KVM이 6KRO를 지원하는 경우)
  tud_hid_keyboard_report(0, report->modifier, report->keycode);
}


// send mouse report to usb device CDC
static void process_mouse_report(uint8_t dev_addr, hid_mouse_report_t const * report)
{
  // [수정] 절대 좌표 필터링: 마우스 리포트가 0이 아닐 때만 전송
  if (report->x == 0 && report->y == 0 && report->wheel == 0 && report->buttons == last_mouse_report.buttons) {
      return;
  }

  // [수정] KVM (PIO Port)로 재전송 (핵심)
  // [오류 수정] tud_hid_mouse_report 함수의 6개 인자에 맞게 0을 추가함 (horizontal_wheel)
  tud_hid_mouse_report(0, report->buttons, report->x, report->y, report->wheel, 0);
  
  // [수정] 상태 저장
  last_mouse_report = *report;
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) len;
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch(itf_protocol)
  {
    case HID_ITF_PROTOCOL_KEYBOARD:
      // [수정] 키보드 인터페이스가 선택된 첫 번째 인터페이스인지 확인
      if (hid_kbd_count == 1) { 
          // [수정] 함수 이름 오타 수정: process_kbd_report 호출 (인자: dev_addr, report)
          process_kbd_report(dev_addr, (hid_keyboard_report_t const*) report );
      }
    break;

    case HID_ITF_PROTOCOL_MOUSE:
      // [수정] 마우스 인터페이스가 선택된 첫 번째 인터페이스인지 확인
      if (hid_mouse_count == 1) {
          // [수정] 함수 이름 오타 수정: process_mouse_report 호출 (인자: dev_addr, report)
          process_mouse_report(dev_addr, (hid_mouse_report_t const*) report );
      }
    break;

    default: 
      // [필터링] 나머지 모든 장치(절대 좌표, 커스텀 HID 등)는 무시하고 버립니다.
    break;
  }

  // continue to request to receive report
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    // 에러 처리
  }
}
