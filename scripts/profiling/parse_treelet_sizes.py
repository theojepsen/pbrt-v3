import sys
import os

scene_dir = sys.argv[1]

treelets = {}

for fn in os.listdir(scene_dir):
    full_fn = os.path.join(scene_dir, fn)
    if not os.path.isfile(full_fn) or fn[0] != "T": continue
    treelet_id = int(fn[1:])
    size = os.path.getsize(full_fn)
    treelets[treelet_id] = size

for treelet_id,size in sorted(treelets.items(), key=lambda x: x[0]):
    print(treelet_id, size)
