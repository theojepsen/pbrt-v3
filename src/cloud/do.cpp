#include <iostream>
#include <queue>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <chrono>

#include "accelerators/cloud.h"
#include "cloud/manager.h"
#include "pbrt/main.h"
#include "pbrt/raystate.h"
#include "messages/serialization.h"
#include "messages/utils.h"
#include "util/exception.h"

#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>

using namespace std;
using namespace pbrt;

void usage(const char *argv0) {
    cerr << argv0 << " SCENE-DATA CAMERA-RAYS OUT-PREFIX [START-PATHID END-PATHID]" << endl;
}

vector<shared_ptr<Light>> loadLights() {
    vector<shared_ptr<Light>> lights;
    auto reader = global::manager.GetReader(ObjectType::Lights);

    while (!reader->eof()) {
        protobuf::Light proto_light;
        reader->read(&proto_light);
        lights.push_back(move(light::from_protobuf(proto_light)));
    }

    return lights;
}

shared_ptr<Camera> loadCamera(vector<unique_ptr<Transform>> &transformCache) {
    auto reader = global::manager.GetReader(ObjectType::Camera);
    protobuf::Camera proto_camera;
    reader->read(&proto_camera);
    return camera::from_protobuf(proto_camera, transformCache);
}

shared_ptr<GlobalSampler> loadSampler() {
    auto reader = global::manager.GetReader(ObjectType::Sampler);
    protobuf::Sampler proto_sampler;
    reader->read(&proto_sampler);
    return sampler::from_protobuf(proto_sampler);
}

Scene loadFakeScene() {
    auto reader = global::manager.GetReader(ObjectType::Scene);
    protobuf::Scene proto_scene;
    reader->read(&proto_scene);
    return from_protobuf(proto_scene);
}

enum class Operation { Trace, Shade };

#define DO_PERF_STATS 0
#define DUMP_ALL_TIMING_SAMPLES 0
#define TIMING_SAMPLES_CNT (1 * 1000)
enum TaskType {
  TaskTypeTrace = 1,
  TaskTypeShade = 2
};

struct trace_edge {
  int nodeID;
  int prevNodeID;
  TaskType taskType;
  TreeletId treeletID;
  long int elapsed_ns;
  unsigned bvhNodesVisited;
};

#if DO_PERF_STATS
struct perf_sample {
  unsigned cycles;
  unsigned instructions;
  unsigned l1d_access;
  unsigned l1d_miss;
  unsigned ctx_switches;
  unsigned migrations;
};
struct task_desc {
  unsigned pathID;
  int nodeID;
  TaskType taskType;
};

struct read_format {
    uint64_t nr;
    struct {
        uint64_t value;
        uint64_t id;
    } values[];
};

long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
    int cpu, int group_fd, unsigned long flags) {
  int ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  return ret;
}
int perf_group_fd;
uint64_t perf_id_ref, perf_id_miss, perf_id_cycles, perf_id_inst;
uint64_t perf_id_ctx_switches, perf_id_migrations;
void perf_setup() {
  int fd;
  struct perf_event_attr pe;

  bzero(&pe, sizeof(struct perf_event_attr));
  pe.size = sizeof(struct perf_event_attr);
  //pe.type = PERF_TYPE_HARDWARE;
  //pe.config = PERF_COUNT_HW_CACHE_REFERENCES;
  pe.type = PERF_TYPE_HW_CACHE;
  pe.config = (PERF_COUNT_HW_CACHE_L1D) |
    (PERF_COUNT_HW_CACHE_OP_READ << 8) |
    (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
  pe.disabled = 1;
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;
  pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
  perf_group_fd = perf_event_open(&pe, 0, -1, -1, 0);
  if (perf_group_fd == -1) {
    fprintf(stderr, "Error opening leader %llx\n", pe.config);
    exit(EXIT_FAILURE);
  }
  ioctl(perf_group_fd, PERF_EVENT_IOC_ID, &perf_id_ref);

  bzero(&pe, sizeof(struct perf_event_attr));
  pe.size = sizeof(struct perf_event_attr);
  //pe.type = PERF_TYPE_HARDWARE;
  //pe.config = PERF_COUNT_HW_CACHE_MISSES;
  pe.type = PERF_TYPE_HW_CACHE;
  pe.config = (PERF_COUNT_HW_CACHE_L1D) |
    (PERF_COUNT_HW_CACHE_OP_READ << 8) |
    (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
  pe.disabled = 1;
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;
  pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
  fd = perf_event_open(&pe, 0, -1, perf_group_fd, 0);
  if (fd == -1) {
    fprintf(stderr, "Error opening second %llx\n", pe.config);
    exit(EXIT_FAILURE);
  }
  ioctl(fd, PERF_EVENT_IOC_ID, &perf_id_miss);

  bzero(&pe, sizeof(struct perf_event_attr));
  pe.type = PERF_TYPE_HARDWARE;
  pe.size = sizeof(struct perf_event_attr);
  pe.config = PERF_COUNT_HW_CPU_CYCLES;
  pe.disabled = 1;
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;
  pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
  fd = perf_event_open(&pe, 0, -1, perf_group_fd, 0);
  if (fd == -1) {
    fprintf(stderr, "Error opening third %llx\n", pe.config);
    exit(EXIT_FAILURE);
  }
  ioctl(fd, PERF_EVENT_IOC_ID, &perf_id_cycles);

  bzero(&pe, sizeof(struct perf_event_attr));
  pe.type = PERF_TYPE_HARDWARE;
  pe.size = sizeof(struct perf_event_attr);
  pe.config = PERF_COUNT_HW_INSTRUCTIONS;
  pe.disabled = 1;
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;
  pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
  fd = perf_event_open(&pe, 0, -1, perf_group_fd, 0);
  if (fd == -1) {
    fprintf(stderr, "Error perf_event_open %llx\n", pe.config);
    exit(EXIT_FAILURE);
  }
  ioctl(fd, PERF_EVENT_IOC_ID, &perf_id_inst);

  bzero(&pe, sizeof(struct perf_event_attr));
  pe.type = PERF_TYPE_SOFTWARE;
  pe.size = sizeof(struct perf_event_attr);
  pe.config = PERF_COUNT_SW_CONTEXT_SWITCHES;
  pe.disabled = 1;
  pe.exclude_kernel = 0;
  pe.exclude_hv = 0;
  pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
  fd = perf_event_open(&pe, 0, -1, perf_group_fd, 0);
  if (fd == -1) {
    fprintf(stderr, "Error perf_event_open %llx\n", pe.config);
    exit(EXIT_FAILURE);
  }
  ioctl(fd, PERF_EVENT_IOC_ID, &perf_id_ctx_switches);

  bzero(&pe, sizeof(struct perf_event_attr));
  pe.type = PERF_TYPE_SOFTWARE;
  pe.size = sizeof(struct perf_event_attr);
  pe.config = PERF_COUNT_SW_CPU_MIGRATIONS;
  pe.disabled = 1;
  pe.exclude_kernel = 0;
  pe.exclude_hv = 0;
  pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
  fd = perf_event_open(&pe, 0, -1, perf_group_fd, 0);
  if (fd == -1) {
    fprintf(stderr, "Error perf_event_open %llx\n", pe.config);
    exit(EXIT_FAILURE);
  }
  ioctl(fd, PERF_EVENT_IOC_ID, &perf_id_migrations);

}

inline void perf_record_start() {
  ioctl(perf_group_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
  ioctl(perf_group_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

inline void perf_record_end(struct perf_sample *out) {
  ioctl(perf_group_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

  char buf[4096];
  struct read_format* rf = (struct read_format*) buf;
  bzero(out, sizeof(*out));

  ssize_t r = read(perf_group_fd, buf, sizeof(buf));
  assert(r > 0);
  assert(r <= sizeof(buf));
  assert(rf->nr == 6);
  for (unsigned i = 0; i < rf->nr; i++) {
    if (rf->values[i].id == perf_id_ref)
      out->l1d_access = rf->values[i].value;
    else if (rf->values[i].id == perf_id_miss)
      out->l1d_miss = rf->values[i].value;
    else if (rf->values[i].id == perf_id_inst)
      out->instructions = rf->values[i].value;
    else if (rf->values[i].id == perf_id_cycles)
      out->cycles = rf->values[i].value;
    else if (rf->values[i].id == perf_id_ctx_switches)
      out->ctx_switches = rf->values[i].value;
    else if (rf->values[i].id == perf_id_migrations)
      out->migrations = rf->values[i].value;
    else
      assert(0 && "Unexpected perf id");
  }
}
#endif // DO_PERF_STATS

int main(int argc, char const *argv[]) {

    map<uint64_t, vector<struct trace_edge> > trace_per_path;
#if DUMP_ALL_TIMING_SAMPLES
    vector<vector<uint32_t> > allTimingSamples;
#endif
#if DO_PERF_STATS
    perf_setup();
    vector<pair<struct task_desc, vector<struct perf_sample> > > allPerfSamples;
#endif
    unsigned rayCntr = 0;

    try {
        if (argc <= 0) {
            abort();
        }

        if (argc < 4) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }

        /* CloudBVH requires this */
        PbrtOptions.nThreads = 1;

        const string scenePath{argv[1]};
        const string raysPath{argv[2]};
        const string outPrefix{argv[3]};

        int startPathId = -1, endPathId = -1;
        if (argc == 6) {
          startPathId = atoi(argv[4]);
          endPathId = atoi(argv[5]);
        }

        global::manager.init(scenePath);

        queue<RayStatePtr> rayList;
        queue<int> prevNodes;
        vector<Sample> samples;

        /* loading all the rays */
        {
            protobuf::RecordReader reader{raysPath};
            while (!reader.eof()) {
                string rayStr;

                if (reader.read(&rayStr)) {
                    auto rayStatePtr = RayState::Create();
                    rayStatePtr->Deserialize(rayStr.data(), rayStr.length());
                    const uint64_t pathID = rayStatePtr->PathID();
                    if (startPathId != -1 && !(startPathId <= pathID && pathID <= endPathId))
                      continue;
                    trace_per_path[rayStatePtr->PathID()] = vector<struct trace_edge>();
                    rayList.push(move(rayStatePtr));
                    prevNodes.push(-1);
                }
            }
        }

        cerr << rayList.size() << " RayState(s) loaded." << endl;

        if (!rayList.size()) {
            return EXIT_SUCCESS;
        }

        /* prepare the scene */
        MemoryArena arena;
        vector<unique_ptr<Transform>> transformCache;
        auto camera = loadCamera(transformCache);
        auto sampler = loadSampler();
        auto lights = loadLights();
        auto fakeScene = loadFakeScene();

        vector<unique_ptr<CloudBVH>> treelets;
        treelets.resize(global::manager.treeletCount());

        ofstream node_cnt_file(outPrefix + "node_cnt_for_treelet.txt");

        /* let's load all the treelets */
        for (size_t i = 0; i < treelets.size(); i++) {
            treelets[i] = make_unique<CloudBVH>(i);
            treelets[i]->LoadTreelet(i, nullptr);
            node_cnt_file << i << " " << treelets[i]->nodeCount() << endl;
        }
        node_cnt_file.close();

        cerr << treelets.size() << " total treelets." << endl;

        for (auto &light : lights) {
            light->Preprocess(fakeScene);
        }

        const auto sampleExtent = camera->film->GetSampleBounds().Diagonal();
        const int maxDepth = 5;

        while (!rayList.empty()) {
            RayStatePtr theRayPtr = move(rayList.front());
            RayState &theRay = *theRayPtr;
            rayList.pop();
            const int prevNodeID = prevNodes.front(); prevNodes.pop();
            rayCntr++;

            const TreeletId rayTreeletId = theRay.CurrentTreelet();
            const unsigned pathID = theRay.PathID();
            const int thisNodeID = trace_per_path[pathID].size();

#if TIMING_SAMPLES_CNT
            char rayBuffer[sizeof(RayState)];
            const auto serializedLen = theRay.Serialize(rayBuffer);
            vector<RayStatePtr> rayCopies(TIMING_SAMPLES_CNT);
            for (int i = 0; i < TIMING_SAMPLES_CNT; i++) {
                auto theRayPtr2 = RayState::Create();
                theRayPtr2->Deserialize(rayBuffer+4, serializedLen-4);
                rayCopies[i] = move(theRayPtr2);
            }
            long int min_elapsed = -1;
            vector<uint32_t> timingSamples(TIMING_SAMPLES_CNT);
            TaskType taskType;
            unsigned bvhNodesVisited = 0;
#if DO_PERF_STATS
            vector<struct perf_sample> perfSamples(TIMING_SAMPLES_CNT);
#endif
#endif
            if (!theRay.toVisitEmpty()) {
#if TIMING_SAMPLES_CNT
                taskType = TaskTypeTrace;
                for (int i = 0; i < TIMING_SAMPLES_CNT; i++) {
                    auto theRayPtr2 = move(rayCopies[i]);
                    const auto nodesVisitedBefore = getNodesVisitedCounter();
                    auto start = chrono::high_resolution_clock::now();
#if DO_PERF_STATS
                    perf_record_start();
#endif
                    graphics::TraceRay(move(theRayPtr2),
                                                    *treelets[rayTreeletId]);
#if DO_PERF_STATS
                    perf_record_end(&perfSamples[i]);
#endif
                    auto stop = chrono::high_resolution_clock::now();
                    auto elapsed_ns = chrono::duration_cast<chrono::nanoseconds>(stop - start).count();
                    if (elapsed_ns < min_elapsed || min_elapsed == -1) min_elapsed = elapsed_ns;
                    bvhNodesVisited = getNodesVisitedCounter() - nodesVisitedBefore;
#if DUMP_ALL_TIMING_SAMPLES
                    timingSamples[i] = elapsed_ns;
#endif
                }
#endif // TIMING_SAMPLES_CNT
                auto newRayPtr = graphics::TraceRay(move(theRayPtr),
                                                    *treelets[rayTreeletId]);
                auto &newRay = *newRayPtr;

                const bool hit = newRay.HasHit();
                const bool emptyVisit = newRay.toVisitEmpty();

                if (newRay.IsShadowRay()) {
                    if (hit || emptyVisit) {
                        newRay.Ld = hit ? 0.f : newRay.Ld;
                        samples.emplace_back(*newRayPtr);
                    } else {
                        rayList.push(move(newRayPtr));
                        prevNodes.push(thisNodeID);
                    }
                } else if (!emptyVisit || hit) {
                    rayList.push(move(newRayPtr));
                    prevNodes.push(thisNodeID);
                } else if (emptyVisit) {
                    newRay.Ld = 0.f;
                    samples.emplace_back(*newRayPtr);
                }
            } else if (theRay.HasHit()) {
#if TIMING_SAMPLES_CNT
                taskType = TaskTypeShade;
                for (int i = 0; i < 20; i++) {
                    auto theRayPtr2 = move(rayCopies[i]);
                    const auto nodesVisitedBefore = getNodesVisitedCounter();
                    auto start = chrono::high_resolution_clock::now();
#if DO_PERF_STATS
                    perf_record_start();
#endif
                    graphics::ShadeRay(move(theRayPtr2), *treelets[rayTreeletId],
                                       lights, sampleExtent, sampler, maxDepth, arena);
#if DO_PERF_STATS
                    perf_record_end(&perfSamples[i]);
#endif
                    auto stop = chrono::high_resolution_clock::now();
                    auto elapsed_ns = chrono::duration_cast<chrono::nanoseconds>(stop - start).count();
                    if (elapsed_ns < min_elapsed || min_elapsed == -1) min_elapsed = elapsed_ns;
                    bvhNodesVisited = getNodesVisitedCounter() - nodesVisitedBefore;
#if DUMP_ALL_TIMING_SAMPLES
                    timingSamples[i] = elapsed_ns;
#endif
                }
#endif // TIMING_SAMPLES_CNT
                RayStatePtr bounceRay, shadowRay;
                tie(bounceRay, shadowRay) =
                    graphics::ShadeRay(move(theRayPtr), *treelets[rayTreeletId],
                                       lights, sampleExtent, sampler, maxDepth, arena);

                if (bounceRay != nullptr) {
                    rayList.push(move(bounceRay));
                    prevNodes.push(thisNodeID);
                }

                if (shadowRay != nullptr) {
                    rayList.push(move(shadowRay));
                    prevNodes.push(thisNodeID);
                }
            }
#if TIMING_SAMPLES_CNT
            trace_per_path[pathID].push_back({ thisNodeID, prevNodeID, taskType, rayTreeletId, min_elapsed, bvhNodesVisited });
#if DUMP_ALL_TIMING_SAMPLES
            allTimingSamples.push_back(timingSamples);
#endif
#if DO_PERF_STATS
            allPerfSamples.push_back({ { pathID, thisNodeID, taskType }, perfSamples });
#endif
#endif
        }

        ofstream trace_per_path_file(outPrefix + "trace_per_path.txt");
        for (auto const& x : trace_per_path) {
          trace_per_path_file << x.first;
          for (auto const& e: x.second) trace_per_path_file << " "
            << e.nodeID << " " << e.prevNodeID << " " << e.taskType << " " << e.treeletID << " " << e.elapsed_ns << " " << e.bvhNodesVisited;
          trace_per_path_file << endl;
        }
        trace_per_path_file.close();

#if DO_PERF_STATS
        ofstream perfSamples_file(outPrefix + "perfSamples.txt");
        for (auto const& x : allPerfSamples) {
          perfSamples_file << x.first.pathID << " " << x.first.nodeID << " " << x.first.taskType;
          for (auto const& ps: x.second) {
            perfSamples_file << " " << ps.cycles << " " << ps.instructions << " "
                << ps.ctx_switches << " " << ps.migrations << " "
                << ps.l1d_access << " " << ps.l1d_miss;
          }
          perfSamples_file << endl;
        }
        perfSamples_file.close();
#endif // DO_PERF_STATS

#if DUMP_ALL_TIMING_SAMPLES
        ofstream timingSamples_file(outPrefix + "timingSamples.txt");
        for (auto const& x : allTimingSamples) {
          for (auto const& ns: x) timingSamples_file << ns << " ";
          timingSamples_file << endl;
        }
        timingSamples_file.close();
#endif // DUMP_ALL_TIMING_SAMPLES

        cerr << rayCntr << " rays total." << endl;

        graphics::AccumulateImage(camera, samples);
        camera->film->WriteImage();
    } catch (const exception &e) {
        print_exception(argv[0], e);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
