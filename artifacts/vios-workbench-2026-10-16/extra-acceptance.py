import json,re,time
from pathlib import Path
from tools.vios_cli import RPCClient
root=Path.cwd();out=root/'docs/bring-up/vios-workbench-extra-acceptance.json'
report={'status':'running','started_unix':time.time(),'checks':[]}
def save():out.write_text(json.dumps(report,indent=2)+'\n')
def check(name,result,condition=True):
 report['checks'].append({'name':name,'result':result,'passed':bool(condition)})
 save()
 if not condition:raise AssertionError(name)
 return result
with RPCClient(timeout=180) as c:
 c.initialize()
 def tool(name,args=None):
  r=c.tool(name,args)
  if not r.get('ok',True) or r.get('pending') or r.get('compiling'):raise RuntimeError((name,r))
  return r
 try:
  names=[x['name'] for x in c.request('tools/list')['tools']]
  check('typed_workbench_tools',names,len([x for x in names if x.startswith('wb_')])==19)
  initial=tool('boot_status')['boot'];check('boot_selection_before',initial,initial['confirmed']=='A' and initial['state']=='CLEAN')
  state=tool('wb_status')['workbench'];check('owned_fixture',state,state['module']=='WBM1P')
  state=tool('wb_edit',{'module':'WBM1P','discard':True})['workbench']
  source=tool('wb_source');check('typed_exact_source',source,source['source'].startswith(': WBM1P-VALUE 18 ;\n'))
  saved=tool('wb_save',{'revision':state['revision']});check('guarded_save',saved,saved['workbench']['saved'])
  # The native guarded WB save is silent; existing SAVE reports its verified
  # immutable path and is idempotent for the same bytes.
  located=tool('module_save',{'name':'WBM1P'})
  paths=re.findall(r'(?:^|\r\n)saved (\S+)',located['output']);check('saved_package_path',paths,len(paths)==1);report['saved_package']=paths[0]
  tool('wb_open');tool('wb_select',{'kind':'MODULE','value':'WBM1P'});tool('wb_flush')
  samples=[]
  for _ in range(9):
   r=tool('evaluate',{'source':'TIMER@ WB-VIEW 0 0 TIMER@ - $FFFF AND . CR'})
   ticks=[int(n) for n in re.findall(r'(?:^|\r\n)(\d+) \r\n',r['output'])]
   if len(ticks)!=1:raise AssertionError(r)
   samples.append(ticks[0])
  check('compose_ticks_16bit_interval',samples,max(samples)<60000);report['compose_timer_hz']=288000
  r=tool('evaluate',{'source':'MILLIS2 WB-VIEW 0 0 MILLIS2 . . . . CR'})
  data=re.findall(r'(?:^|\r\n)((?:\d+ ){4})\r\n',r['output'])
  if len(data)!=1:raise AssertionError(r)
  after_hi,after_lo,before_hi,before_lo=map(int,data[0].split())
  elapsed=((after_hi<<24|after_lo)-(before_hi<<24|before_lo))&0xffffffff
  check('compose_32bit_clock_crosscheck',{'elapsed_ms':elapsed,'result':r},0<elapsed<227)
  bench=tool('benchmark');check('vm_benchmark_while_workbench_open',bench,'BENCH DONE' in bench['output']);report['vm_benchmark']=bench
  trials=[]
  source=('12345 . CR \\ ').ljust(235,'x')
  for i in range(50):
   tool('wb_redraw')
   r=tool('evaluate',{'source':source,'timeout':30})
   if '\r\n12345 \r\n' not in r['output']:raise AssertionError((i,r))
   trials.append({'trial':i,'ok':True})
  tool('wb_flush');r=tool('evaluate',{'source':'VDP-INFO'})
  check('50_maximum_source_requests_during_repaints',{'trials':trials,'vdp':r},'console-dropped 0' in r['output'])
  # Exercise execution tokens well beyond a one-byte dictionary index.
  definitions=[f': WBCAP{i:03d} {i} ;' for i in range(300)]
  lines=[];line=''
  for word in definitions:
   if len(line)+len(word)+1>235:lines.append(line);line=''
   line+=word+' '
  if line:lines.append(line)
  for line in lines:tool('evaluate',{'source':line})
  resources=tool('resources');check('hardware_dictionary_over_255',resources,int(resources['resources']['words'].split('/')[0])>350)
  for engine in ('C','TOKEN','DIRECT'):
   r=tool('evaluate',{'source':f"ENGINE {engine} ' WBCAP299 EXECUTE . CR"})
   check('high_xt_'+engine,r,'\r\n299 \r\n' in r['output'])
  words=tool('wb_browse',{'browser':'WORDS'});check('typed_large_word_browser',{'count':len(words['items'])},len(words['items'])>350)
  tool('wb_browse',{'browser':'MODULES'});tool('wb_select',{'kind':'MODULE','value':'WBM1P'})
  state=tool('wb_edit',{'module':'WBM1P','discard':True})['workbench']
  if not state['focus']:tool('wb_key',{'code':9})
  tool('wb_flush')
  view=tool('wb_view');check('typed_full_view',{'rows':len(view['rows'])},len(view['rows'])==60)
  (root/'docs/bring-up/vios-workbench-screen.json').write_text(json.dumps(view,indent=2)+'\n')
  final=tool('boot_status')['boot'];check('boot_selection_preserved',final,final==initial)
  info=tool('evaluate',{'source':'VDP-INFO LIVE1-C . LIVE1-D . CR'})
  check('original_startup_and_zero_drops',info,'console-dropped 0' in info['output'] and '11 12 ' in info['output'])
  report['status']='passed'
 except BaseException as e:
  report.update(status='failed',error=str(e));raise
 finally:report['finished_unix']=time.time();save()
print('extra acceptance',report['status'],len(report['checks']),'checks; package',report.get('saved_package'))
