"""Shared module/persistence tools for RAM and resident VIOS board backends.

A backend supplies evaluate(source), an RLock named lock, and compilation state.
All commands use the normal framed evaluation path. No hardware opens here.
"""
import hashlib
import re
import zlib

IDENTIFIER = r'[!-~]{1,31}'
PATH = r'[!-~]{1,127}'


def _token(value, pattern=IDENTIFIER, label='identifier'):
    if not isinstance(value,str) or re.fullmatch(pattern,value) is None:
        raise ValueError(label+' must be one bounded printable ASCII token')
    return value


def _crc(value):
    if type(value) is int and 0<=value<=0xffffffff:
        return value
    if isinstance(value,str) and re.fullmatch(r'[0-9a-fA-F]{8}',value):
        return int(value,16)
    raise ValueError('crc32 must be an unsigned 32-bit integer or eight hexadecimal digits')


def _source(value):
    if not isinstance(value,str) or len(value)>4096:
        raise ValueError('Module source must be a string of at most 4096 ASCII bytes')
    try:
        data=value.encode('ascii')
    except UnicodeEncodeError as error:
        raise ValueError('Module source must be ASCII') from error
    if any(byte<32 and byte not in (9,10,13) or byte>126 for byte in data):
        raise ValueError('Module source contains an unsupported control byte')
    return data


def _metadata(data):
    return {'bytes':len(data),'crc32':zlib.crc32(data),'crc32_hex':f'{zlib.crc32(data):08X}',
            'source_sha256':hashlib.sha256(data).hexdigest()}


class ModuleClient:
    def _module_idle(self):
        if getattr(self,'compiling',False):
            raise RuntimeError('Definition in progress; finish or abort before module operations')

    def _module_commands(self,commands):
        with self.lock:
            self._module_idle()
            outputs=[]
            for command in commands:
                result=dict(self.evaluate(command))
                outputs.append(result.get('output',''))
                if not result.get('ok',False):
                    result['output']=''.join(outputs)
                    return result
            result['output']=''.join(outputs)
            return result

    def module_stage(self,name,version,source,test_word='',dependencies=None):
        name=_token(name);version=_token(version)
        if not isinstance(test_word,str):
            raise ValueError('test_word must be an ASCII token or an empty string')
        if test_word:_token(test_word,label='test word')
        data=_source(source)
        if dependencies is None:dependencies=[]
        if not isinstance(dependencies,list) or len(dependencies)>4:
            raise ValueError('dependencies must be a list of at most four entries')
        normalized=[];seen=set()
        for item in dependencies:
            if not isinstance(item,dict) or set(item)!={'name','crc32'}:
                raise ValueError('Each dependency requires only name and crc32')
            dependency=_token(item['name'],label='dependency name');crc=_crc(item['crc32'])
            if dependency in seen:raise ValueError('Duplicate module dependency')
            seen.add(dependency);normalized.append({'name':dependency,'crc32':crc})
        # Validate the entire request before the first BEGIN is transmitted.
        commands=[f'MODULE-BEGIN {name} {version} {test_word or "-"}']
        commands.extend(f'MODULE-DEP {item["name"]} {item["crc32"]:08X}' for item in normalized)
        chunk=min(116,(getattr(self,'source_line_limit',245)-12)//2)
        commands.extend('MODULE-DATA '+data[offset:offset+chunk].hex().upper() for offset in range(0,len(data),chunk))
        commands.append('MODULE-STAGE')
        result=self._module_commands(commands)
        result.update({'name':name,'version':version,'dependencies':normalized,**_metadata(data)})
        return result

    def module_activate(self,name):
        return self._module_commands(['MODULE-ACTIVATE '+_token(name)])

    def module_discard(self):
        return self._module_commands(['MODULE-DISCARD'])

    def module_rollback(self,name):
        return self._module_commands(['MODULE-ROLLBACK '+_token(name)])

    def modules(self):
        result=self._module_commands(['MODULES'])
        if not result.get('ok'):return result
        entries=[];by_name={};dependencies=[]
        for line in result.get('output','').replace('\r','').splitlines():
            line=line.strip()
            match=re.fullmatch(r'module (\S+) version (\S+) crc32 (\d+) bytes (\d+) deps (\d+) active\s*([01]) previous\s*([01])',line)
            if match:
                name,version,crc,length,count,active,previous=match.groups()
                _token(name);_token(version)
                if name in by_name or int(length)>4096 or int(count)>4:
                    raise IOError('Malformed module inventory')
                entry={'name':name,'version':version,'crc32':_crc(int(crc)),
                       'bytes':int(length),'dependency_count':int(count),'active':active=='1',
                       'previous':previous=='1','dependencies':[]}
                entries.append(entry);by_name[name]=entry
                continue
            match=re.fullmatch(r'dependency (\S+) module (\S+) crc32 (\d+)',line)
            if match:
                owner,name,crc=match.groups();dependencies.append((owner,{'name':_token(name),'crc32':_crc(int(crc))}))
            elif line.startswith(('module ','dependency ')):
                raise IOError('Malformed module inventory line')
        for owner,dependency in dependencies:
            if owner not in by_name:raise IOError('Dependency refers to an absent module')
            items=by_name[owner]['dependencies']
            if any(item['name']==dependency['name'] for item in items):raise IOError('Duplicate inventory dependency')
            items.append(dependency)
        if any(entry['dependency_count']!=len(entry['dependencies']) for entry in entries):
            raise IOError('Module dependency count mismatch')
        result['modules']=entries
        return result

    def module_source(self,name,previous=False):
        name=_token(name)
        if type(previous) is not bool:raise ValueError('previous must be a boolean')
        result=self._module_commands(['MODULE-SOURCE '+name+(' PREVIOUS' if previous else '')])
        if not result.get('ok'):return result
        chunks=[]
        for line in result.get('output','').replace('\r','').splitlines():
            line=line.strip()
            if line.startswith('source-hex'):
                match=re.fullmatch(r'source-hex(?: ([0-9a-fA-F]{0,128}))?',line)
                if not match or len(match[1] or '')%2:raise IOError('Malformed module source hex')
                chunks.append(bytes.fromhex(match[1] or ''))
        if not chunks:raise IOError('Missing module source response')
        data=b''.join(chunks)
        try:source=data.decode('ascii')
        except UnicodeDecodeError as error:raise IOError('Module source response is not ASCII') from error
        _source(source)
        result.update({'name':name,'previous':previous,'source':source,**_metadata(data)})
        return result

    def module_save(self,name,path=None):
        command='SAVE '+_token(name)
        if path is not None:command+=' '+_token(path,PATH,'path')
        result=self._module_commands([command])
        if result.get('ok'):
            match=re.search(r'(?:^|\r?\n)saved ([!-~]{1,127})(?:\r?\n|$)',result.get('output',''))
            if match:result['path']=match[1]
        return result

    def module_load(self,path):
        return self._module_commands(['MODULE-LOAD '+_token(path,PATH,'path')])

    def load(self,path):
        return self._module_commands(['LOAD '+_token(path,PATH,'path')])

    def boot_status(self):
        result=self._module_commands(['BOOT-STATUS'])
        if result.get('ok'):
            match=re.search(r'(?:^|\r?\n)boot confirmed (A|B|NONE) candidate (A|B|NONE) state (CLEAN|PENDING|TRIED) current (A|B|NONE) generation (\d+)(?:\r?\n|$)',result.get('output',''))
            if not match:raise IOError('Missing or malformed boot status')
            result['boot']=dict(zip(('confirmed','candidate','state','current'),match.groups()[:4]),generation=int(match[5]))
        return result

    def boot_manifest(self):
        """Parse BOOT-MANIFEST: what the confirmed/candidate slot would load at power-on.

        Each role is {'slot','status','error','crc32','modules'}; status 'ok', 'none'
        (no slot selected) or 'error' (bundle unusable, reason in 'error'). Module
        version is None when its package is not a verified copy of that source.
        """
        result=self._module_commands(['BOOT-MANIFEST'])
        if not result.get('ok'):return result
        roles={};counts={}
        for line in result.get('output','').replace('\r','').splitlines():
            line=line.strip()
            match=re.fullmatch(r'manifest (A|B|NONE) (confirmed|candidate) (?:(none)|error (.{1,95})|modules (\d+) crc32 (\d+))',line)
            if match:
                slot,role,none,error,count,crc=match.groups()
                if role in roles or (slot=='NONE')!=(none is not None):raise IOError('Malformed boot manifest header')
                roles[role]={'slot':slot,'status':'none' if none else 'error' if error else 'ok','error':error,
                             'crc32':_crc(int(crc)) if crc else None,'modules':[]}
                if count is not None:
                    if int(count)>8:raise IOError('Malformed boot manifest count')
                    counts[role]=int(count)
                continue
            match=re.fullmatch(r'manifest-module (A|B) (\S+) version (\S+) crc32 (\d+) package (ok|bad|unreadable)',line)
            if match:
                slot,name,version,crc,package=match.groups()
                owners=[r for r in roles.values() if r['slot']==slot and r['status']=='ok']
                if not owners or (version=='-')!=(package!='ok'):raise IOError('Malformed boot manifest module line')
                owners[-1]['modules'].append({'name':_token(name),'version':None if version=='-' else _token(version),
                                              'crc32':_crc(int(crc)),'package':package})
            elif line.startswith('manifest'):
                raise IOError('Malformed boot manifest line')
        if set(roles)!={'confirmed','candidate'} or any(len(roles[r]['modules'])!=n for r,n in counts.items()):
            raise IOError('Missing or incomplete boot manifest')
        result['manifest']=roles
        return result

    def boot_trial(self):
        return self._module_commands(['BOOT-TRIAL'])

    def boot_confirm(self):
        return self._module_commands(['BOOT-CONFIRM'])

    def boot_revert(self):
        return self._module_commands(['BOOT-REVERT'])
