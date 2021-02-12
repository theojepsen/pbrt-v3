import sys
import numpy as np

fn = sys.argv[1]

with open(fn, 'r') as f:
    for l in f:
        data = np.fromstring(l, dtype=float, sep=' ')
        path_id,node_id,task_type = data[:3].astype(int)
        rows = data[3:].reshape(-1, 4)
        cycles = rows[:,0]
        instructions = rows[:,1]
        accesses = rows[:,2]
        misses = rows[:,3]
        ipc = np.divide(instructions, cycles, out=np.zeros_like(instructions), where=cycles!=0)
        miss_rate = np.divide(misses, accesses, out=np.zeros_like(misses), where=accesses!=0)
        print(
                path_id, node_id, task_type,
                int(np.percentile(accesses, 50)),
                int(np.percentile(accesses, 99)),
                np.percentile(miss_rate, 50),
                np.percentile(miss_rate, 99),
                np.percentile(ipc, 50),
                np.percentile(ipc, 99),
                )
