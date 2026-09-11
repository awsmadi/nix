#pragma once
///@file

#include "nix/store/build/derivation-builder.hh"
#include "nix/store/local-store.hh"
#include "nix/util/sync.hh"
#ifndef _WIN32
#  include "nix/store/user-lock.hh"
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <list>
#include <thread>

namespace nix {

/**
 * The state for building locally that does not depend on the platform,
 * and `registerOutputs`, which every implementation ends with.
 *
 * The platform-specific subclasses --- `UnixDerivationBuilderImpl` and
 * `WindowsDerivationBuilderImpl` --- own everything about *running* the
 * builder: sandboxing, build users, and recursive Nix are all Unix-only
 * so far.
 *
 * @todo This should not be a class. `registerOutputs` wants to be a
 * function over the state it reads, which is most of what is here.
 */
class DerivationBuilderImpl : public DerivationBuilder, public DerivationBuilderParams
{
private:
    /* VTable anchor to avoid weak linkage of the vtable - it breaks
       dynamic_cast across shared libraries on Darwin. */
    void anchor() override;

protected:

    /**
     * The process ID of the builder.
     */
    Pid pid;

    std::shared_ptr<BuildingStore> store;

    std::shared_ptr<DerivationBuilderCallbacks> miscMethods;

    /**
     * The temporary directory used for the build.
     */
    std::filesystem::path tmpDir;

    /**
     * The sort of derivation we are building.
     *
     * Just a cached value, computed from `drv`.
     */
    const derivation::Type derivationType;

    const LocalSettings & localSettings = store->getLocalSettings();

#ifndef _WIN32
    /**
     * User selected for running the builder.
     */
    std::unique_ptr<UserLock> buildUser;
#endif

    /**
     * Hash rewriting.
     */
    StringMap inputRewrites, outputRewrites;
    typedef std::map<StorePath, StorePath> RedirectedOutputs;
    RedirectedOutputs redirectedOutputs;

    /**
     * The output paths used during the build.
     *
     * - Input-addressed derivations or fixed content-addressed outputs are
     *   sometimes built when some of their outputs already exist, and can not
     *   be hidden via sandboxing. We use temporary locations instead and
     *   rewrite after the build. Otherwise the regular predetermined paths are
     *   put here.
     *
     * - Floating content-addressing derivations do not know their final build
     *   output paths until the outputs are hashed, so random locations are
     *   used, and then renamed. The randomness helps guard against hidden
     *   self-references.
     */
    OutputPathMap scratchOutputs;

    /**
     * Where an output path lives while the build runs.
     *
     * Sandboxing can put it somewhere other than its final home, so this
     * is a hook rather than just `Store::toRealPath`.
     */
    virtual std::filesystem::path realPathInHost(const StorePath & p)
    {
        return store->toRealPath(p);
    }

    /**
     * Whether a path may be referenced by outputs of this build, checking
     * both the input closure and paths added at runtime through recursive
     * Nix (`RestrictionContext::addDependency`, tracked in `state_`).
     */
    bool isAllowed(const StorePath & path) override
    {
        if (inputPaths.count(path))
            return true;
        auto state(state_.lock());
        auto iter = state->addedPaths.find(path);
        if (iter == state->addedPaths.end())
            return false;
        return iter->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

public:

    DerivationBuilderImpl(
        std::shared_ptr<BuildingStore> store,
        std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params)
        : DerivationBuilderParams{std::move(params)}
        , store{std::move(store)}
        , miscMethods{std::move(miscMethods)}
        , derivationType{derivation::type(drv)}
    {
    }

    SingleDrvOutputs registerOutputs(LocalStore & localStore) override;

    /**
     * Output paths from the `SubmitOutput` store command
     */
    Sync<OutputPathMap> submittedOutputs;

    SingleDrvOutputs checkSubmittedOutputs(LocalStore & localStore) override;

    /**
     * Record an output submitted by a recursive-nix client.
     */
    void submitOutput(const SingleDerivedPath & path, const OutputName & output) override;

protected:

    /**
     * The recursive Nix daemon socket.
     */
    AutoCloseFD daemonSocket;

    /**
     * The daemon main thread.
     */
    std::thread daemonThread;

    /**
     * Set by `stopDaemon` before it touches `daemonSocket`, and checked by
     * the accept loop on every iteration.
     *
     * Without this, the accept loop re-reads `daemonSocket.get()` with no
     * synchronisation against `stopDaemon()` closing it: `stopDaemon` can
     * close the fd between the loop's check and its use, and if that fd
     * number has already been reused for an unrelated *listening* socket
     * (plausible under `max-jobs > 1`, since a sibling build's own
     * `.nix-socket` is the most likely occupant), `accept` on it can
     * succeed and this thread would serve the sibling's connection with
     * *our* restricted store and `*this` as the `RestrictionContext`. This
     * flag lets the loop recognise a shutdown in progress instead of
     * inferring it from an accept failure's error code, which both closes
     * that hole and makes the error classification below only responsible
     * for classifying genuine errors.
     */
    std::atomic<bool> daemonStopping{false};

    struct DaemonWorkerState
    {
        std::thread thread;
        ref<std::atomic_flag> done;
        /**
         * Shared with the worker thread, so `stopDaemon` can `shutdown` it
         * to unblock a worker whose client never closes its own end.
         * Closing the listener only wakes a blocked `accept`; it does
         * nothing for connections already accepted.
         */
        ref<AutoCloseFD> remote;
    };

    /**
     * The daemon worker threads.
     */
    std::list<DaemonWorkerState> daemonWorkerThreads;

    /**
     * Start an in-process nix daemon thread for recursive-nix.
     *
     * Platform-neutral apart from the three hooks below.
     */
    void startDaemon();

    /**
     * Stop the in-process nix daemon thread.
     * @see startDaemon
     */
    void stopDaemon();

    /**
     * Where the build directory appears from the builder's point of view.
     *
     * A sandbox can mount it somewhere else; without one it is just `tmpDir`.
     */
    virtual std::filesystem::path tmpDirInSandbox()
    {
        return tmpDir;
    }

    /**
     * Make the daemon socket reachable by whoever runs the builder.
     *
     * On Unix that means handing it to the build user. Windows has no build
     * users, so there is nothing to do.
     */
    virtual void prepareDaemonSocket(const std::filesystem::path & path) {}

    /**
     * Where the builder should reach the recursive Nix daemon, once
     * `startDaemon` has bound the socket.
     *
     * `startDaemon` cannot write it into the environment itself: Unix keeps a
     * `StringMap` it mutates, while Windows builds an `OsString` block from
     * scratch. Each injects this instead.
     */
    std::optional<std::string> daemonRemoteUri;

    /**
     * Whether the outputs are being submitted by the builder rather than
     * produced by it, which gates on a different experimental feature.
     *
     * Only the Unix builder supports dynamic derivations so far.
     */
    virtual bool usingSubmittedOutputs()
    {
        return false;
    }
};

} // namespace nix
