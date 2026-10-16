#!/usr/bin/env python3
"""Validate/scaling-check hardware timer samples and produce a compact report."""
import argparse,json,re,statistics
from pathlib import Path

def analyze(text):
 rows=[]
 for match in re.finditer(r'SAMPLE engine=(\d+) word=(\S+) n=(\d+) result=(\d+) ticks=([0-9,]+) median=(\d+)',text):
  engine,word,n,result,raw,median=match.groups();samples=[int(x) for x in raw.split(',')]
  if len(samples)!=9 or int(statistics.median(samples))!=int(median):raise ValueError('invalid sample count/median')
  if not 0<max(samples)<32768:raise ValueError('sample too close to timer wrap or zero')
  rows.append({'engine':int(engine),'word':word,'n':int(n),'result':int(result),'ticks':samples,'median':int(median)})
 if len(rows)!=45:raise ValueError(f'expected45 conditions, got{len(rows)}')
 by={(r['engine'],r['word'],r['n']):r for r in rows};summaries=[]
 for word in sorted({r['word'] for r in rows}):
  for engine in range(3):
   a,b,c=(by[engine,word,n] for n in [25,50,100])
   ratio=(c['median']-b['median'])/(b['median']-a['median'])
   if not 1.9<ratio<2.1:raise ValueError(f'{word} engine{engine} fails affine timing scaling: {ratio}')
  c,t,d=(by[e,word,100] for e in range(3))
  if len({r['result'] for r in [c,t,d]})!=1:raise ValueError('engine result mismatch')
  summaries.append({'word':word,'c_ticks':c['median'],'token_ticks':t['median'],'direct_ticks':d['median'],'token_ms':t['median']/288,'direct_ms':d['median']/288,'direct_time_reduction_percent':100*(1-d['median']/t['median']),'direct_vs_c_speedup':c['median']/d['median']})
 return {'timer_hz':288000,'samples_per_condition':9,'conditions':45,'largest_sample_ticks':max(max(r['ticks']) for r in rows),'all_scaling_checks_passed':True,'workloads':summaries,'raw':rows}

def main():
 p=argparse.ArgumentParser();p.add_argument('input',type=Path);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
 data=analyze(args.input.read_text());args.output.write_text(json.dumps(data,indent=2)+'\n')
 for r in data['workloads']:print(r['word'],f"token{r['token_ms']:.3f}ms direct{r['direct_ms']:.3f}ms reduction{r['direct_time_reduction_percent']:.1f}% vsC{r['direct_vs_c_speedup']:.2f}x")
 print('All45 conditions, medians, results and25/50/100 scaling checks passed.')
if __name__=='__main__':main()
