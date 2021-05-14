
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <optional>
#include <vector>

#include <stdlib.h>
#include <sys/signal.h>

#include <fmt/format.h>
#include <boost/algorithm/string.hpp>
#include <boost/process.hpp>
#include <boost/program_options.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/uuid/uuid_generators.hpp>

#include "tempdir.hpp"

namespace bp = boost::process;
namespace fs = std::filesystem;
namespace po = boost::program_options;

using Context = struct {
    bool preserve_temp;
    int threads;
    bool verbose;
};

static void verbose(const Context& ctx, const std::string& msg) {
    if (ctx.verbose) {
        std::cout << fmt::format("{}\n", msg);
    }
}
#define VERBOSE(ctx, msg, ...) \
    verbose(ctx, fmt::format(FMT_STRING(msg), ##__VA_ARGS__))

[[noreturn]] [[maybe_unused]] static void error(const std::string& msg) {
    std::cerr << fmt::format("{}\n", msg);
    exit(1);
}
[[noreturn]] static void error_sys(int syserr, const std::string& msg) {
    std::cerr << fmt::format("{}: {}\n", msg, strerror(syserr));
    exit(1);
}

#define EFMT(msg, ...) error(fmt::format(FMT_STRING(msg), ##__VA_ARGS__))
#define EFMT_SYS(err, msg, ...) \
    error_sys(err, fmt::format(FMT_STRING(msg), ##__VA_ARGS__))

using clean_function = std::function<void()>;

static std::optional<clean_function> cleanup;

static void sig_handler(int sig,
                        siginfo_t* info,
                        [[maybe_unused]] void* uctx) {
    if (cleanup) {
        (*cleanup)();
        cleanup.reset();
    }
}

static bool exportfs(const Context& ctx) {
    auto efs = bp::search_path("exportfs");
    std::error_code ec;
    VERBOSE(ctx, "run exportfs");
    bp::system(efs.native() + " -ra", ec);
    if (ec.value() != 0) {
        EFMT_SYS(ec.value(), "exportfs failed");
    }
    return true;
}

int main(int argc, char* argv[]) {
    Context ctx{};
    std::error_code ec{};

    bool exit_code = EXIT_SUCCESS;

    auto desc = po::options_description("Allowed options");
    desc.add_options()                      //
        ("help,h", "produce help message")  //
        ("preserve,p", po::bool_switch(),
         "preserve temporary files and directories")  //
        ("threads,t", po::value<int>(&ctx.threads)->default_value(128),
         "the number of concurrent commands to issue")  //
        ("verbose,v", po::bool_switch(),
         "show verbose output")  //
        ;

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return EXIT_FAILURE;
    }
    ctx.preserve_temp = vm["preserve"].as<bool>();
    ctx.verbose = vm["verbose"].as<bool>();

    // Self-deleting temporary directory.
    auto tdobj = TemporaryDirectory("paramount");
    if (ctx.preserve_temp) {
        tdobj.PreserveContents();
    }
    auto tmpdir = tdobj.Dir();

    auto exports = fs::path("/etc/exports.d/paramount.exports");

    cleanup = [&]() {
        VERBOSE(ctx, "cleanup");
        VERBOSE(ctx, "unmount all NFS mounts");
        bp::system("umount -a -t nfs -l");
        VERBOSE(ctx, "remove export file");
        fs::remove(exports);
        exportfs(ctx);
        VERBOSE(ctx, "remove temp dir");
        tdobj.DeleteNow();
    };

    struct sigaction crashaction {};
    crashaction.sa_sigaction = sig_handler;
    crashaction.sa_flags |= SA_SIGINFO;
    for (auto signal : {SIGINT}) {
        if (sigaction(signal, &crashaction, NULL) < 0) {
            EFMT_SYS(errno, "Unable to install signal handler");
        }
    }

    try {
        // Map from mount to client mountpoint.
        std::unordered_map<std::string, std::string> m_to_c{};

        // Create mount directories.
        auto mountdir = tmpdir / "mount";
        if (!fs::create_directory(mountdir, ec)) {
            EFMT_SYS(ec.value(), "Failed to create mount root directory {}",
                     mountdir.native());
        }

        auto mdir = std::vector<fs::path>{};
        for (int d = 0; d < ctx.threads; d++) {
            auto newdir = mountdir / fmt::format(FMT_STRING("d{:04}"), d);
            if (!fs::create_directory(newdir, ec)) {
                EFMT_SYS(ec.value(), "Failed to create mount directory {}",
                         newdir.native());
            }
            VERBOSE(ctx, "Created mount {}", newdir.native());
            mdir.push_back(newdir);
        }

        // Export mount directories.
        auto ef = std::ofstream{};
        ef.exceptions(std::ofstream::failbit);
        ef.open(exports, std::ios_base::trunc);
        ef << "### BEGIN paramount\n";

        auto muuid = std::vector<std::string>{};

        for (int d = 0; d < ctx.threads; d++) {
            auto us = fmt::format(
                FMT_STRING("00000000-0000-0000-0000-00000000{:04x}"), d);
            muuid.push_back(us);
            auto opts =
                fmt::format("rw,no_subtree_check,no_root_squash,fsid={}", us);
            VERBOSE(ctx, "options: {}", opts);
            ef << mdir[d] << "\t*(" << opts << ")\n";
        }
        ef << "### END paramount\n";
        ef.close();

        // Configure mounts.
        exportfs(ctx);

        // Create client directories.
        auto clientdir = tmpdir / "client";
        if (!fs::create_directory(clientdir, ec)) {
            EFMT_SYS(ec.value(), "Failed to create client root directory {}",
                     clientdir.native());
        }

        auto cdir = std::vector<fs::path>{};
        for (int d = 0; d < ctx.threads; d++) {
            auto newdir = clientdir / fmt::format(FMT_STRING("d{:04}"), d);
            if (!fs::create_directory(newdir, ec)) {
                EFMT_SYS(ec.value(), "Failed to create client directory {}",
                         newdir.native());
            }
            VERBOSE(ctx, "Created client mountpoint {}", newdir.native());
            cdir.push_back(newdir);
        }

        // Map together.
        for (int d = 0; d < ctx.threads; d++) {
            m_to_c[mdir[d]] = cdir[d];
        }

        // Mount each.
        using mountfut = std::future<int>;
        auto mounters = std::vector<mountfut>{};

        auto start_barrier = boost::barrier(ctx.threads + 1);
        auto mountp = bp::search_path("mount");

        for (int d = 0; d < ctx.threads; d++) {
            VERBOSE(ctx, "Start mounter {}", d);
            mounters.emplace_back(std::async(
                std::launch::async, [cd = cdir[d], ctx, d, md = mdir[d],
                                     mountp, &start_barrier, u = muuid[d]]() {
                    start_barrier.wait();
                    VERBOSE(ctx, "mounter {} mdir {} mount on cdir {}", d,
                            md.native(), cd.native());
                    auto cmdline = fmt::format(
                        FMT_STRING(
                            "{} -t nfs -o rw,nfsvers=3 127.0.0.1:{} {}"),
                        mountp.native(), md.native(), cd.native());
                    VERBOSE(ctx, "mounter {} cmd '{}'", d, cmdline);
                    std::error_code ec;
                    bp::system(cmdline, ec);
                    if (ec.value() != 0) {
                        return ec.value();
                    }
                    return 0;  // XXX
                }));
        }

        start_barrier.wait();

        int failures = 0;
        for (auto& future : mounters) {
            if (future.get() != 0) {
                failures++;
            }
        }
        if (failures) {
            std::cerr << "Got " << failures << " mount failures\n";
        }

        // Scan /proc/mounts.
        VERBOSE(ctx, "Scan mounts");
        auto mstr = std::ifstream{};
        auto old_e = mstr.exceptions();
        mstr.exceptions(std::iostream::failbit);
        mstr.open("/proc/self/mounts");
        mstr.exceptions(old_e);

        auto mline = std::vector<std::string>{};
        for (std::string line; std::getline(mstr, line);)
            mline.push_back(line);
        mstr.close();

        for (const auto& mount : mline) {
            auto fields = std::vector<std::string>{};
            // fields:
            // 0      1          2          3       4        5
            // device mountpoint filesystem options dontcare dontcare
            boost::split(fields, mount, boost::is_any_of(" "),
                         boost::token_compress_on);
            if (fields[1] != "nfs" && fields[1] != "nfs4") {
                continue;
            }

            // Check mount-to-client-mountpoint.
            auto m = fields[0];
            auto c = fields[1];
            auto srch = m_to_c.find(m);
            if (srch == m_to_c.end()) {
                EFMT("Mount '{}' not found in map", m);
            }
            if (srch->second != c) {
                EFMT("Mount '{}' expected mountpoint {} found {}", m, c,
                     srch->second);
            }
        }
        VERBOSE(ctx, "Mounts check out");

    } catch (std::exception& e) {
        std::cerr << "Caught exception: " << e.what() << "\n";
        exit_code = EXIT_FAILURE;
    }

    if (cleanup) {
        (*cleanup)();
    }

    return exit_code;
}
