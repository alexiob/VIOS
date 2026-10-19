"""Portable VIOS COBS/CRC32 codec checks; no hardware or Arduino dependency."""
import ctypes
import pathlib
import subprocess
import tempfile
import unittest
import zlib
import sys

ROOT=pathlib.Path(__file__).resolve().parents[1]
class Message(ctypes.Structure):
    _fields_=[('channel',ctypes.c_uint8),('sequence',ctypes.c_uint16),('length',ctypes.c_uint16),('payload',ctypes.c_uint8*240)]

class WireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source=ROOT/'drivers/vios-wire/wire.c'
        if not source.exists():return
        cls.tmp=tempfile.TemporaryDirectory()
        shared=pathlib.Path(cls.tmp.name)/'wire.so'
        subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fPIC','-shared','-I'+str(ROOT/'include'),str(source),'-o',str(shared)],check=True)
        cls.lib=ctypes.CDLL(str(shared))
        cls.lib.vios_wire_crc32.argtypes=[ctypes.c_void_p,ctypes.c_size_t];cls.lib.vios_wire_crc32.restype=ctypes.c_uint32
        cls.lib.vios_wire_encode.argtypes=[ctypes.POINTER(Message),ctypes.c_void_p,ctypes.c_size_t];cls.lib.vios_wire_encode.restype=ctypes.c_size_t
        cls.lib.vios_wire_decode.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(Message)];cls.lib.vios_wire_decode.restype=ctypes.c_int
    @classmethod
    def tearDownClass(cls):
        if hasattr(cls,'tmp'):cls.tmp.cleanup()
    def setUp(self):self.assertTrue(hasattr(self,'lib'),'Portable wire codec missing')
    def encode(self,payload,channel=1,sequence=0x1234):
        message=Message(channel,sequence,len(payload))
        message.payload[:len(payload)]=payload
        output=ctypes.create_string_buffer(252)
        size=self.lib.vios_wire_encode(ctypes.byref(message),output,len(output))
        self.assertGreater(size,0)
        return output.raw[:size]
    def decode(self,frame):
        message=Message()
        good=self.lib.vios_wire_decode(frame,len(frame),ctypes.byref(message))
        return good,message
    def test_crc_known_ieee_vector(self):
        self.assertEqual(self.lib.vios_wire_crc32(b'123456789',9),0xCBF43926)
    def test_roundtrips_all_bytes_and_maximum_payload(self):
        for payload in [b'',b'\0',b'hello\0world',bytes(range(240)),bytes([255])*240,b'\0'*240]:
            encoded=self.encode(payload)
            self.assertEqual(encoded[-1],0);self.assertNotIn(0,encoded[:-1]);self.assertLessEqual(len(encoded),252)
            good,message=self.decode(encoded[:-1])
            self.assertEqual(good,1);self.assertEqual(message.sequence,0x1234);self.assertEqual(bytes(message.payload[:message.length]),payload)
    def test_rejects_corruption_truncation_bad_channel_and_capacity(self):
        encoded=self.encode(b'hello')[:-1]
        for damaged in [b'',b'\0',encoded[:-1],bytes([255])+encoded[1:],encoded[:-1]+bytes([encoded[-1]^1])]:
            self.assertEqual(self.decode(damaged)[0],0)
        message=Message(0,0,1);output=ctypes.create_string_buffer(252)
        self.assertEqual(self.lib.vios_wire_encode(ctypes.byref(message),output,252),0)
        message.channel=1;message.length=241
        self.assertEqual(self.lib.vios_wire_encode(ctypes.byref(message),output,252),0)
        message.length=1
        self.assertEqual(self.lib.vios_wire_encode(ctypes.byref(message),output,2),0)
    def test_frame_header_and_crc_match_independent_python_decode(self):
        wire=self.encode(b'abc',channel=2,sequence=0xBEEF)[:-1]
        raw=bytearray();index=0
        while index<len(wire):
            count=wire[index];index+=1
            raw.extend(wire[index:index+count-1]);index+=count-1
            if count<255 and index<len(wire):raw.append(0)
        self.assertEqual(raw[:9],bytes.fromhex('0102efbe0300616263'))
        self.assertEqual(int.from_bytes(raw[-4:],'little'),zlib.crc32(raw[:-4]))

    def test_actual_firmware_bridge_boot_control_and_health(self):
        with tempfile.TemporaryDirectory() as directory:
            obj=pathlib.Path(directory)/'wire.o';binary=pathlib.Path(directory)/'bridge'
            firmware=ROOT/'firmware/esp32-vios'
            subprocess.run(['cc','-std=c99','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-c',str(ROOT/'drivers/vios-wire/wire.c'),'-o',str(obj)],check=True)
            for requested,expected in ((None,1152000),(384000,384000),(115200,115200)):
                defines=[f'-DTEST_EXPECTED_UART_BAUD={expected}']+([] if requested is None else [f'-DVIOS_INTERNAL_BAUD={requested}'])
                subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined']+defines+['-I'+str(ROOT/'include'),'-I'+str(firmware/'include'),'-I'+str(firmware/'lib/vios-wire/src'),str(firmware/'tests/bridge.cpp'),str(obj),'-o',str(binary)],check=True)
                result=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertIn('health checks passed',result.stdout)
                self.check_real_resident_debug_recovery(binary)

    def check_real_resident_debug_recovery(self,binary):
        """Actual bridge replies must satisfy the inherited host recovery API."""
        sys.path.insert(0,str(ROOT/'tools'))
        from vios_wire import Decoder
        from vios_resident_transport import ResidentSession
        from vios_resident_board import ResidentBoard
        class FirmwarePeer:
            def __init__(self):self.decoder=Decoder();self.pending=bytearray();self.controls=[]
            def write(self,data):
                for packet in self.decoder.feed(data):
                    assert packet.channel==2
                    self.controls.append(packet.payload.rstrip(b'\n'))
                    result=subprocess.run([str(binary),'--control-frame',packet.payload.decode('ascii'),str(packet.sequence)],capture_output=True,check=True)
                    self.pending.extend(result.stdout)
            def read(self):
                data=bytes(self.pending[:17]);del self.pending[:17];return data
            def close(self):pass
        raw=FirmwarePeer();session=ResidentSession(raw);board=ResidentBoard(session=session)
        board.deployed=True
        registers=board.debug_registers()
        self.assertEqual(registers['status'],0x98)
        self.assertEqual(raw.controls,[b'H',b'S',b'Y'])
        self.assertFalse(board.paused or session.paused or board.transition_unknown or board.uncertain)
        # Legacy upload adapters also consume the stable GO acknowledgement.
        session.write(b'\x1d'*3);session.until(b'\r\nDEBUG\r\n',.1)
        session.write(b'X')
        self.assertIn(b'GO 040000 USB/UART bridge 115200\r\n',session.until(b'GO 040000 USB/UART bridge 115200\r\n',.1))

    def test_actual_vdp_uart_initializer_uses_selected_rate_and_retains_flow_control(self):
        firmware=ROOT/'firmware/esp32-vios'
        with tempfile.TemporaryDirectory() as directory:
            temp=pathlib.Path(directory)
            (temp/'HardwareSerial.h').write_text(r"""#pragma once
#include <cassert>
#include <cstdint>
#define SERIAL_8N1 8
#define HW_FLOWCTRL_CTS_RTS 3
struct SerialProbe {
 unsigned baud=0,stage=0;
 void end(){assert(stage==0);++stage;}
 void setRxBufferSize(unsigned n){assert(stage++==1&&n==256);}
 void setTxBufferSize(unsigned n){assert(stage++==2&&n==1024);} // writes queue, never stall the VDP loop
 void begin(unsigned rate,unsigned config,int rx,int tx){assert(stage++==3&&config==8&&rx==34&&tx==2);baud=rate;}
 void setHwFlowCtrlMode(unsigned mode,unsigned threshold){assert(stage++==4&&mode==3&&threshold==64);}
 void setPins(int rx,int tx,int cts,int rts){assert(stage++==5&&rx==-1&&tx==-1&&cts==14&&rts==13);}
 void write(uint8_t){}
} Serial2;
""")
            (temp/'probe.cpp').write_text(r"""void vios_vdp_tx_begin(){} void vios_vdp_tx_end(){}
#include "vdp_protocol.h"
int main(){setupVDPProtocol();assert(Serial2.stage==6&&Serial2.baud==TEST_EXPECTED_UART_BAUD);}
""")
            for requested,expected,mux in ((None,1152000,1),(384000,384000,1),(115200,115200,1),(None,115200,0)):
                defines=[f'-DVIOS_USB_MUX={mux}',f'-DTEST_EXPECTED_UART_BAUD={expected}']+([] if requested is None else [f'-DVIOS_INTERNAL_BAUD={requested}'])
                subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror']+defines+['-I'+str(temp),'-I'+str(ROOT/'include'),'-I'+str(firmware/'vendor/agon-vdp/video'),str(temp/'probe.cpp'),'-o',str(temp/'probe')],check=True)
                subprocess.run([str(temp/'probe')],check=True)

    def test_actual_low_level_zdi_gpio_instruction_bytes_and_critical_state(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=pathlib.Path(directory)/'zdi'
            firmware=ROOT/'firmware/esp32-vios'
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I'+str(firmware/'include'),str(firmware/'tests/zdi.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_production_stream_read_timeouts_exclude_debug_pause(self):
        firmware=ROOT/'firmware/esp32-vios'
        header=(firmware/'vendor/agon-vdp/video/vdu_stream_processor.h').read_text()
        def definition(signature):
            start=header.index(signature);brace=header.index('{',start);depth=1;end=brace+1
            while depth:
                if header[end]=='{':depth+=1
                elif header[end]=='}':depth-=1
                end+=1
            return header[start:end]
        prefix=r'''#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#define COMMS_TIMEOUT 200
#define pdMS_TO_TICKS(x) (x)
static uint32_t tick;
static bool pause_once;
static unsigned waits;
struct Stream {std::deque<uint8_t> bytes;int available(){return bytes.size();}int read(){int value=bytes.front();bytes.pop_front();return value;}void readBytes(uint8_t *p,unsigned n){while(n--)*p++=read();}};
static Stream stream;
static unsigned long millis(){return tick++;}
static uint32_t xTaskGetTickCountFromISR(){return tick++;}
static void delay(unsigned n){tick+=n;}
static void debug_log(const char*,...){}
static unsigned long vios_vdp_wait_running(){++waits;if(!pause_once)return 0;pause_once=false;tick+=1000;stream.bytes={42,43,44};return 1000;}
struct VDUStreamProcessor {Stream *inputStream=&stream;bool byteAvailable(){return inputStream->available()>0;}uint8_t readByte(){vios_vdp_wait_running();return inputStream->read();}int16_t readByte_t(uint16_t);uint32_t readIntoBuffer(uint8_t*,uint32_t,uint16_t);};
'''
        suffix=r'''int main(){VDUStreamProcessor p;pause_once=true;assert(p.readByte_t(200)==42&&tick>=1000);stream.bytes.clear();tick=0;assert(p.readByte_t(2)==-1);uint8_t guarded[5]={99,0,0,0,98};tick=0;pause_once=true;assert(p.readIntoBuffer(guarded+1,3,200)==0);assert(guarded[0]==99&&guarded[1]==42&&guarded[3]==44&&guarded[4]==98&&tick>=1000);assert(p.readIntoBuffer(nullptr,3,200)==3);assert(waits>0);}
'''
        code=prefix+definition('int16_t VDUStreamProcessor::readByte_t')+'\n'+definition('uint32_t VDUStreamProcessor::readIntoBuffer')+'\n'+suffix
        with tempfile.TemporaryDirectory() as directory:
            source=pathlib.Path(directory)/'reads.cpp';source.write_text(code);binary=pathlib.Path(directory)/'reads'
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
