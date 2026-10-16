"""Strict host-side workbench protocol. All I/O uses the existing VM owner.

Patches are single atomic transactions, at most 80 bytes. Reads are locked and
revision checked; no operation retries or silently overwrites a changed draft.
"""
import re
try:
 from .vios_module_client import _token, _source, _metadata
except ImportError:
 from vios_module_client import _token, _source, _metadata


def _integer(value,maximum=0xffffffff,label='integer',minimum=0):
 if type(value) is not int or not minimum<=value<=maximum:
  raise ValueError(f'{label} must be an integer in {minimum}..{maximum}')
 return value


def _boolean(value):
 if type(value) is not bool:raise ValueError('discard/reset must be a boolean')
 return int(value)


def _lines(result,prefix):
 output=result.get('output','')
 # A full 512-entry catalog with 31-byte names/modules exceeds 64 KiB.
 # Match the existing owner output ceiling; this adds no target buffer.
 if not isinstance(output,str) or len(output)>1048576:raise IOError('Invalid workbench response size')
 # Real REPL responses include the command echo. Device records start with
 # a lower-case field/type; WB-SOURCE's numeric arguments and RESET do not.
 return [line.strip() for line in output.replace('\r','').splitlines()
         if re.match(re.escape(prefix)+r' [a-z_]+(?: |$)',line.strip())]


def _record(result,prefix,fields):
 records=_lines(result,prefix)
 if len(records)!=1:raise IOError('Missing or duplicate '+prefix+' response')
 parts=records[0].split()
 if len(parts)!=1+2*len(fields) or parts[1::2]!=list(fields):raise IOError('Malformed '+prefix+' response')
 return dict(zip(fields,parts[2::2]))


def _unsigned(value,maximum=0xffffffff):
 if re.fullmatch(r'[0-9]{1,10}',value) is None or int(value)>maximum:raise IOError('Invalid workbench unsigned integer')
 return int(value)


def _hex(value,maximum,empty=True):
 if value=='-' and empty:return b''
 if not value or len(value)>maximum*2 or len(value)%2 or re.fullmatch('[0-9A-Fa-f]+',value) is None:raise IOError('Malformed workbench hex')
 return bytes.fromhex(value)


def _ascii(value,maximum,source=False):
 data=_hex(value,maximum)
 try:text=data.decode('ascii')
 except UnicodeDecodeError as error:raise IOError('Non-ASCII workbench data') from error
 if any((b<32 and (not source or b not in (9,10,13))) or b>126 for b in data):raise IOError('Unsupported control byte in workbench data')
 return text


def _name(value):
 text=_ascii(value,31)
 if text:
  try:_token(text)
  except ValueError as error:raise IOError('Invalid workbench identifier') from error
 return text


_STATUS_FIELDS=('revision','catalog','open','draft','dirty','staged','active','saved','length','cursor','selection','browser','focus','words','words_max','module','version','test','error')

def _status(result):
 fields=_record(result,'WB-STATUS',_STATUS_FIELDS)
 output={key:_unsigned(fields[key]) for key in _STATUS_FIELDS[:15]}
 for key in ('open','draft','dirty','staged','active','saved'):
  if output[key]>1:raise IOError('Invalid workbench boolean')
  output[key]=bool(output[key])
 if output['length']>4096 or output['cursor']>output['length'] or output['browser']>1 or output['focus']>1 or output['words']>output['words_max'] or output['words_max']>65535:raise IOError('Invalid workbench status bounds')
 for key in ('module','version','test'):output[key]=_name(fields[key])
 output['error']=_ascii(fields['error'],127)
 if output['draft'] and (not output['module'] or not output['version']):raise IOError('Missing workbench draft identity')
 return output


class WorkbenchClient:
 def _wb(self,command,status=True):
  result=self._module_commands([command])
  if result.get('ok') and status:result['workbench']=_status(result)
  return result

 def wb_status(self):return self._wb('WB-STATUS')
 def wb_open(self):return self._wb('WB-OPEN')
 def wb_close(self):return self._wb('WB-CLOSE')
 def wb_redraw(self):return self._wb('WB-REDRAW')

 def wb_browse(self,browser='MODULES',module_filter=None):
  if browser not in ('MODULES','WORDS'):raise ValueError('browser must be MODULES or WORDS')
  command='WB-BROWSE '+browser
  if module_filter is not None:
   if browser!='WORDS':raise ValueError('Module filters apply only to WORDS')
   command+=' '+_token(module_filter)
  result=self._wb(command)
  if not result.get('ok'):return result
  items=[];seen=set()
  for line in _lines(result,'WB-ITEM'):
   parts=line.split()
   if browser=='MODULES':
    if len(parts)!=9 or parts[1]!='module' or parts[3::2]!=['name','version','active']:raise IOError('Malformed module browser entry')
    index=_unsigned(parts[2],7);active=_unsigned(parts[8],1)
    item={'index':index,'kind':'module','name':_name(parts[4]),'version':_name(parts[6]),'active':bool(active)}
   else:
    if len(parts)!=9 or parts[1]!='word' or parts[3::2]!=['kind','name','module']:raise IOError('Malformed word browser entry')
    index=_unsigned(parts[2],65534)
    item={'index':index,'kind':_unsigned(parts[4],5),'name':_name(parts[6]),'module':_name(parts[8])}
    if index>=result['workbench']['words']:raise IOError('Word browser index out of range')
   if index in seen or not item['name']:raise IOError('Duplicate or empty browser entry')
   seen.add(index);items.append(item)
  result['items']=items
  return result

 def wb_select(self,kind,value):
  if kind=='MODULE':argument=_token(value)
  elif kind=='WORD':argument=str(_integer(value,65534,'word index'))
  else:raise ValueError('selection kind must be MODULE or WORD')
  return self._wb('WB-SELECT '+kind+' '+argument)

 def wb_edit(self,module,discard=False):return self._wb(f'WB-EDIT {_token(module)} {_boolean(discard)}')
 def wb_new(self,module,version,discard=False):return self._wb(f'WB-NEW {_token(module)} {_token(version)} {_boolean(discard)}')
 def wb_patch(self,revision,offset,remove,source):
  revision=_integer(revision,label='revision');offset=_integer(offset,4096,'offset');remove=_integer(remove,4096,'remove')
  data=_source(source)
  if len(data)>80 or offset+remove>4096:raise ValueError('Atomic patch must be <=80 bytes and within 4096-byte draft')
  return self._wb(f'WB-PATCH {revision} {offset} {remove} '+(data.hex().upper() or '-'))

 def wb_undo(self,revision):return self._wb('WB-UNDO '+str(_integer(revision,label='revision')))
 def wb_metadata(self,revision,version,test_word=''):
  revision=_integer(revision,label='revision');version=_token(version)
  if not isinstance(test_word,str):raise ValueError('test_word must be a string')
  if test_word:
   _token(test_word)
   if test_word=='-':raise ValueError('A dash is reserved for an empty test word')
  return self._wb(f'WB-META {revision} {version} {test_word or "-"}')
 def wb_stage(self,revision):return self._wb('WB-STAGE '+str(_integer(revision,label='revision')))
 def wb_activate(self,revision):return self._wb('WB-ACTIVATE '+str(_integer(revision,label='revision')))
 def wb_save(self,revision):return self._wb('WB-SAVE '+str(_integer(revision,label='revision')))
 def wb_key(self,code,modifiers=0):
  code=_integer(code,267,'key code');modifiers=_integer(modifiers,255,'modifiers')
  if 127<code<256:raise ValueError('Key code must be ASCII or a semantic workbench key')
  return self._wb(f'WB-KEY {code} {modifiers}')

 def wb_source(self):
  with self.lock:
   first=self.wb_status()
   if not first.get('ok'):return first
   status=first['workbench'];length=status['length'];chunks=[];outputs=[first.get('output','')]
   for offset in range(0,length,80):
    count=min(80,length-offset);result=self._wb(f'WB-SOURCE {offset} {count}',False);outputs.append(result.get('output',''))
    if not result.get('ok'):result['output']=''.join(outputs);return result
    fields=_record(result,'WB-SOURCE',('revision','offset','total','hex'))
    if (_unsigned(fields['revision'])!=status['revision'] or _unsigned(fields['offset'],4096)!=offset or _unsigned(fields['total'],4096)!=length):raise IOError('Workbench source changed during read')
    source=_ascii(fields['hex'],80,True).encode('ascii')
    if len(source)!=count:raise IOError('Workbench source chunk length mismatch')
    chunks.append(source)
   data=b''.join(chunks);first.update(source=data.decode('ascii'),revision=status['revision'],output=''.join(outputs),**_metadata(data));return first

 def wb_view(self,row=0,count=60):
  row=_integer(row,59,'row');count=_integer(count,60,'count',1)
  if row+count>60:raise ValueError('View extends past row 59')
  with self.lock:
   first=self.wb_status()
   if not first.get('ok'):return first
   rows=[];outputs=[first.get('output','')]
   # The native command composes once and streams the requested rows without
   # processing keyboard events between them. Avoid 60 redundant compositions
   # and host round trips for a full screen.
   result=self._wb(f'WB-VIEW {row} {count}',False);outputs.append(result.get('output',''))
   if not result.get('ok'):result['output']=''.join(outputs);return result
   records=_lines(result,'WB-VIEW')
   if len(records)!=count:raise IOError('Workbench view row count mismatch')
   for index,record in zip(range(row,row+count),records):
    fields=_record({'output':record},'WB-VIEW',('row','text','attr'))
    text=_ascii(fields['text'],80);attributes=_hex(fields['attr'],80,False)
    if _unsigned(fields['row'],59)!=index or len(text)!=80 or len(attributes)!=80 or any(attr&0xcc for attr in attributes):raise IOError('Invalid workbench view row')
    rows.append({'row':index,'text':text,'attributes':list(attributes)})
   last=self.wb_status();outputs.append(last.get('output',''))
   if not last.get('ok'):last['output']=''.join(outputs);return last
   if first['workbench']!=last['workbench']:raise IOError('Workbench view changed during read')
   first.update(rows=rows,output=''.join(outputs));return first

 def wb_flush(self):
  result=self._wb('WB-FLUSH',False)
  if result.get('ok'):
   fields=_record(result,'WB-FLUSH',('elapsed_ms','bytes','spans','batches','cells','pending'))
   result['render']={key:_unsigned(value,1 if key=='pending' else 0xffffffff) for key,value in fields.items()}
  return result
 def wb_render_stats(self,reset=False):
  reset=_boolean(reset);result=self._wb('WB-RENDER-STATS'+(' RESET' if reset else ''),False)
  if result.get('ok'):
   fields=_record(result,'WB-RENDER-STATS',('cells','spans','batches','bytes','elapsed_ticks','pending'))
   result['render']={key:_unsigned(value,1 if key=='pending' else 0xffffffff) for key,value in fields.items()}
  return result
