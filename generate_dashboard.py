#!/usr/bin/env python3
"""
AdaptiveSched++ Dashboard Generator
Reads output/benchmark_results.json and writes output/adaptivesched_dashboard.html
"""
import json, os, statistics

BASE = os.path.dirname(os.path.abspath(__file__))
JSON_PATH = os.path.join(BASE, "output", "benchmark_results.json")
OUT_PATH  = os.path.join(BASE, "output", "adaptivesched_dashboard.html")

if not os.path.exists(JSON_PATH):
    print("ERROR: output/benchmark_results.json not found.")
    print("Run:  python3 benchmark.py --count 500   first.")
    exit(1)

with open(JSON_PATH) as f:
    d = json.load(f)

recs_a = d['runs']['adaptive']['records']
recs_m = d['runs']['mlfq_fixed']['records']
sum_a  = d['runs']['adaptive']['summary']
sum_m  = d['runs']['mlfq_fixed']['summary']
pb     = d['runs']['adaptive']['policy_breakdown']
n      = d['n_processes']

def fld(recs, key): return [float(r[key]) for r in recs]

def bucket(vals, n_bins=40):
    lo, hi = min(vals), max(vals)
    w = (hi - lo) / n_bins or 1
    counts = [0]*n_bins
    for v in vals:
        idx = min(int((v-lo)/w), n_bins-1)
        counts[idx] += 1
    centers = [lo + (i+0.5)*w for i in range(n_bins)]
    return centers, counts

def jain(vals):
    n = len(vals)
    if n < 2: return 1.0
    s = sum(vals); sq = sum(v*v for v in vals)
    return (s*s)/(n*sq) if sq > 1e-9 else 1.0

by_pid_a = {r['pid']: r for r in recs_a}
by_pid_m = {r['pid']: r for r in recs_m}

wt_a_x, wt_a_y = bucket(fld(recs_a,'waiting'))
wt_m_x, wt_m_y = bucket(fld(recs_m,'waiting'))
rt_a_x, rt_a_y = bucket(fld(recs_a,'response'))
rt_m_x, rt_m_y = bucket(fld(recs_m,'response'))

common = sorted(set(by_pid_a.keys()) & set(by_pid_m.keys()))
delta  = [float(by_pid_m[p]['waiting']) - float(by_pid_a[p]['waiting']) for p in common]
delta_sorted = sorted(delta)
n_better_adapt = sum(1 for x in delta_sorted if x > 0)
n_better_mlfq  = sum(1 for x in delta_sorted if x < 0)

int_pids = [p for p in common if by_pid_a[p]['interactive']=='1']
cpu_pids = [p for p in common if by_pid_a[p]['interactive']=='0']
int_rt_a = [float(by_pid_a[p]['response']) for p in int_pids]
int_rt_m = [float(by_pid_m[p]['response']) for p in int_pids if p in by_pid_m]
cpu_rt_a = [float(by_pid_a[p]['response']) for p in cpu_pids]
cpu_rt_m = [float(by_pid_m[p]['response']) for p in cpu_pids if p in by_pid_m]
int_wt_a = [float(by_pid_a[p]['waiting']) for p in int_pids]
int_wt_m = [float(by_pid_m[p]['waiting']) for p in int_pids if p in by_pid_m]

comp_a_t = sorted([int(r['completion']) for r in recs_a])
comp_m_t = sorted([int(r['completion']) for r in recs_m])
cum_a = list(range(1, len(comp_a_t)+1))
cum_m = list(range(1, len(comp_m_t)+1))

ctx_a = sum(int(r['ctx_switches']) for r in recs_a)
ctx_m = sum(int(r['ctx_switches']) for r in recs_m)
pol_names  = list(pb.keys())
pol_counts = [pb[k]['count'] for k in pol_names]

chart_data = dict(
    metrics=['Avg Wait','Avg TAT','Avg Response','p95 Wait','p99 Wait'],
    adapt_vals=[sum_a['avg_waiting_time'],sum_a['avg_turnaround_time'],
                sum_a['avg_response_time'],sum_a['p95_waiting'],sum_a['p99_waiting']],
    mlfq_vals=[sum_m['avg_waiting_time'],sum_m['avg_turnaround_time'],
               sum_m['avg_response_time'],sum_m['p95_waiting'],sum_m['p99_waiting']],
    wt_a_x=wt_a_x,wt_a_y=wt_a_y,wt_m_x=wt_m_x,wt_m_y=wt_m_y,
    rt_a_x=rt_a_x,rt_a_y=rt_a_y,rt_m_x=rt_m_x,rt_m_y=rt_m_y,
    burst_a=fld(recs_a,'burst'),wait_a=fld(recs_a,'waiting'),
    inter_a=[r['interactive'] for r in recs_a],
    burst_m=fld(recs_m,'burst'),wait_m=fld(recs_m,'waiting'),
    inter_m=[r['interactive'] for r in recs_m],
    delta_sorted=delta_sorted,
    comp_a_t=comp_a_t,cum_a=cum_a,comp_m_t=comp_m_t,cum_m=cum_m,
    ctx_a=ctx_a,ctx_m=ctx_m,
    fair_a=round(jain(fld(recs_a,'waiting')),4),
    fair_m=round(jain(fld(recs_m,'waiting')),4),
    pol_names=pol_names,pol_counts=pol_counts,
    n_processes=n,
    n_better_adapt=n_better_adapt,n_better_mlfq=n_better_mlfq,
    int_rt_a=int_rt_a,int_rt_m=int_rt_m,
    cpu_rt_a=cpu_rt_a,cpu_rt_m=cpu_rt_m,
    int_wt_a=int_wt_a,int_wt_m=int_wt_m,
    sum_a=sum_a,sum_m=sum_m,
)

DATA_JS = f"const D = {json.dumps(chart_data)};"

HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>AdaptiveSched++ · Benchmark Dashboard</title>
<script src="https://cdn.plot.ly/plotly-2.27.0.min.js"></script>
<link href="https://fonts.googleapis.com/css2?family=DM+Mono:wght@300;400;500&family=Space+Grotesk:wght@400;500;700&family=Syne:wght@700;800&display=swap" rel="stylesheet"/>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0a0c10;--surface:#111318;--surface2:#181c22;--border:#242830;
  --accent:#00e5ff;--accent2:#ff6b6b;--accent3:#a8ff78;--accent4:#ffbe76;
  --text:#e8eaf0;--muted:#6b7280;--adapt:#00e5ff;--mlfq:#ff6b6b;
}
body{background:var(--bg);color:var(--text);font-family:'Space Grotesk',sans-serif;min-height:100vh;overflow-x:hidden;}
body::before{content:'';position:fixed;inset:0;
  background-image:linear-gradient(rgba(0,229,255,0.03) 1px,transparent 1px),
    linear-gradient(90deg,rgba(0,229,255,0.03) 1px,transparent 1px);
  background-size:40px 40px;pointer-events:none;z-index:0;}
.page{position:relative;z-index:1;max-width:1400px;margin:0 auto;padding:0 24px 80px;}
header{padding:56px 0 40px;border-bottom:1px solid var(--border);margin-bottom:40px;}
.tag{display:inline-block;font-family:'DM Mono',monospace;font-size:11px;letter-spacing:.12em;
     text-transform:uppercase;color:var(--accent);border:1px solid var(--accent);padding:3px 10px;
     border-radius:2px;margin-bottom:18px;background:rgba(0,229,255,0.06);}
h1{font-family:'Syne',sans-serif;font-size:clamp(32px,5vw,64px);font-weight:800;
   line-height:1.05;letter-spacing:-0.02em;color:#fff;margin-bottom:12px;}
h1 span{color:var(--accent);}
.subtitle{color:var(--muted);font-size:16px;max-width:620px;line-height:1.6;}
.kpi-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:16px;margin-bottom:40px;}
.kpi{background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:20px 22px;
     position:relative;overflow:hidden;transition:border-color .2s,transform .2s;}
.kpi:hover{border-color:var(--accent);transform:translateY(-2px);}
.kpi::before{content:'';position:absolute;inset:0;
  background:linear-gradient(135deg,rgba(0,229,255,0.06),transparent 60%);}
.kpi-label{font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);
           font-family:'DM Mono',monospace;margin-bottom:8px;}
.kpi-val{font-family:'Syne',sans-serif;font-size:28px;font-weight:800;color:#fff;}
.kpi-sub{font-size:12px;color:var(--muted);margin-top:4px;}
.kpi-delta{font-size:12px;font-family:'DM Mono',monospace;margin-top:6px;}
.kpi-delta.pos{color:var(--accent3);}
.kpi-delta.neg{color:var(--accent2);}
.section{margin-bottom:48px;}
.section-title{font-family:'Syne',sans-serif;font-size:20px;font-weight:700;color:#fff;
               margin-bottom:8px;display:flex;align-items:center;gap:10px;}
.section-title .dot{width:8px;height:8px;border-radius:50%;background:var(--accent);
                    box-shadow:0 0 8px var(--accent);}
.section-desc{color:var(--muted);font-size:14px;margin-bottom:20px;max-width:700px;}
.chart-card{background:var(--surface);border:1px solid var(--border);border-radius:10px;
            padding:24px;overflow:hidden;transition:border-color .3s;}
.chart-card:hover{border-color:#303640;}
.chart-title{font-size:13px;font-family:'DM Mono',monospace;letter-spacing:.08em;
             text-transform:uppercase;color:var(--muted);margin-bottom:16px;}
.grid-2{display:grid;grid-template-columns:repeat(2,1fr);gap:20px;}
.grid-3{display:grid;grid-template-columns:repeat(3,1fr);gap:20px;}
@media(max-width:900px){.grid-2,.grid-3{grid-template-columns:1fr;}}
.legend{display:flex;gap:16px;margin-bottom:12px;flex-wrap:wrap;}
.pill{display:flex;align-items:center;gap:6px;font-size:12px;font-family:'DM Mono',monospace;}
.pill-dot{width:10px;height:10px;border-radius:50%;}
.adapt-col{background:var(--adapt);}
.mlfq-col{background:var(--mlfq);}
.insight{background:rgba(0,229,255,0.06);border:1px solid rgba(0,229,255,0.2);
         border-radius:8px;padding:16px 20px;margin-top:20px;font-size:13px;
         color:var(--text);line-height:1.7;}
.insight strong{color:var(--accent);}
footer{border-top:1px solid var(--border);padding-top:24px;font-family:'DM Mono',monospace;
       font-size:12px;color:var(--muted);display:flex;justify-content:space-between;
       align-items:center;flex-wrap:wrap;gap:12px;}
.data-table{width:100%;border-collapse:collapse;font-size:13px;font-family:'DM Mono',monospace;}
.data-table th{padding:8px 12px;text-align:left;color:var(--muted);border-bottom:1px solid var(--border);
               font-weight:400;font-size:11px;text-transform:uppercase;letter-spacing:.08em;}
.data-table td{padding:10px 12px;border-bottom:1px solid rgba(255,255,255,0.04);}
.data-table tr:hover td{background:rgba(255,255,255,0.02);}
.val-adapt{color:var(--adapt);}
.val-mlfq{color:var(--mlfq);}
.val-win{color:var(--accent3);font-weight:500;}
</style>
</head>
<body>
<div class="page">
<header>
  <div class="tag">Performance Analysis Report</div>
  <h1>AdaptiveSched<span>++</span><br>Benchmark Dashboard</h1>
  <p class="subtitle">Comparing Adaptive meta-scheduler vs MLFQ-fixed across
    <strong id="hdr-n"></strong> processes with heterogeneous burst profiles.</p>
</header>

<div class="kpi-row" id="kpi-row"></div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Summary Metrics Comparison</div>
  <p class="section-desc">Side-by-side grouped bar chart of five key scheduling metrics. Lower is better for all.</p>
  <div class="legend">
    <div class="pill"><div class="pill-dot adapt-col"></div>Adaptive</div>
    <div class="pill"><div class="pill-dot mlfq-col"></div>MLFQ Fixed</div>
  </div>
  <div class="chart-card"><div id="chart-summary" style="height:380px"></div></div>
</div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Waiting &amp; Response Time Distributions</div>
  <p class="section-desc">Histogram of per-process times. Tight clusters = consistent service; wide spreads = high variance.</p>
  <div class="grid-2">
    <div class="chart-card">
      <div class="chart-title">Waiting Time Distribution</div>
      <div class="legend">
        <div class="pill"><div class="pill-dot adapt-col"></div>Adaptive</div>
        <div class="pill"><div class="pill-dot mlfq-col"></div>MLFQ Fixed</div>
      </div>
      <div id="chart-wt-dist" style="height:300px"></div>
    </div>
    <div class="chart-card">
      <div class="chart-title">Response Time Distribution</div>
      <div class="legend">
        <div class="pill"><div class="pill-dot adapt-col"></div>Adaptive</div>
        <div class="pill"><div class="pill-dot mlfq-col"></div>MLFQ Fixed</div>
      </div>
      <div id="chart-rt-dist" style="height:300px"></div>
    </div>
  </div>
</div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Interactive vs CPU-bound Breakdown</div>
  <p class="section-desc">Box plots split by process type. A good adaptive scheduler minimises response time for interactive and wait for CPU-bound.</p>
  <div class="grid-2">
    <div class="chart-card">
      <div class="chart-title">Response Time by Process Type</div>
      <div id="chart-type-rt" style="height:320px"></div>
    </div>
    <div class="chart-card">
      <div class="chart-title">Waiting Time — Interactive Processes</div>
      <div id="chart-type-wt" style="height:320px"></div>
    </div>
  </div>
</div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Per-Process Waiting Time Δ (MLFQ − Adaptive)</div>
  <p class="section-desc">Cyan bars = Adaptive wins (lower wait). Red bars = MLFQ wins. Sorted ascending.</p>
  <div class="chart-card"><div id="chart-delta" style="height:340px"></div></div>
  <div class="insight" id="delta-insight"></div>
</div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Cumulative Throughput Over Time</div>
  <p class="section-desc">Processes completed vs simulation tick. Steeper = higher throughput.</p>
  <div class="chart-card"><div id="chart-throughput" style="height:360px"></div></div>
</div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Policy Usage &amp; Context Switches</div>
  <div class="grid-2">
    <div class="chart-card">
      <div class="chart-title">Policy Distribution within Adaptive Run</div>
      <div id="chart-pie" style="height:300px"></div>
    </div>
    <div class="chart-card">
      <div class="chart-title">Total Context Switches</div>
      <div id="chart-ctx" style="height:300px"></div>
    </div>
  </div>
</div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Burst vs Waiting Time Scatter</div>
  <p class="section-desc">Each dot is one process. Red = interactive, teal = CPU-bound.</p>
  <div class="grid-2">
    <div class="chart-card">
      <div class="chart-title">Adaptive Scheduler</div>
      <div id="chart-scatter-a" style="height:340px"></div>
    </div>
    <div class="chart-card">
      <div class="chart-title">MLFQ Fixed</div>
      <div id="chart-scatter-m" style="height:340px"></div>
    </div>
  </div>
</div>

<div class="section">
  <div class="section-title"><span class="dot"></span>Full Metrics Summary Table</div>
  <div class="chart-card"><table class="data-table" id="summary-table"></table></div>
</div>

<footer>
  <span>AdaptiveSched++ · CPU Scheduling Research Tool</span>
  <span id="footer-info"></span>
</footer>
</div>

<script>
""" + DATA_JS + """
const LAYOUT_BASE={
  paper_bgcolor:'transparent',plot_bgcolor:'transparent',
  font:{family:"'DM Mono',monospace",color:'#6b7280',size:11},
  margin:{t:20,r:20,b:40,l:60},
  xaxis:{gridcolor:'#1e222a',linecolor:'#242830',tickfont:{size:10}},
  yaxis:{gridcolor:'#1e222a',linecolor:'#242830',tickfont:{size:10}},
  legend:{bgcolor:'transparent',font:{size:11}},
  hoverlabel:{bgcolor:'#1a1e26',bordercolor:'#303640',
    font:{family:"'DM Mono',monospace",color:'#e8eaf0'}},
};
const CFG={responsive:true,displayModeBar:false};
const AC='#00e5ff',MC='#ff6b6b';
function L(o={}){return Object.assign({},LAYOUT_BASE,o);}

document.getElementById('hdr-n').textContent=D.n_processes.toLocaleString();
document.getElementById('footer-info').textContent=
  D.n_processes+' processes · AdaptiveSched++ Benchmark';

// KPI strip
const kpis=[
  {label:'Avg Waiting Time',av:D.sum_a.avg_waiting_time.toFixed(1)+' ticks',
   mv:D.sum_m.avg_waiting_time.toFixed(1)+' ticks',
   d:((D.sum_a.avg_waiting_time-D.sum_m.avg_waiting_time)/D.sum_m.avg_waiting_time*100).toFixed(1),lower:true},
  {label:'Avg Response Time',av:D.sum_a.avg_response_time.toFixed(1)+' ticks',
   mv:D.sum_m.avg_response_time.toFixed(1)+' ticks',
   d:((D.sum_a.avg_response_time-D.sum_m.avg_response_time)/D.sum_m.avg_response_time*100).toFixed(1),lower:true},
  {label:"Jain's Fairness",av:D.fair_a.toFixed(4),mv:D.fair_m.toFixed(4),
   d:((D.fair_a-D.fair_m)/D.fair_m*100).toFixed(1),lower:false},
  {label:'p95 Waiting',av:D.sum_a.p95_waiting+' ticks',mv:D.sum_m.p95_waiting+' ticks',
   d:((D.sum_a.p95_waiting-D.sum_m.p95_waiting)/D.sum_m.p95_waiting*100).toFixed(1),lower:true},
  {label:'Context Switches',av:D.ctx_a.toLocaleString(),mv:D.ctx_m.toLocaleString(),
   d:((D.ctx_a-D.ctx_m)/(D.ctx_m||1)*100).toFixed(1),lower:true},
  {label:'Interactive Avg RT',
   av:(D.sum_a.interactive_avg_rt||0).toFixed(1)+' ticks',
   mv:(D.sum_m.interactive_avg_rt||0).toFixed(1)+' ticks',
   d:(((D.sum_a.interactive_avg_rt||0)-(D.sum_m.interactive_avg_rt||1))/(D.sum_m.interactive_avg_rt||1)*100).toFixed(1),
   lower:true},
];
const kr=document.getElementById('kpi-row');
kpis.forEach(k=>{
  const dv=parseFloat(k.d);
  const win=k.lower ? dv<0 : dv>0;
  const cls=win?'pos':'neg'; const arr=win?'▲':'▼';
  kr.innerHTML+=`<div class="kpi"><div class="kpi-label">${k.label}</div>
    <div class="kpi-val" style="color:${AC}">${k.av}</div>
    <div class="kpi-sub">MLFQ: ${k.mv}</div>
    <div class="kpi-delta ${cls}">${arr} ${Math.abs(dv)}% vs MLFQ</div></div>`;
});

// Summary bars
Plotly.newPlot('chart-summary',[
  {name:'Adaptive',x:D.metrics,y:D.adapt_vals,type:'bar',
   marker:{color:AC,opacity:0.85},
   hovertemplate:'<b>%{x}</b><br>Adaptive: %{y:.1f}<extra></extra>'},
  {name:'MLFQ Fixed',x:D.metrics,y:D.mlfq_vals,type:'bar',
   marker:{color:MC,opacity:0.85},
   hovertemplate:'<b>%{x}</b><br>MLFQ: %{y:.1f}<extra></extra>'},
],L({barmode:'group',margin:{t:20,r:20,b:50,l:70}}),CFG);

// WT dist
Plotly.newPlot('chart-wt-dist',[
  {name:'Adaptive',x:D.wt_a_x,y:D.wt_a_y,type:'bar',marker:{color:AC,opacity:0.7},
   hovertemplate:'WT≈%{x:.0f}: %{y}<extra></extra>'},
  {name:'MLFQ',x:D.wt_m_x,y:D.wt_m_y,type:'bar',marker:{color:MC,opacity:0.7},
   hovertemplate:'WT≈%{x:.0f}: %{y}<extra></extra>'},
],L({barmode:'overlay',margin:{t:10,r:20,b:50,l:60}}),CFG);

// RT dist
Plotly.newPlot('chart-rt-dist',[
  {name:'Adaptive',x:D.rt_a_x,y:D.rt_a_y,type:'bar',marker:{color:AC,opacity:0.7},
   hovertemplate:'RT≈%{x:.0f}: %{y}<extra></extra>'},
  {name:'MLFQ',x:D.rt_m_x,y:D.rt_m_y,type:'bar',marker:{color:MC,opacity:0.7},
   hovertemplate:'RT≈%{x:.0f}: %{y}<extra></extra>'},
],L({barmode:'overlay',margin:{t:10,r:20,b:50,l:60}}),CFG);

// Box plots
Plotly.newPlot('chart-type-rt',[
  {type:'box',name:'Interactive/Adaptive',y:D.int_rt_a,marker:{color:AC},line:{color:AC},boxmean:true},
  {type:'box',name:'Interactive/MLFQ',y:D.int_rt_m,marker:{color:MC},line:{color:MC},boxmean:true},
  {type:'box',name:'CPU-bound/Adaptive',y:D.cpu_rt_a,marker:{color:'#a8ff78'},line:{color:'#a8ff78'},boxmean:true},
  {type:'box',name:'CPU-bound/MLFQ',y:D.cpu_rt_m,marker:{color:'#ffbe76'},line:{color:'#ffbe76'},boxmean:true},
],L({margin:{t:10,r:20,b:50,l:70},showlegend:true,legend:{font:{size:10}}}),CFG);

Plotly.newPlot('chart-type-wt',[
  {type:'box',name:'Interactive/Adaptive',y:D.int_wt_a,marker:{color:AC},line:{color:AC},boxmean:true},
  {type:'box',name:'Interactive/MLFQ',y:D.int_wt_m,marker:{color:MC},line:{color:MC},boxmean:true},
],L({margin:{t:10,r:20,b:50,l:70}}),CFG);

// Delta
const dColors=D.delta_sorted.map(v=>v>=0?AC:MC);
Plotly.newPlot('chart-delta',[
  {x:D.delta_sorted.map((_,i)=>i),y:D.delta_sorted,type:'bar',
   marker:{color:dColors,opacity:0.75},
   hovertemplate:'Process #%{x}<br>WT saved: %{y:.0f} ticks<extra></extra>'},
],L({
  xaxis:{...LAYOUT_BASE.xaxis,title:{text:'Processes (sorted)',font:{size:11}},showticklabels:false},
  yaxis:{...LAYOUT_BASE.yaxis,title:{text:'WT Saved (+ = Adaptive wins)',font:{size:11}},
    zeroline:true,zerolinecolor:'#303640',zerolinewidth:2},
  margin:{t:20,r:20,b:50,l:80},
}),CFG);
document.getElementById('delta-insight').innerHTML=
  `Adaptive outperforms MLFQ for <strong>${D.n_better_adapt}</strong> of ${D.n_processes} processes
   (${(D.n_better_adapt/D.n_processes*100).toFixed(1)}%).
   MLFQ wins for <strong>${D.n_better_mlfq}</strong> (${(D.n_better_mlfq/D.n_processes*100).toFixed(1)}%).
   Jain Fairness — Adaptive: <strong>${D.fair_a}</strong> · MLFQ: <strong>${D.fair_m}</strong>.`;

// Throughput
Plotly.newPlot('chart-throughput',[
  {name:'Adaptive',x:D.comp_a_t,y:D.cum_a,type:'scatter',mode:'lines',
   line:{color:AC,width:2.5},hovertemplate:'Tick %{x}<br>Completed: %{y}<extra></extra>'},
  {name:'MLFQ Fixed',x:D.comp_m_t,y:D.cum_m,type:'scatter',mode:'lines',
   line:{color:MC,width:2.5,dash:'dot'},hovertemplate:'Tick %{x}<br>Completed: %{y}<extra></extra>'},
],L({margin:{t:20,r:20,b:50,l:70}}),CFG);

// Pie
Plotly.newPlot('chart-pie',[
  {type:'pie',labels:D.pol_names,values:D.pol_counts,
   marker:{colors:['#00e5ff','#ff6b6b','#a8ff78','#ffbe76'],
     line:{color:'#0a0c10',width:2}},
   textfont:{color:'#fff',size:12},
   hovertemplate:'%{label}: %{value} processes (%{percent})<extra></extra>'},
],{...L({margin:{t:20,r:20,b:20,l:20}}),showlegend:true},CFG);

// CTX
Plotly.newPlot('chart-ctx',[
  {x:['Adaptive','MLFQ Fixed'],y:[D.ctx_a,D.ctx_m],type:'bar',
   marker:{color:[AC,MC],opacity:0.85},
   hovertemplate:'%{x}: %{y:,} ctx switches<extra></extra>'},
],L({margin:{t:20,r:20,b:50,l:80},showlegend:false}),CFG);

// Scatter
function scatter(burst,wait,inter,divId){
  const ix=[],iy=[],cx=[],cy=[];
  burst.forEach((b,i)=>{
    if(inter[i]==='1'){ix.push(b);iy.push(wait[i]);}
    else{cx.push(b);cy.push(wait[i]);}
  });
  Plotly.newPlot(divId,[
    {name:'Interactive',x:ix,y:iy,mode:'markers',type:'scatter',
     marker:{color:MC,size:5,opacity:0.6}},
    {name:'CPU-bound',x:cx,y:cy,mode:'markers',type:'scatter',
     marker:{color:AC,size:5,opacity:0.5}},
  ],L({
    xaxis:{...LAYOUT_BASE.xaxis,title:{text:'Burst (ticks)',font:{size:11}}},
    yaxis:{...LAYOUT_BASE.yaxis,title:{text:'Waiting (ticks)',font:{size:11}}},
    margin:{t:10,r:20,b:50,l:70},
  }),CFG);
}
scatter(D.burst_a,D.wait_a,D.inter_a,'chart-scatter-a');
scatter(D.burst_m,D.wait_m,D.inter_m,'chart-scatter-m');

// Table
const tbl=document.getElementById('summary-table');
const rows=[
  ['Metric','Adaptive','MLFQ Fixed','Winner'],
  ['Avg Waiting Time',D.sum_a.avg_waiting_time.toFixed(2),D.sum_m.avg_waiting_time.toFixed(2),
   D.sum_a.avg_waiting_time<=D.sum_m.avg_waiting_time?'Adaptive ✓':'MLFQ ✓'],
  ['Avg Turnaround',D.sum_a.avg_turnaround_time.toFixed(2),D.sum_m.avg_turnaround_time.toFixed(2),
   D.sum_a.avg_turnaround_time<=D.sum_m.avg_turnaround_time?'Adaptive ✓':'MLFQ ✓'],
  ['Avg Response Time',D.sum_a.avg_response_time.toFixed(2),D.sum_m.avg_response_time.toFixed(2),
   D.sum_a.avg_response_time<=D.sum_m.avg_response_time?'Adaptive ✓':'MLFQ ✓'],
  ["Jain's Fairness",D.fair_a.toFixed(4),D.fair_m.toFixed(4),
   D.fair_a>=D.fair_m?'Adaptive ✓':'MLFQ ✓'],
  ['p95 Wait',D.sum_a.p95_waiting,D.sum_m.p95_waiting,
   D.sum_a.p95_waiting<=D.sum_m.p95_waiting?'Adaptive ✓':'MLFQ ✓'],
  ['p99 Wait',D.sum_a.p99_waiting,D.sum_m.p99_waiting,
   D.sum_a.p99_waiting<=D.sum_m.p99_waiting?'Adaptive ✓':'MLFQ ✓'],
  ['Context Switches',D.ctx_a.toLocaleString(),D.ctx_m.toLocaleString(),
   D.ctx_a<=D.ctx_m?'Adaptive ✓':'MLFQ ✓'],
  ['Interactive Avg RT',(D.sum_a.interactive_avg_rt||0).toFixed(2),
   (D.sum_m.interactive_avg_rt||0).toFixed(2),
   (D.sum_a.interactive_avg_rt||0)<=(D.sum_m.interactive_avg_rt||0)?'Adaptive ✓':'MLFQ ✓'],
  ['Total Processes','"""+str(n)+"""','"""+str(n)+"""','—'],
];
let h='<thead><tr>'+rows[0].map(c=>`<th>${c}</th>`).join('')+'</tr></thead><tbody>';
rows.slice(1).forEach(r=>{
  const aw=r[3].startsWith('Adaptive');
  h+=`<tr><td>${r[0]}</td>
    <td class="${aw?'val-win':'val-adapt'}">${r[1]}</td>
    <td class="${!aw&&r[3]!=='—'?'val-win':'val-mlfq'}">${r[2]}</td>
    <td class="val-win">${r[3]}</td></tr>`;
});
tbl.innerHTML=h+'</tbody>';
</script>
</body>
</html>"""

with open(OUT_PATH, 'w') as f:
    f.write(HTML)
print(f"Dashboard written → {OUT_PATH}")
