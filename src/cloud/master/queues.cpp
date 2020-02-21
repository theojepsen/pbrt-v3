#include "cloud/lambda-master.h"
#include "messages/utils.h"
#include "util/random.h"

using namespace std;
using namespace pbrt;
using namespace meow;
using namespace PollerShortNames;

using OpCode = Message::OpCode;

bool LambdaMaster::assignWork(Worker& worker) {
    /* return, if worker is not active anymore */
    if (worker.state != Worker::State::Active) return false;

    /* return if the worker already has enough work */
    if (worker.activeRays() >= WORKER_MAX_ACTIVE_RAYS) return false;

    /* return, if the worker doesn't have any treelets */
    if (worker.treelets.empty()) return false;

    const TreeletId treeletId = *worker.treelets.begin();
    auto queueIt = queuedRayBags.find(treeletId);

    /* Q1: do we have any rays to generate? */
    bool raysToGenerate =
        tiles.cameraRaysRemaining() && (worker.treelets.count(0) > 0);

    /* Q2: do we have any work for this worker? */
    bool workToDo = (queueIt != queuedRayBags.end());

    if (!raysToGenerate && !workToDo) {
        return true;
    }

    protobuf::RayBags proto;

    while ((raysToGenerate || workToDo) &&
           worker.activeRays() < WORKER_MAX_ACTIVE_RAYS) {
        if (raysToGenerate && workToDo) {
            /* we flip and coin and decide which one to do */
            bernoulli_distribution coin{0.0};
            if (coin(randEngine)) {
                tiles.sendWorkerTile(worker);
                raysToGenerate = tiles.cameraRaysRemaining();
                continue;
            }
        } else if (raysToGenerate) {
            tiles.sendWorkerTile(worker);
            raysToGenerate = tiles.cameraRaysRemaining();
            continue;
        }

        /* only if workToDo or the coin flip returned false */
        *proto.add_items() = to_protobuf(queueIt->second.front());
        recordAssign(worker.id, queueIt->second.front());
        queueIt->second.pop();

        if (queueIt->second.empty()) {
            queuedRayBags.erase(queueIt);
            workToDo = false;
        }
    }

    if (proto.items_size()) {
        worker.connection->enqueue_write(Message::str(
            0, OpCode::ProcessRayBag, protoutil::to_string(proto)));
    }

    return worker.activeRays() < WORKER_MAX_ACTIVE_RAYS;
}

ResultType LambdaMaster::handleQueuedRayBags() {
    ScopeTimer<TimeLog::Category::QueuedRayBags> _timer;

    shuffle(freeWorkers.begin(), freeWorkers.end(), randEngine);

    auto it = freeWorkers.begin();

    while (it != freeWorkers.end() &&
           (!queuedRayBags.empty() || tiles.cameraRaysRemaining())) {
        auto workerIt = workers.find(*it);

        if (workerIt == workers.end() || !assignWork(workerIt->second)) {
            it = freeWorkers.erase(it);
        } else {
            it++;
        }
    }

    return ResultType::Continue;
}

template <class T>
void moveFromTo(queue<T>& from, queue<T>& to) {
    while (!from.empty()) {
        to.emplace(move(from.front()));
        from.pop();
    }
}

void LambdaMaster::moveFromPendingToQueued(const TreeletId treeletId) {
    if (pendingRayBags.count(treeletId) > 0) {
        moveFromTo(pendingRayBags[treeletId], queuedRayBags[treeletId]);
        pendingRayBags.erase(treeletId);
    }
}

void LambdaMaster::moveFromQueuedToPending(const TreeletId treeletId) {
    if (queuedRayBags.count(treeletId) > 0) {
        moveFromTo(queuedRayBags[treeletId], pendingRayBags[treeletId]);
        queuedRayBags.erase(treeletId);
    }
}
