"""Module transport/schema tests. Never opens hardware or a real serial port."""
from pathlib import Path
import hashlib
import json
import io
import types
from unittest.mock import patch
import sys
import threading
import unittest
import zlib
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
try:
    from vios_module_client import ModuleClient
except ImportError:
    ModuleClient=None
from vios_harness import RpcServer

class ModuleClientTests(unittest.TestCase):
    def backend(self):
        self.assertIsNotNone(ModuleClient,'ModuleClient is not implemented')
        class Board(ModuleClient):
            def __init__(self):self.lock=threading.RLock();self.calls=[];self.responses=[];self.compiling=False
            def evaluate(self,source,timeout=15):
                self.calls.append(source)
                return self.responses.pop(0) if self.responses else {'ok':True,'output':'ok\r\nvios> ','compiling':False}
        return Board()
    def test_stage_preserves_exact_source_bytes_and_chunks(self):
        board=self.backend();source=': Mixed S" spaces; \\"quotes" 123 ;\r\n'+'\\ source comment\n'*15
        self.assertGreater(len(source),232)
        result=board.module_stage('APP','v1.2',source,'CHECK',[{'name':'CORE','crc32':'AABBCCDD'}])
        self.assertEqual(board.calls[0],'MODULE-BEGIN APP v1.2 CHECK')
        self.assertEqual(board.calls[1],'MODULE-DEP CORE AABBCCDD')
        self.assertEqual(board.calls[-1],'MODULE-STAGE')
        payload=bytearray()
        for command in board.calls[2:-1]:
            self.assertTrue(command.startswith('MODULE-DATA '));self.assertLessEqual(len(command),244)
            payload.extend(bytes.fromhex(command.split()[1]))
        self.assertEqual(payload,source.encode('ascii'))
        self.assertEqual(result['crc32'],zlib.crc32(payload));self.assertEqual(result['source_sha256'],hashlib.sha256(payload).hexdigest())
        self.assertEqual(result['bytes'],len(payload));self.assertTrue(result['ok'])
    def test_rejects_whole_request_before_begin_and_stops_on_target_error(self):
        board=self.backend()
        bad=[{'name':'BAD NAME'},{'version':'\nX'},{'source':'x'*4097},{'source':'\x00'},{'source':'é'},
             {'test_word':'A B'},{'dependencies':[{'name':'CORE','crc32':-1}]},
             {'dependencies':[{'name':'CORE','crc32':1},{'name':'CORE','crc32':2}]},
             {'dependencies':[{'name':'CORE','crc32':'BAD'}]},
             {'dependencies':[{'name':str(i),'crc32':i} for i in range(5)]}]
        for invalid in bad:
            args={'name':'APP','version':'1','source':': X ;',**invalid}
            with self.assertRaises(ValueError):board.module_stage(**args)
        self.assertEqual(board.calls,[])
        board.responses=[{'ok':True,'output':'begin\r\n'},{'ok':False,'output':'error: rejected\r\n'}]
        result=board.module_stage('APP','1',': X ;',dependencies=[{'name':'CORE','crc32':12}])
        self.assertFalse(result['ok']);self.assertEqual(board.calls,['MODULE-BEGIN APP 1 -','MODULE-DEP CORE 0000000C'])
    def test_inventory_source_and_boot_metadata_use_decimal(self):
        board=self.backend()
        board.responses=[{'ok':True,'output':'MODULES\r\nmodule APP version V2 crc32 4294967295 bytes 12 deps 1 active1 previous0\r\ndependency APP module CORE crc32 305419896\r\nok\r\nvios> '},
                         {'ok':True,'output':'source-hex 3A204D697865642031203B0D0A\r\nok\r\nvios> '},
                         {'ok':True,'output':'BOOT-STATUS\r\nboot confirmed A candidate B state PENDING current A generation 17\r\nok\r\nvios> '}]
        result=board.modules();entry=result['modules'][0]
        self.assertEqual(entry['name'],'APP');self.assertEqual(entry['crc32'],4294967295)
        self.assertEqual(entry['dependencies'],[{'name':'CORE','crc32':305419896}]);self.assertTrue(entry['active'])
        result=board.module_source('APP',previous=True);self.assertEqual(result['source'],': Mixed 1 ;\r\n')
        self.assertEqual(board.calls[-1],'MODULE-SOURCE APP PREVIOUS')
        self.assertEqual(result['source_sha256'],hashlib.sha256(result['source'].encode()).hexdigest())
        result=board.boot_status();self.assertEqual(result['boot'],{'confirmed':'A','candidate':'B','state':'PENDING','current':'A','generation':17})
    def test_mutations_are_explicit_and_paths_cannot_inject_commands(self):
        board=self.backend()
        for method,args,command in [('module_activate',('APP',),'MODULE-ACTIVATE APP'),('module_discard',(),'MODULE-DISCARD'),
                                    ('module_rollback',('APP',),'MODULE-ROLLBACK APP'),('module_save',('APP',),'SAVE APP'),
                                    ('module_save',('APP','/VIOS/NEW.VIM'),'SAVE APP /VIOS/NEW.VIM'),
                                    ('module_load',('/VIOS/NEW.VIM',),'MODULE-LOAD /VIOS/NEW.VIM'),
                                    ('load',('/VIOS/START.FTH',),'LOAD /VIOS/START.FTH'),
                                    ('boot_trial',(),'BOOT-TRIAL'),('boot_confirm',(),'BOOT-CONFIRM'),('boot_revert',(),'BOOT-REVERT')]:
            getattr(board,method)(*args);self.assertEqual(board.calls[-1],command)
        before=list(board.calls)
        for path in ['/VIOS/X\nREBOOT','A B','x'*128,'é']:
            with self.assertRaises(ValueError):board.module_load(path)
        with self.assertRaises(ValueError):board.module_source('APP',previous='false')
        self.assertEqual(board.calls,before)
        board.compiling=True
        with self.assertRaises(RuntimeError):board.module_discard()
        self.assertEqual(board.calls,before)
    def test_boundary_sources_save_paths_and_malformed_readback(self):
        board=self.backend()
        result=board.module_stage('EMPTY','1','')
        self.assertEqual(board.calls,['MODULE-BEGIN EMPTY 1 -','MODULE-STAGE']);self.assertEqual(result['bytes'],0)
        board.calls.clear();board.module_stage('MAX','1','x'*4096)
        self.assertEqual(len(board.calls),38)
        payload=b''.join(bytes.fromhex(command.split()[1]) for command in board.calls[1:-1])
        self.assertEqual(payload,b'x'*4096)
        board.responses=[{'ok':True,'output':'saved /VIOS/11223344/AABBCCDD.VIM\r\nok\r\nvios> '}]
        self.assertEqual(board.module_save('APP')['path'],'/VIOS/11223344/AABBCCDD.VIM')
        for output in ['source-hex 0\r\n','source-hex FF\r\n','no source here\r\n']:
            board.responses=[{'ok':True,'output':output}]
            with self.assertRaises((IOError,ValueError)):board.module_source('APP')
        board.responses=[{'ok':True,'output':'module APP version V1 crc32 12 bytes 1 deps 1 active1 previous0\r\n'}]
        with self.assertRaises(IOError):board.modules()
        board.responses=[{'ok':True,'output':'boot confirmed A candidate C state CLEAN current A generation 1\r\n'}]
        with self.assertRaises(IOError):board.boot_status()

    def test_resident_cli_selects_lazy_backend_without_ram_deployment(self):
        import vios_harness
        instances=[]
        class Resident:
            def __init__(self,**kwargs):self.arguments=kwargs;self.closed=False;instances.append(self)
            def close(self):self.closed=True
        module=types.ModuleType('vios_resident_board');module.ResidentBoard=Resident
        request={'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{}}}
        incoming=types.SimpleNamespace(buffer=io.BytesIO(json.dumps(request).encode()+b'\n'))
        outgoing=types.SimpleNamespace(buffer=io.BytesIO())
        with patch.dict(sys.modules,{'vios_resident_board':module}),patch.object(sys,'argv',['vios_harness.py','--backend','resident','--port','fixture','--image','fixture-resident.bin','--fast-console']),patch.object(sys,'stdin',incoming),patch.object(sys,'stdout',outgoing),patch.object(vios_harness,'ViosBoard',side_effect=AssertionError('RAM backend must not open')):
            vios_harness.main()
        self.assertEqual(instances[0].arguments,{'port':'fixture','image':'fixture-resident.bin','log':None,'fast_console':True})
        self.assertTrue(instances[0].closed)
        self.assertIn('result',json.loads(outgoing.buffer.getvalue()))

    def test_rpc_nested_dependency_schema_and_tool_registration(self):
        board=self.backend();rpc=RpcServer(board)
        init=rpc.handle({'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{}}})
        self.assertIn('result',init)
        listed=rpc.handle({'jsonrpc':'2.0','id':2,'method':'tools/list'})['result']['tools'];names={t['name'] for t in listed}
        self.assertTrue({'module_stage','module_source','module_save','boot_trial','boot_status','connect'}<=names)
        for arguments in [{'name':'APP','version':'1','source':': X ;','dependencies':[{'name':'CORE','crc32':True}]},
                          {'name':'APP','version':'1','source':': X ;','dependencies':[{'name':'CORE','crc32':1,'extra':1}]},
                          {'name':'APP','previous':'false'}]:
            name='module_source' if 'previous' in arguments else 'module_stage'
            response=rpc.handle({'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':name,'arguments':arguments}})
            self.assertEqual(response['error']['code'],-32602)
        self.assertEqual(board.calls,[])
        response=rpc.handle({'jsonrpc':'2.0','id':4,'method':'tools/call','params':{'name':'module_stage','arguments':{'name':'APP','version':'1','source':': X ;','dependencies':[{'name':'CORE','crc32':305419896}]}}})
        self.assertFalse(response['result']['isError']);self.assertEqual(board.calls[1],'MODULE-DEP CORE 12345678')
        self.assertEqual(json.loads(response['result']['content'][0]['text'])['bytes'],5)
if __name__=='__main__':unittest.main()
