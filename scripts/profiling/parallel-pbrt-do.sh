#!/bin/zsh

scene_name=$1

JOBS=10
TOTAL_PATHS=1400000

paths_per_job=$(( TOTAL_PATHS / JOBS ))

for i in $(seq $JOBS)
do
    i=$(( i - 1 ))
    start_path=$(( i * paths_per_job ))
    end_path=$(( start_path + paths_per_job - 1))
    echo job $i tracing paths $start_path to $end_path
    time ./pbrt-do "$scene_name" "$scene_name"_0.rays job"$i"_ $start_path $end_path &
done
