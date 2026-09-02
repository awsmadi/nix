#include "nix/util/processes.hh"
#include "nix/util/current-process.hh"
#include "nix/util/environment-variables.hh"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

namespace nix {

/* ----------------------------------------------------------------------------
 * statusOk
 * --------------------------------------------------------------------------*/

TEST(statusOk, zeroIsOk)
{
    ASSERT_EQ(statusOk(0), true);
    ASSERT_EQ(statusOk(1), false);
}

/* ----------------------------------------------------------------------------
 * runProgram
 * --------------------------------------------------------------------------*/

TEST(runProgram, worksTrivial)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    for (bool isInteractive : {false, true}) {
        std::string output;
        ASSERT_NO_THROW({
            output = runProgram(
                *self,
                /*lookupPath=*/false,
                {
                    OS_STR("__util_test_spawn_trivial"),
                },
                /*isInteractive=*/isInteractive);
        });
        ASSERT_EQ(output, "hello");
    }
}

#ifdef _WIN32
#  define NIX_EXECUTABLE_EXTENSION ".exe"
/* FIXME: runProgram reports spawn errors as WinError, while other
   platforms conflate everything into ExecError. */
#  define NIX_SPAWN_EXCEPTION windows::WinError
#else
#  define NIX_EXECUTABLE_EXTENSION ""
#  define NIX_SPAWN_EXCEPTION ExecError
#endif

TEST(runProgram, nonexistent)
{
    /* For now spawn failures get reported as ExecError. TODO: Report the proper error that's less
       confusing. */
    ASSERT_THROW(
        runProgram("/this/path/really/should/not/exist/for/real" NIX_EXECUTABLE_EXTENSION), NIX_SPAWN_EXCEPTION);
}

TEST(runProgram2, nonexistent)
{
    ASSERT_THROW(
        {
            runProgram2({
                .program = "/this/path/really/should/not/exist/for/real" NIX_EXECUTABLE_EXTENSION,
            });
        },
        NIX_SPAWN_EXCEPTION);
}

#ifndef _WIN32 /* Leaking file descriptors into the child isn't a concern on windows. */

TEST(runProgram2, leakedFDsAreClosed)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);
    int fds[2];
    ASSERT_NE(::pipe(fds), -1);

    AutoCloseFD readSide = fds[0];
    AutoCloseFD writeSide = fds[1];

    /* Test that both fds are closed in the child. Just the one isn't always
       enough if the read side is assigned to 3 (also the fd of the relocated pipe in
       the child on linux that gets dup3-ed into). */
    ASSERT_NO_THROW(runProgram2({
        .program = *self,
        .args = {"__util_test_spawn_leaked_fds"},
        .environment = OsStringMap{{
            "NIX_CHILD_FDS_SHOULD_BE_CLOSED",
            fmt("%d,%d", readSide.get(), writeSide.get()),
        }},
    }));
}

#endif

/* ----------------------------------------------------------------------------
 * The `RunOptions` contract
 *
 * Each field below was previously unexercised, so a refactor of the spawn path
 * could drop one and stay green. These are deliberately behavioural rather than
 * structural: they observe the child, not the options struct.
 *
 * The child in every case is this test binary re-execed with a marker argument;
 * see the dispatch in `main.cc`.
 *
 * One footgun, found the hard way: the two `environment` tests replace the
 * child's environment wholesale, which drops `LD_LIBRARY_PATH`. That is correct
 * and is the point of the test, but it means the child can only find its shared
 * libraries via RPATH. Running this binary relocated from its build tree, with
 * the libraries reachable only through `LD_LIBRARY_PATH`, makes those two fail
 * in the loader rather than in the assertion.
 * --------------------------------------------------------------------------*/

TEST(runProgram2, nonZeroExitIsReportedWithItsStatus)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    /* `ExecError::status` is a raw wait status, not an exit code, so assert
       through `statusOk` rather than comparing it to 3 directly. */
    try {
        runProgram2({
            .program = *self,
            .lookupPath = false,
            .args = {OS_STR("__util_test_spawn_exit_code"), OS_STR("3")},
        });
        FAIL() << "expected ExecError for a child that exited 3";
    } catch (ExecError & e) {
        ASSERT_FALSE(statusOk(e.status));
#ifndef _WIN32
        ASSERT_TRUE(WIFEXITED(e.status));
        ASSERT_EQ(WEXITSTATUS(e.status), 3);
#endif
    }
}

TEST(runProgram, capturesStandardOutAndNotStandardError)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    auto [status, output] = runProgram({
        .program = *self,
        .lookupPath = false,
        .args = {OS_STR("__util_test_spawn_streams")},
    });

    ASSERT_TRUE(statusOk(status));
    /* The child writes "out:" to stdout and "err:" to stderr. Without
       `mergeStderrToStdout` only the former is captured. */
    ASSERT_EQ(output, "out:");
}

TEST(runProgram, mergeStderrToStdoutCapturesBoth)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    auto [status, output] = runProgram({
        .program = *self,
        .lookupPath = false,
        .args = {OS_STR("__util_test_spawn_streams")},
        .mergeStderrToStdout = true,
    });

    ASSERT_TRUE(statusOk(status));
    /* Assert containment rather than an exact string: the two streams are
       separately buffered, so their interleaving is not guaranteed. */
    ASSERT_THAT(output, ::testing::HasSubstr("out:"));
    ASSERT_THAT(output, ::testing::HasSubstr("err:"));
}

TEST(runProgram, environmentReplacesRatherThanExtends)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    setEnv("NIX_TEST_SHOULD_NOT_REACH_CHILD", "leaked");
    ASSERT_EQ(getEnv("NIX_TEST_SHOULD_NOT_REACH_CHILD"), std::optional<std::string>("leaked"));

    /* Supplying `environment` replaces the child's environment wholesale, so a
       variable set in this process must not be visible to the child. */
    auto [status, output] = runProgram({
        .program = *self,
        .lookupPath = false,
        .args = {OS_STR("__util_test_spawn_print_env"), OS_STR("NIX_TEST_SHOULD_NOT_REACH_CHILD")},
        .environment = OsStringMap{{OS_STR("SOMETHING_ELSE"), OS_STR("1")}},
    });

    ASSERT_TRUE(statusOk(status));
    ASSERT_EQ(output, "<unset>");
}

TEST(runProgram, environmentPassesTheGivenVariable)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    auto [status, output] = runProgram({
        .program = *self,
        .lookupPath = false,
        .args = {OS_STR("__util_test_spawn_print_env"), OS_STR("NIX_TEST_PASSED_THROUGH")},
        .environment = OsStringMap{{OS_STR("NIX_TEST_PASSED_THROUGH"), OS_STR("visible")}},
    });

    ASSERT_TRUE(statusOk(status));
    ASSERT_EQ(output, "visible");
}

TEST(runProgram, chdirIsHonoured)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    auto target = std::filesystem::canonical(std::filesystem::temp_directory_path());
    /* Positive control: if the test already runs there, the assertion below
       would hold whether or not `chdir` did anything. */
    ASSERT_NE(std::filesystem::canonical(std::filesystem::current_path()), target);

    auto [status, output] = runProgram({
        .program = *self,
        .lookupPath = false,
        .args = {OS_STR("__util_test_spawn_print_cwd")},
        .chdir = target,
    });

    ASSERT_TRUE(statusOk(status));
    ASSERT_EQ(std::filesystem::canonical(output), target);
}

#ifndef _WIN32 /* `RunOptions::argv0` is Unix-only. */

TEST(runProgram, argv0IsHonoured)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    auto [status, output] = runProgram({
        .program = *self,
        .lookupPath = false,
        .args = {OS_STR("__util_test_spawn_print_argv0")},
        .argv0 = "not-the-program-name",
    });

    ASSERT_TRUE(statusOk(status));
    ASSERT_EQ(output, "not-the-program-name");
}

#endif

#undef NIX_EXECUTABLE_EXTENSION
#undef NIX_SPAWN_EXCEPTION

} // namespace nix
