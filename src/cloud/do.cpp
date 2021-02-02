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

#define TIMING_SAMPLES_CNT (1 * 1000)
struct trace_edge {
  int nodeID;
  int prevNodeID;
  TreeletId treeletID;
  long int elapsed_ns;
};

int main(int argc, char const *argv[]) {

    map<uint64_t, vector<struct trace_edge> > trace_per_path;
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

        /* let's load all the treelets */
        for (size_t i = 0; i < treelets.size(); i++) {
            treelets[i] = make_unique<CloudBVH>(i);
        }

        cerr << treelets.size() << " total treelets" << endl;

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
            const uint64_t pathID = theRay.PathID();
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
#endif
            if (!theRay.toVisitEmpty()) {
#if TIMING_SAMPLES_CNT
                for (int i = 0; i < TIMING_SAMPLES_CNT; i++) {
                    auto theRayPtr2 = move(rayCopies[i]);
                    auto start = chrono::high_resolution_clock::now();
                    graphics::TraceRay(move(theRayPtr2),
                                                    *treelets[rayTreeletId]);
                    auto stop = chrono::high_resolution_clock::now();
                    auto elapsed_ns = chrono::duration_cast<chrono::nanoseconds>(stop - start).count();
                    if (elapsed_ns < min_elapsed || min_elapsed == -1) min_elapsed = elapsed_ns;
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
                for (int i = 0; i < TIMING_SAMPLES_CNT; i++) {
                    auto theRayPtr2 = move(rayCopies[i]);
                    auto start = chrono::high_resolution_clock::now();
                    graphics::ShadeRay(move(theRayPtr2), *treelets[rayTreeletId],
                                       lights, sampleExtent, sampler, maxDepth, arena);
                    auto stop = chrono::high_resolution_clock::now();
                    auto elapsed_ns = chrono::duration_cast<chrono::nanoseconds>(stop - start).count();
                    if (elapsed_ns < min_elapsed || min_elapsed == -1) min_elapsed = elapsed_ns;
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
            trace_per_path[pathID].push_back({ thisNodeID, prevNodeID, rayTreeletId, min_elapsed });
#endif
        }

        ofstream trace_per_path_file(outPrefix + "trace_per_path.txt");
        for (auto const& x : trace_per_path) {
          trace_per_path_file << x.first;
          for (auto const& e: x.second) trace_per_path_file << " "
            << e.nodeID << " " << e.prevNodeID << " " << e.treeletID << " " << e.elapsed_ns;
          trace_per_path_file << endl;
        }
        trace_per_path_file.close();

        cerr << rayCntr << " rays total." << endl;

        graphics::AccumulateImage(camera, samples);
        camera->film->WriteImage();
    } catch (const exception &e) {
        print_exception(argv[0], e);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
