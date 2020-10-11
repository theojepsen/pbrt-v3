#include "integrators/pathcluster.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "bssrdf.h"
#include "camera.h"
#include "film.h"
#include "integrator.h"
#include "interaction.h"
#include "parallel.h"
#include "paramset.h"
#include "progressreporter.h"
#include "sampler.h"
#include "sampling.h"
#include "scene.h"
#include "stats.h"
#include "util/httplib.h"
#include "util/tokenize.h"

using namespace std;
using namespace chrono;

namespace pbrt {

void PathClusterIntegrator::Render(const Scene &scene) {
    Preprocess(scene, *sampler);
    // Render image tiles in parallel

    if (PbrtOptions.clusterCoordinator.empty()) {
        throw runtime_error("Cluster coordinator address not provided.");
    }

    while (true) {
        httplib::Client coordinator(
            ("http://" + PbrtOptions.clusterCoordinator).c_str());

        auto res = coordinator.Get("/hello");
        if (res && res->status == 200) {
            break;
        } else {
            cerr << "Retrying /hello...:\n";
            this_thread::sleep_for(1s);
        }
    }

    __timepoints.wait_for_coordinator_ended = TimePoints::clock::now();

    constexpr milliseconds WAIT_BEFORE_RETRY = 500ms;

    ParallelFor(
        [&](int64_t thread_id) {
            httplib::Client coordinator(
                ("http://" + PbrtOptions.clusterCoordinator).c_str());

            coordinator.set_keep_alive(false);

            while (true) {
                auto res = coordinator.Get("/tile");

                if (!res || res->status != 200 || res->body.empty()) {
                    LOG(INFO) << "Error /tile";
                    this_thread::sleep_for(WAIT_BEFORE_RETRY);
                    continue;
                }

                if (res->body == "DONE") {
                    LOG(INFO) << "Job done.";
                    break;
                }

                const auto tokens = split(res->body, " ");

                if (tokens.size() != 5) {
                    LOG(INFO)
                        << "Invalid response from coordinator: " << res->body;
                    this_thread::sleep_for(1s);
                    continue;
                }

                const auto &tile_id = tokens[0];
                int x0 = stoi(tokens[1]);
                int x1 = stoi(tokens[2]);
                int y0 = stoi(tokens[3]);
                int y1 = stoi(tokens[4]);

                // Allocate _MemoryArena_ for tile
                MemoryArena arena;

                // Get sampler instance for tile
                int seed = stoi(tile_id);
                unique_ptr<Sampler> tileSampler = sampler->Clone(seed);

                // Compute sample bounds for tile
                Bounds2i tileBounds(Point2i(x0, y0), Point2i(x1, y1));
                LOG(INFO) << "Starting image tile " << tileBounds;

                // Get _FilmTile_ for tile
                unique_ptr<FilmTile> filmTile =
                    camera->film->GetFilmTile(tileBounds);

                // Loop over pixels in tile to render them
                for (Point2i pixel : tileBounds) {
                    {
                        ProfilePhase pp(Prof::StartPixel);
                        tileSampler->StartPixel(pixel);
                    }

                    // Do this check after the StartPixel() call; this keeps
                    // the usage of RNG values from (most) Samplers that use
                    // RNGs consistent, which improves reproducability /
                    // debugging.
                    if (!InsideExclusive(pixel, pixelBounds)) continue;

                    do {
                        // Initialize _CameraSample_ for current sample
                        CameraSample cameraSample =
                            tileSampler->GetCameraSample(pixel);

                        // Generate camera ray for current sample
                        RayDifferential ray;
                        Float rayWeight =
                            camera->GenerateRayDifferential(cameraSample, &ray);
                        ray.ScaleDifferentials(
                            1 / sqrt((Float)tileSampler->samplesPerPixel));

                        // Evaluate radiance along camera ray
                        Spectrum L(0.f);
                        if (rayWeight > 0)
                            L = Li(ray, scene, *tileSampler, arena, 0);

                        // Issue warning if unexpected radiance value returned
                        if (L.HasNaNs()) {
                            LOG(ERROR) << StringPrintf(
                                "Not-a-number radiance value returned "
                                "for pixel (%d, %d), sample %d. Setting to "
                                "black.",
                                pixel.x, pixel.y,
                                (int)tileSampler->CurrentSampleNumber());
                            L = Spectrum(0.f);
                        } else if (L.y() < -1e-5) {
                            LOG(ERROR) << StringPrintf(
                                "Negative luminance value, %f, returned "
                                "for pixel (%d, %d), sample %d. Setting to "
                                "black.",
                                L.y(), pixel.x, pixel.y,
                                (int)tileSampler->CurrentSampleNumber());
                            L = Spectrum(0.f);
                        } else if (isinf(L.y())) {
                            LOG(ERROR) << StringPrintf(
                                "Infinite luminance value returned "
                                "for pixel (%d, %d), sample %d. Setting to "
                                "black.",
                                pixel.x, pixel.y,
                                (int)tileSampler->CurrentSampleNumber());
                            L = Spectrum(0.f);
                        }
                        VLOG(1) << "Camera sample: " << cameraSample
                                << " -> ray: " << ray << " -> L = " << L;

                        // Add camera ray's contribution to image
                        filmTile->AddSample(cameraSample.pFilm, L, rayWeight);

                        // Free _MemoryArena_ memory from computing image sample
                        // value
                        arena.Reset();
                    } while (tileSampler->StartNextSample());
                }
                LOG(INFO) << "Finished image tile " << tileBounds;

                // Merge image tile into _Film_
                camera->film->MergeFilmTile(move(filmTile));

                while (true) {
                    auto res = coordinator.Get(("/done?t=" + tile_id).c_str());
                    if (!res || res->status != 200) {
                        LOG(INFO) << "Retrying /done...:";
                        this_thread::sleep_for(WAIT_BEFORE_RETRY);
                        continue;
                    } else {
                        break;
                    }
                }
            }
        },
        MaxThreadIndex());

    LOG(INFO) << "Rendering finished";

    // Save final image after rendering
    camera->film->WriteImage();
}

PathIntegrator *CreatePathClusterIntegrator(const ParamSet &params,
                                            shared_ptr<Sampler> sampler,
                                            shared_ptr<const Camera> camera) {
    int maxDepth = params.FindOneInt("maxdepth", 5);
    int np;
    const int *pb = params.FindInt("pixelbounds", &np);
    Bounds2i pixelBounds = camera->film->GetSampleBounds();
    if (pb) {
        if (np != 4)
            Error("Expected four values for \"pixelbounds\" parameter. Got %d.",
                  np);
        else {
            pixelBounds = Intersect(pixelBounds,
                                    Bounds2i{{pb[0], pb[2]}, {pb[1], pb[3]}});
            if (pixelBounds.Area() == 0)
                Error("Degenerate \"pixelbounds\" specified.");
        }
    }
    Float rrThreshold = params.FindOneFloat("rrthreshold", 1.);
    string lightStrategy =
        params.FindOneString("lightsamplestrategy", "spatial");
    return new PathClusterIntegrator(maxDepth, camera, sampler, pixelBounds,
                                     rrThreshold, lightStrategy);
}

}  // namespace pbrt
