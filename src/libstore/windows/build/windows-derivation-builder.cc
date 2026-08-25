#include "nix/store/build/derivation-builder.hh"
#include "nix/store/build/derivation-env-desugar.hh"
#include "nix/store/local-store.hh"
#include "nix/store/globals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/file-content-address.hh"
#include "nix/util/file-system.hh"
#include "nix/util/muxable-pipe.hh"
#include "nix/util/os-string.hh"
#include "nix/util/processes.hh"

#include <windows.h>

namespace nix {

namespace {

/** Like `windows/processes.cc`'s version, which is file-local rather than exported. */
void setInheritable(AutoCloseFD & fd, bool inherit)
{
    if (!SetHandleInformation(fd.get(), HANDLE_FLAG_INHERIT, inherit ? HANDLE_FLAG_INHERIT : 0))
        throw windows::WinError("cannot change handle inheritability");
}

/** The builder's stdin. */
AutoCloseFD openNullDevice()
{
    SECURITY_ATTRIBUTES sa{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    AutoCloseFD fd = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!fd)
        throw windows::WinError("cannot open NUL device for the builder's stdin");
    return fd;
}

/**
 * Quote one argument per the `CommandLineToArgvW` rules: backslashes are
 * literal unless they precede a quote, where they double.
 *
 * Like `windowsEscape`, which is also not exported.
 */
OsString escapeArg(OsString arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == OsString::npos)
        return arg;

    OsString out;
    out += L'"';
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            /* Escape trailing backslashes so they do not escape the closing
               quote. */
            out.append(backslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += *it;
        } else {
            out.append(backslashes, L'\\');
            out += *it;
        }
    }
    out += L'"';
    return out;
}

/**
 * A minimal, unsandboxed `DerivationBuilder` for Windows: enough to run a
 * builder and register its outputs, and no more. Missing, relative to Unix:
 *
 * - no sandbox, chroot, or filesystem isolation
 * - no build user; the builder runs as whoever ran Nix
 * - no network isolation
 * - no recursive Nix (`submitOutput` throws)
 * - no content-addressed or fixed-output derivations
 * - no hash rewriting, so no self-references in outputs
 * - no output checks; `allowedReferences` and friends are ignored
 *
 * Separate from `DerivationBuilderImpl` rather than derived from it because that
 * class lives under `unix/`. Sharing it means moving the platform-neutral part
 * out first, at which point this can go.
 */
class WindowsDerivationBuilder : public DerivationBuilder, public DerivationBuilderParams
{
public:

    WindowsDerivationBuilder(
        LocalStore & store,
        std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params,
        HANDLE ioport)
        : DerivationBuilderParams{std::move(params)}
        , store{store}
        , miscMethods{std::move(miscMethods)}
        , ioport{ioport}
    {
    }

    LocalStore & store;
    std::shared_ptr<DerivationBuilderCallbacks> miscMethods;

    /** The worker's I/O completion port, which the log pipe must be tied to. */
    HANDLE ioport;

    /** The build directory, exposed as `NIX_BUILD_TOP`. Not a sandbox. */
    std::filesystem::path tmpDir;

    /**
     * The builder's merged stdout/stderr. A `MuxablePipe` because the worker
     * waits on I/O completion ports, and `MuxablePipePollState::iterate` reads
     * the pipe's `overlapped` state directly.
     */
    MuxablePipe builderPipe;

    /** The child. Killed by `Pid`'s destructor if it outlives this object. */
    Pid process;

    /** Whether `process` holds a started child, since `Pid` does not say. */
    bool started = false;

    /* --- RestrictionContext --- */

    const StorePathSet & originalPaths() override
    {
        return inputPaths;
    }

    bool isAllowed(const StorePath & path) override
    {
        return inputPaths.count(path) > 0;
    }

    bool isAllowed(const DrvOutput &) override
    {
        return false;
    }

    bool shouldModifySandbox() override
    {
        /* There is no sandbox to modify. */
        return false;
    }

    void submitOutput(const SingleDerivedPath &, const OutputName &) override
    {
        throw UnimplementedError("recursive Nix is not yet supported on Windows");
    }

    void addDependencyImpl(const StorePath &) override
    {
        /* Only reachable through recursive Nix, which `submitOutput` rejects. */
        throw UnimplementedError("recursive Nix is not yet supported on Windows");
    }

    /* --- DerivationBuilder --- */

    std::optional<Descriptor> startBuild() override;
    SingleDrvOutputs unprepareBuild() override;
    bool killChild() override;

private:

    /**
     * Remove a path a previous build left behind. Store paths are read-only, and
     * Windows honours that attribute on delete where POSIX goes by directory
     * permission, so it has to be cleared first.
     */
    void deleteStalePath(const std::filesystem::path & path);

    /** The builder's environment block. Not inherited from the parent. */
    OsString makeEnvBlock();

    /** Start the builder. Sets `process`. */
    void spawnBuilder();
};

void WindowsDerivationBuilder::deleteStalePath(const std::filesystem::path & path)
{
    if (!std::filesystem::exists(std::filesystem::symlink_status(path)))
        return;

    /* `DeleteFileW` refuses a read-only file, so clear it here and below. */
    auto clearReadOnly = [](const std::filesystem::path & p) {
        auto wide = p.native();
        auto attrs = GetFileAttributesW(wide.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(wide.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    };

    clearReadOnly(path);
    if (std::filesystem::is_directory(std::filesystem::symlink_status(path)))
        for (auto & entry : std::filesystem::recursive_directory_iterator{path})
            clearReadOnly(entry.path());

    deletePath(path);
}

OsString WindowsDerivationBuilder::makeEnvBlock()
{
    OsStringMap env;

    /* For runtime strings; fixed names use `OS_STR`, which widens at compile time. */
    auto os = [](std::string_view s) { return string_to_os_string(s); };

    /* Built from scratch rather than inherited. This is why `spawnProcess` is not
       used below: it merges the parent's environment. */
    env[OS_STR("NIX_BUILD_TOP")] = tmpDir.native();
    env[OS_STR("TMP")] = tmpDir.native();
    env[OS_STR("TEMP")] = tmpDir.native();
    env[OS_STR("TMPDIR")] = tmpDir.native();
    env[OS_STR("TEMPDIR")] = tmpDir.native();
    env[OS_STR("PWD")] = tmpDir.native();
    env[OS_STR("NIX_STORE")] = os(store.storeDir);

    /* Most Windows programs, `cmd.exe` included, will not start without these, and
       system tools live outside the store so `PATH` is needed to find them at all.
       Impure: a derivation sees whatever else is on the builder's `PATH`. */
    for (std::string_view name : {"SystemRoot", "SystemDrive", "windir", "COMSPEC", "PATHEXT", "PATH"})
        if (auto value = getEnvOs(os(name)))
            env[os(name)] = *value;

    /* The derivation's own environment wins over all of the above. */
    for (auto & [name, entry] : desugaredEnv.variables)
        env[os(name)] = os(entry.value);

    OsString block;
    for (auto & [name, value] : env) {
        block += name;
        block += L'=';
        block += value;
        block += L'\0';
    }
    /* An environment block is terminated by a second NUL. */
    block += L'\0';

    return block;
}

void WindowsDerivationBuilder::spawnBuilder()
{
    /* The child must not inherit the side we read from. */
    setInheritable(builderPipe.readSide, false);
    setInheritable(builderPipe.writeSide, true);

    AutoCloseFD stdIn = openNullDevice();

    STARTUPINFOW startInfo = {0};
    startInfo.cb = sizeof(startInfo);
    startInfo.dwFlags = STARTF_USESTDHANDLES;
    startInfo.hStdInput = stdIn.get();
    startInfo.hStdOutput = builderPipe.writeSide.get();
    startInfo.hStdError = builderPipe.writeSide.get();

    OsString cmdline = escapeArg(string_to_os_string(std::string_view{drv.builder}));
    for (auto & arg : drv.args) {
        cmdline += L' ';
        cmdline += escapeArg(string_to_os_string(std::string_view{arg}));
    }

    auto envBlock = makeEnvBlock();

    PROCESS_INFORMATION procInfo = {0};
    if (CreateProcessW(
            /* The executable is given in the command line. */
            NULL,
            cmdline.data(),
            NULL,
            NULL,
            /* Inherit handles, so the child gets the pipe. */
            TRUE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
            envBlock.data(),
            tmpDir.native().c_str(),
            &startInfo,
            &procInfo)
        == 0)
        throw windows::WinError("CreateProcessW failed for builder '%s'", drv.builder);

    /* Held locally until the child is fully set up, so that the failure paths
       below can tear it down without `Pid` also doing so. */
    AutoCloseFD processHandle = procInfo.hProcess;
    AutoCloseFD thread = procInfo.hThread;

    /* Put the child in a job object so it dies with us rather than leaking.
       Without this a runaway builder outlives the daemon. */
    Descriptor job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) {
        TerminateProcess(processHandle.get(), 1);
        throw windows::WinError("cannot create job object for builder");
    }
    if (AssignProcessToJobObject(job, processHandle.get()) == FALSE) {
        TerminateProcess(processHandle.get(), 1);
        throw windows::WinError("cannot assign builder to job object");
    }
    if (ResumeThread(thread.get()) == (DWORD) -1) {
        TerminateProcess(processHandle.get(), 1);
        throw windows::WinError("cannot resume builder");
    }

    process = std::move(processHandle);
    started = true;

    /* We do not write to the builder's output, and holding the write side open
       would mean never seeing EOF. */
    builderPipe.writeSide.close();
}

std::optional<Descriptor> WindowsDerivationBuilder::startBuild()
{
    /* There are no build users to contend over, so this never has to ask the
       caller to retry. */

    if (drv.isBuiltin())
        throw UnimplementedError("builtin builders are not yet supported on Windows");

    for (auto & [name, output] : drv.outputs)
        if (!std::get_if<DerivationOutput::InputAddressed>(&output.raw))
            throw UnimplementedError(
                "only input-addressed derivation outputs are supported on Windows, but output '%s' is not one", name);

    /* A fresh build directory per attempt. */
    tmpDir = createTempDir(defaultTempDir(), "nix-build");

    /* Clear anything a previous failed build left at the output paths. */
    for (auto & [name, status] : initialOutputs)
        if (status.known)
            deleteStalePath(store.toRealPath(status.known->path));

    miscMethods->openLogFile();

    builderPipe.createAsyncPipe(ioport);

    spawnBuilder();

    /* `builderOut` stays unset: the pipe owns the read handle, so duplicating it
       there would close it twice. Windows callers use `commChannel->readSide`. */
    commChannel = &builderPipe;

    return builderPipe.readSide.get();
}

bool WindowsDerivationBuilder::killChild()
{
    if (!started)
        return false;

    /* `Pid::kill` terminates and then reaps. Windows has no signals, so there
       is no gentler option to try first. */
    process.kill();
    started = false;

    return true;
}

SingleDrvOutputs WindowsDerivationBuilder::unprepareBuild()
{
    /* The caller only gets here once the log pipe hit EOF, which means the
       builder closed its handles. Reap anyway, so the exit code is settled. */
    int exitCode = process.wait();
    started = false;

    miscMethods->closeLogFile();
    miscMethods->childTerminated();

    if (exitCode != 0) {
        deletePath(tmpDir);
        throw BuilderFailureError{
            BuildResult::Failure::PermanentFailure,
            exitCode,
            fmt("builder '%s' exited with status %d", drv.builder, exitCode),
        };
    }

    SingleDrvOutputs builtOutputs;

    for (auto & [outputName, output] : drv.outputs) {
        auto * ia = std::get_if<DerivationOutput::InputAddressed>(&output.raw);
        assert(ia); /* checked in `startBuild` */

        auto & requiredPath = ia->path;
        auto actualPath = store.toRealPath(requiredPath);

        if (!std::filesystem::exists(std::filesystem::symlink_status(actualPath)))
            throw BuildError(
                BuildResult::Failure::OutputRejected,
                "builder for '%s' failed to produce output path '%s'",
                store.printStorePath(drvPath),
                store.printStorePath(requiredPath));

        /* Fix timestamps and permissions. On Windows this makes the path
           read-only, which is what `deleteStalePath` above has to undo. */
        canonicalisePathMetaData(actualPath, {});

        /* Rooted at the output, as the Unix builder does. A store-rooted accessor
           gives the wrong NAR, and on Windows fails: the store dir is not a path. */
        auto narHashAndSize = hashPath(
            SourcePath{makeFSSourceAccessor(actualPath), CanonPath::root},
            FileSerialisationMethod::NixArchive,
            HashAlgorithm::SHA256);

        ValidPathInfo info{StorePath{requiredPath}, UnkeyedValidPathInfo{store, narHashAndSize.hash}};
        info.narSize = narHashAndSize.numBytesDigested;
        info.deriver = drvPath;
        /* No hash rewriting, so an output cannot refer to itself. References to
           inputs are not scanned for either -- see the class comment. */
        info.references = {};

        store.registerValidPaths({{info.path, info}});

        builtOutputs.insert_or_assign(outputName, UnkeyedRealisation{.outPath = requiredPath});
    }

    deletePath(tmpDir);

    return builtOutputs;
}

} // namespace

DerivationBuilderUnique makeDerivationBuilder(
    LocalStore & store,
    std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
    DerivationBuilderParams params,
    HANDLE ioport)
{
    return DerivationBuilderUnique{
        new WindowsDerivationBuilder{store, std::move(miscMethods), std::move(params), ioport}};
}

void DerivationBuilderDeleter::operator()(DerivationBuilder * builder) noexcept
{
    delete builder;
}

} // namespace nix
