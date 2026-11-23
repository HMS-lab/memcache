import argparse, contextlib, importlib.util, json, pathlib, time, sys
root = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('allocator_sim', root.parent / 'allocator_sim.py')
sim = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = sim
spec.loader.exec_module(sim)
p = argparse.ArgumentParser()
p.add_argument('--steps', type=int, default=200000)
p.add_argument('--seeds', type=int, nargs='+', default=[17, 42, 123])
p.add_argument('--capacity', default='1GiB')
p.add_argument('--output', type=pathlib.Path)
a = p.parse_args()
if a.output:
 root = a.output.resolve()
 root.mkdir(parents=True, exist_ok=True)
scenarios = {'kv_uniform':'1MiB:100%', 'kv_ten_sizes':'64KiB,128KiB,256KiB,512KiB,768KiB,1MiB,1536KiB,2MiB,4MiB,8MiB'}
rows=[]
for name, sizes in scenarios.items():
 for seed in a.seeds:
  for mode in ('4k-align','slab'):
   args=argparse.Namespace(capacity=a.capacity,block_sizes=sizes,steps=a.steps,mode=mode,eviction_policy='lru',max_water_level=90,min_water_level=80,hot_block_percent=20,hot_read_percent=80,reads_per_allocation=4,sample_every=a.steps,seed=seed,workload='water-level')
   with (root/f'{name}_{seed}_{mode}.log').open('w') as log, contextlib.redirect_stdout(log):
    start=time.perf_counter(); report=sim.run_simulation(args); elapsed=time.perf_counter()-start
   report['elapsed_seconds_including_build']=elapsed
   (root/f'{name}_{seed}_{mode}.json').write_text(json.dumps(report,indent=2))
   rows.append(dict(scenario=name,seed=seed,mode=mode,seconds=elapsed,**report['counters'],**report['memory']))
   (root/'summary.json').write_text(json.dumps(rows,indent=2))
   print(name,seed,mode,report['counters']['successes'],round(elapsed,2),flush=True)
