import sys
import numpy as np

percentiles = [10, 25, 50, 90, 99, 99.9]

with open(sys.argv[1], 'r') as f:
    n = 0
    for l in f:
        n += 1
        times = [int(x) for x in l.split()]
        min_time = min(times)
        p50 = np.percentile(times, 50)

        print(
                n,
                min_time,
                p50,
                min_time / p50,
                #np.mean(times),
                #np.std(times),
                #' '.join([str(np.percentile(times, p)) for p in percentiles])
                )
