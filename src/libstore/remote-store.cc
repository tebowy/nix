#include "serialise.hh"
#include "signals.hh"
#include "path-with-outputs.hh"
#include "gc-store.hh"
#include "remote-fs-accessor.hh"
#include "build-result.hh"
#include "remote-store.hh"
#include "remote-store-connection.hh"
#include "worker-protocol.hh"
#include "worker-protocol-impl.hh"
#include "archive.hh"
#include "globals.hh"
#include "derivations.hh"
#include "pool.hh"
#include "finally.hh"
#include "logging.hh"
#include "filetransfer.hh"
#include <nlohmann/json.hpp>

namespace nix {

/* TODO: Separate these store impls into different files, give them better names */
RemoteStore::RemoteStore(const Params & params)
    : RemoteStoreConfig(params)
    , Store(params)
    , connections(make_ref<Pool<Connection>>(
            std::max(1, (int) maxConnections),
            [this]() {
                auto conn = openConnectionWrapper();
                try {
                    initConnection(*conn);
                } catch (...) {
                    failed = true;
                    throw;
                }
                return conn;
            },
            [this](const ref<Connection> & r) {
                return
                    r->to.good()
                    && r->from.good()
                    && std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - r->startTime).count() < maxConnectionAge;
            }
            ))
{
}


ref<RemoteStore::Connection> RemoteStore::openConnectionWrapper()
{
    if (failed)
        throw Error("opening a connection to remote store '%s' previously failed", getUri());
    try {
        return openConnection();
    } catch (...) {
        failed = true;
        throw;
    }
}


void RemoteStore::initConnection(Connection & conn)
{
    /* Send the magic greeting, check for the reply. */
    try {
        conn.from.endOfFileError = "Nix daemon disconnected unexpectedly (maybe it crashed?)";
        conn.to << WORKER_MAGIC_1;
        conn.to.flush();

        uint64_t magic = readLongLong(conn.from);
        if (magic != WORKER_MAGIC_2)
            throw Error("protocol mismatch");

        conn.from >> conn.daemonVersion;
        if (GET_PROTOCOL_MAJOR(conn.daemonVersion) != GET_PROTOCOL_MAJOR(PROTOCOL_VERSION))
            throw Error("Nix daemon protocol version not supported");
        if (GET_PROTOCOL_MINOR(conn.daemonVersion) < MIN_SUPPORTED_MINOR_WORKER_PROTO_VERSION)
            throw Error("the Nix daemon version is too old");
        conn.to << PROTOCOL_VERSION;

        // Obsolete CPU affinity.
        conn.to << 0;

        conn.to << false; // obsolete reserveSpace

        if (GET_PROTOCOL_MINOR(conn.daemonVersion) >= 33) {
            conn.to.flush();
            conn.daemonNixVersion = readString(conn.from);
        }

        if (GET_PROTOCOL_MINOR(conn.daemonVersion) >= 35) {
            conn.remoteTrustsUs = WorkerProto::Serialise<std::optional<TrustedFlag>>::read(*this, conn);
        } else {
            // We don't know the answer; protocol to old.
            conn.remoteTrustsUs = std::nullopt;
        }

        auto ex = conn.processStderr();
        if (ex) std::rethrow_exception(ex);
    }
    catch (Error & e) {
        throw Error("cannot open connection to remote store '%s': %s", getUri(), e.what());
    }

    setOptions(conn);
}


void RemoteStore::setOptions(Connection & conn)
{
    conn.to << WorkerProto::Op::SetOptions
       << settings.keepFailed
       << settings.keepGoing
       << settings.tryFallback
       << verbosity
       << settings.maxBuildJobs
       << settings.maxSilentTime
       << true
       << (settings.verboseBuild ? lvlError : lvlVomit)
       << 0 // obsolete log type
       << 0 /* obsolete print build trace */
       << settings.buildCores
       << settings.useSubstitutes;

    std::map<std::string, Config::SettingInfo> overrides;
    settings.getSettings(overrides, true); // libstore settings
    fileTransferSettings.getSettings(overrides, true);
    overrides.erase(settings.keepFailed.name);
    overrides.erase(settings.keepGoing.name);
    overrides.erase(settings.tryFallback.name);
    overrides.erase(settings.maxBuildJobs.name);
    overrides.erase(settings.maxSilentTime.name);
    overrides.erase(settings.buildCores.name);
    overrides.erase(settings.useSubstitutes.name);
    overrides.erase(loggerSettings.showTrace.name);
    overrides.erase(experimentalFeatureSettings.experimentalFeatures.name);
    overrides.erase(settings.pluginFiles.name);
    conn.to << overrides.size();
    for (auto & i : overrides)
        conn.to << i.first << i.second.value;

    auto ex = conn.processStderr();
    if (ex) std::rethrow_exception(ex);
}


RemoteStore::ConnectionHandle::~ConnectionHandle()
{
    if (!daemonException && std::uncaught_exceptions()) {
        handle.markBad();
        debug("closing daemon connection because of an exception");
    }
}

void RemoteStore::ConnectionHandle::processStderr(Sink * sink, Source * source, bool flush)
{
    auto ex = handle->processStderr(sink, source, flush);
    if (ex) {
        daemonException = true;
        try {
            std::rethrow_exception(ex);
        } catch (const Error & e) {
            // Nix versions before #4628 did not have an adequate behavior for reporting that the derivation format was upgraded.
            // To avoid having to add compatibility logic in many places, we expect to catch almost all occurrences of the
            // old incomprehensible error here, so that we can explain to users what's going on when their daemon is
            // older than #4628 (2023).
            if (experimentalFeatureSettings.isEnabled(Xp::DynamicDerivations) &&
                GET_PROTOCOL_MINOR(handle->daemonVersion) <= 35)
            {
                auto m = e.msg();
                if (m.find("parsing derivation") != std::string::npos &&
                    m.find("expected string") != std::string::npos &&
                    m.find("Derive([") != std::string::npos)
                    throw Error("%s, this might be because the daemon is too old to understand dependencies on dynamic derivations. Check to see if the raw derivation is in the form '%s'", std::move(m), "DrvWithVersion(..)");
            }
            throw;
        }
    }
}


RemoteStore::ConnectionHandle RemoteStore::getConnection()
{
    return ConnectionHandle(connections->get());
}

void RemoteStore::setOptions()
{
    setOptions(*(getConnection().handle));
}

bool RemoteStore::isValidPathUncached(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::IsValidPath << printStorePath(path);
    conn.processStderr();
    return readInt(conn->from);
}


StorePathSet RemoteStore::queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryValidPaths;
    WorkerProto::write(*this, *conn, paths);
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 27) {
        conn->to << maybeSubstitute;
    }
    conn.processStderr();
    return WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
}


StorePathSet RemoteStore::queryAllValidPaths()
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryAllValidPaths;
    conn.processStderr();
    return WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
}


StorePathSet RemoteStore::querySubstitutablePaths(const StorePathSet & paths)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QuerySubstitutablePaths;
    WorkerProto::write(*this, *conn, paths);
    conn.processStderr();
    return WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
}


void RemoteStore::querySubstitutablePathInfos(const StorePathCAMap & pathsMap, SubstitutablePathInfos & infos)
{
    if (pathsMap.empty()) return;

    auto conn(getConnection());


    conn->to << WorkerProto::Op::QuerySubstitutablePathInfos;
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 22) {
        StorePathSet paths;
        for (auto & path : pathsMap)
            paths.insert(path.first);
        WorkerProto::write(*this, *conn, paths);
    } else
        WorkerProto::write(*this, *conn, pathsMap);
    conn.processStderr();
    size_t count = readNum<size_t>(conn->from);
    for (size_t n = 0; n < count; n++) {
        SubstitutablePathInfo & info(infos[parseStorePath(readString(conn->from))]);
        auto deriver = readString(conn->from);
        if (deriver != "")
            info.deriver = parseStorePath(deriver);
        info.references = WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
        info.downloadSize = readLongLong(conn->from);
        info.narSize = readLongLong(conn->from);
    }
}


std::shared_ptr<const ValidPathInfo> RemoteStore::queryPathInfoUncached(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryPathInfo << printStorePath(path);
    try {
        conn.processStderr();
    } catch (Error & e) {
        // Ugly backwards compatibility hack. TODO(fj#325): remove.
        if (e.msg().find("is not valid") != std::string::npos)
            return nullptr;
        throw;
    }

    bool valid; conn->from >> valid;
    if (!valid) return nullptr;

    return std::make_shared<ValidPathInfo>(
        StorePath{path},
        WorkerProto::Serialise<UnkeyedValidPathInfo>::read(*this, *conn));
}


void RemoteStore::queryReferrers(const StorePath & path,
    StorePathSet & referrers)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryReferrers << printStorePath(path);
    conn.processStderr();
    for (auto & i : WorkerProto::Serialise<StorePathSet>::read(*this, *conn))
        referrers.insert(i);
}


StorePathSet RemoteStore::queryValidDerivers(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryValidDerivers << printStorePath(path);
    conn.processStderr();
    return WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
}


StorePathSet RemoteStore::queryDerivationOutputs(const StorePath & path)
{
    if (GET_PROTOCOL_MINOR(getProtocol()) >= 22) {
        return Store::queryDerivationOutputs(path);
    }
    REMOVE_AFTER_DROPPING_PROTO_MINOR(21);
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryDerivationOutputs << printStorePath(path);
    conn.processStderr();
    return WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
}


std::map<std::string, std::optional<StorePath>> RemoteStore::queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore_)
{
    if (GET_PROTOCOL_MINOR(getProtocol()) >= 22) {
        if (!evalStore_) {
            auto conn(getConnection());
            conn->to << WorkerProto::Op::QueryDerivationOutputMap << printStorePath(path);
            conn.processStderr();
            return WorkerProto::Serialise<std::map<std::string, std::optional<StorePath>>>::read(*this, *conn);
        } else {
            auto & evalStore = *evalStore_;
            auto outputs = evalStore.queryStaticPartialDerivationOutputMap(path);
            // union with the first branch overriding the statically-known ones
            // when non-`std::nullopt`.
            for (auto && [outputName, optPath] : queryPartialDerivationOutputMap(path, nullptr)) {
                if (optPath)
                    outputs.insert_or_assign(std::move(outputName), std::move(optPath));
                else
                    outputs.insert({std::move(outputName), std::nullopt});
            }
            return outputs;
        }
    } else {
        REMOVE_AFTER_DROPPING_PROTO_MINOR(21);
        auto & evalStore = evalStore_ ? *evalStore_ : *this;
        // Fallback for old daemon versions.
        // For floating-CA derivations (and their co-dependencies) this is an
        // under-approximation as it only returns the paths that can be inferred
        // from the derivation itself (and not the ones that are known because
        // the have been built), but as old stores don't handle floating-CA
        // derivations this shouldn't matter
        return evalStore.queryStaticPartialDerivationOutputMap(path);
    }
}

std::optional<StorePath> RemoteStore::queryPathFromHashPart(const std::string & hashPart)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryPathFromHashPart << hashPart;
    conn.processStderr();
    Path path = readString(conn->from);
    if (path.empty()) return {};
    return parseStorePath(path);
}


ref<const ValidPathInfo> RemoteStore::addCAToStore(
    Source & dump,
    std::string_view name,
    ContentAddressMethod caMethod,
    HashType hashType,
    const StorePathSet & references,
    RepairFlag repair)
{
    std::optional<ConnectionHandle> conn_(getConnection());
    auto & conn = *conn_;

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 25) {

        conn->to
            << WorkerProto::Op::AddToStore
            << name
            << caMethod.render(hashType);
        WorkerProto::write(*this, *conn, references);
        conn->to << repair;

        // The dump source may invoke the store, so we need to make some room.
        connections->incCapacity();
        {
            Finally cleanup([&]() { connections->decCapacity(); });
            conn.withFramedSink([&](Sink & sink) {
                dump.drainInto(sink);
            });
        }

        return make_ref<ValidPathInfo>(
            WorkerProto::Serialise<ValidPathInfo>::read(*this, *conn));
    }
    else {
        if (repair) throw Error("repairing is not supported when building through the Nix daemon protocol < 1.25");

        std::visit(overloaded {
            [&](const TextIngestionMethod & thm) -> void {
                if (hashType != htSHA256)
                    throw UnimplementedError("When adding text-hashed data called '%s', only SHA-256 is supported but '%s' was given",
                        name, printHashType(hashType));
                std::string s = dump.drain();
                conn->to << WorkerProto::Op::AddTextToStore << name << s;
                WorkerProto::write(*this, *conn, references);
                conn.processStderr();
            },
            [&](const FileIngestionMethod & fim) -> void {
                conn->to
                    << WorkerProto::Op::AddToStore
                    << name
                    << ((hashType == htSHA256 && fim == FileIngestionMethod::Recursive) ? 0 : 1) /* backwards compatibility hack */
                    << (fim == FileIngestionMethod::Recursive ? 1 : 0)
                    << printHashType(hashType);

                try {
                    conn->to.written = 0;
                    connections->incCapacity();
                    {
                        Finally cleanup([&]() { connections->decCapacity(); });
                        if (fim == FileIngestionMethod::Recursive) {
                            dump.drainInto(conn->to);
                        } else {
                            std::string contents = dump.drain();
                            conn->to << dumpString(contents);
                        }
                    }
                    conn.processStderr();
                } catch (SysError & e) {
                    /* Daemon closed while we were sending the path. Probably OOM
                      or I/O error. */
                    if (e.errNo == EPIPE)
                        try {
                            conn.processStderr();
                        } catch (EndOfFile & e) { }
                    throw;
                }

            }
        }, caMethod.raw);
        auto path = parseStorePath(readString(conn->from));
        // Release our connection to prevent a deadlock in queryPathInfo().
        conn_.reset();
        return queryPathInfo(path);
    }
}


StorePath RemoteStore::addToStoreFromDump(Source & dump, std::string_view name,
      FileIngestionMethod method, HashType hashType, RepairFlag repair, const StorePathSet & references)
{
    return addCAToStore(dump, name, method, hashType, references, repair)->path;
}


void RemoteStore::addToStore(const ValidPathInfo & info, Source & source,
    RepairFlag repair, CheckSigsFlag checkSigs)
{
    auto conn(getConnection());

    conn->to << WorkerProto::Op::AddToStoreNar
             << printStorePath(info.path)
             << (info.deriver ? printStorePath(*info.deriver) : "")
             << info.narHash.to_string(Base16, false);
    WorkerProto::write(*this, *conn, info.references);
    conn->to << info.registrationTime << info.narSize
             << info.ultimate << info.sigs << renderContentAddress(info.ca)
             << repair << !checkSigs;

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 23) {
        conn.withFramedSink([&](Sink & sink) {
            copyNAR(source, sink);
        });
    } else {
        conn.processStderr(0, &source);
    }
}


void RemoteStore::addMultipleToStore(
    PathsSource & pathsToCopy,
    Activity & act,
    RepairFlag repair,
    CheckSigsFlag checkSigs)
{
    auto remoteVersion = getProtocol();

    auto source = sinkToSource([&](Sink & sink) {
        sink << pathsToCopy.size();
        for (auto & [pathInfo, pathSource] : pathsToCopy) {
            WorkerProto::Serialise<ValidPathInfo>::write(*this,
                 WorkerProto::WriteConn {sink, remoteVersion},
                 pathInfo);
            pathSource->drainInto(sink);
        }
    });

    addMultipleToStore(*source, repair, checkSigs);
}

void RemoteStore::addMultipleToStore(
    Source & source,
    RepairFlag repair,
    CheckSigsFlag checkSigs)
{
    if (GET_PROTOCOL_MINOR(getConnection()->daemonVersion) >= 32) {
        auto conn(getConnection());
        conn->to
            << WorkerProto::Op::AddMultipleToStore
            << repair
            << !checkSigs;
        conn.withFramedSink([&](Sink & sink) {
            source.drainInto(sink);
        });
    } else
        Store::addMultipleToStore(source, repair, checkSigs);
}


StorePath RemoteStore::addTextToStore(
    std::string_view name,
    std::string_view s,
    const StorePathSet & references,
    RepairFlag repair)
{
    StringSource source(s);
    return addCAToStore(source, name, TextIngestionMethod {}, htSHA256, references, repair)->path;
}

void RemoteStore::registerDrvOutput(const Realisation & info)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::RegisterDrvOutput;
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 31) {
        REMOVE_AFTER_DROPPING_PROTO_MINOR(30);
        conn->to << info.id.to_string();
        conn->to << std::string(info.outPath.to_string());
    } else {
        WorkerProto::write(*this, *conn, info);
    }
    conn.processStderr();
}

std::shared_ptr<const Realisation> RemoteStore::queryRealisationUncached(const DrvOutput & id)
{
    auto conn(getConnection());

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 27) {
        warn("the daemon is too old to support content-addressed derivations, please upgrade it to 2.4");
        return nullptr;
    }

    conn->to << WorkerProto::Op::QueryRealisation;
    conn->to << id.to_string();
    conn.processStderr();

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 31) {
        auto outPaths = WorkerProto::Serialise<std::set<StorePath>>::read(
            *this, *conn);
        if (outPaths.empty())
            return nullptr;
        return std::make_shared<const Realisation>(Realisation { .id = id, .outPath = *outPaths.begin() });
    } else {
        auto realisations = WorkerProto::Serialise<std::set<Realisation>>::read(
            *this, *conn);
        if (realisations.empty())
            return nullptr;
        return std::make_shared<const Realisation>(*realisations.begin());
    }
}

void RemoteStore::copyDrvsFromEvalStore(
    const std::vector<DerivedPath> & paths,
    std::shared_ptr<Store> evalStore)
{
    if (evalStore && evalStore.get() != this) {
        /* The remote doesn't have a way to access evalStore, so copy
           the .drvs. */
        RealisedPath::Set drvPaths2;
        for (const auto & i : paths) {
            std::visit(overloaded {
                [&](const DerivedPath::Opaque & bp) {
                    // Do nothing, path is hopefully there already
                },
                [&](const DerivedPath::Built & bp) {
                    drvPaths2.insert(bp.drvPath->getBaseStorePath());
                },
            }, i.raw());
        }
        copyClosure(*evalStore, *this, drvPaths2);
    }
}

void RemoteStore::buildPaths(const std::vector<DerivedPath> & drvPaths, BuildMode buildMode, std::shared_ptr<Store> evalStore)
{
    copyDrvsFromEvalStore(drvPaths, evalStore);

    auto conn(getConnection());
    conn->to << WorkerProto::Op::BuildPaths;
    WorkerProto::write(*this, *conn, drvPaths);
    conn->to << buildMode;
    conn.processStderr();
    readInt(conn->from);
}

std::vector<KeyedBuildResult> RemoteStore::buildPathsWithResults(
    const std::vector<DerivedPath> & paths,
    BuildMode buildMode,
    std::shared_ptr<Store> evalStore)
{
    copyDrvsFromEvalStore(paths, evalStore);

    std::optional<ConnectionHandle> conn_(getConnection());
    auto & conn = *conn_;

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 34) {
        conn->to << WorkerProto::Op::BuildPathsWithResults;
        WorkerProto::write(*this, *conn, paths);
        conn->to << buildMode;
        conn.processStderr();
        return WorkerProto::Serialise<std::vector<KeyedBuildResult>>::read(*this, *conn);
    } else {
        REMOVE_AFTER_DROPPING_PROTO_MINOR(33);
        // Avoid deadlock.
        conn_.reset();

        // Note: this throws an exception if a build/substitution
        // fails, but meh.
        buildPaths(paths, buildMode, evalStore);

        std::vector<KeyedBuildResult> results;

        for (auto & path : paths) {
            std::visit(
                overloaded {
                    [&](const DerivedPath::Opaque & bo) {
                        results.push_back(KeyedBuildResult {
                            {
                                .status = BuildResult::Substituted,
                            },
                            /* .path = */ bo,
                        });
                    },
                    [&](const DerivedPath::Built & bfd) {
                        KeyedBuildResult res {
                            {
                                .status = BuildResult::Built
                            },
                            /* .path = */ bfd,
                        };

                        OutputPathMap outputs;
                        auto drvPath = resolveDerivedPath(*evalStore, *bfd.drvPath);
                        auto drv = evalStore->readDerivation(drvPath);
                        const auto outputHashes = staticOutputHashes(*evalStore, drv); // FIXME: expensive
                        auto built = resolveDerivedPath(*this, bfd, &*evalStore);
                        for (auto & [output, outputPath] : built) {
                            auto outputHash = get(outputHashes, output);
                            if (!outputHash)
                                throw Error(
                                    "the derivation '%s' doesn't have an output named '%s'",
                                    printStorePath(drvPath), output);
                            auto outputId = DrvOutput{ *outputHash, output };
                            if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
                                auto realisation =
                                    queryRealisation(outputId);
                                if (!realisation)
                                    throw MissingRealisation(outputId);
                                res.builtOutputs.emplace(output, *realisation);
                            } else {
                                res.builtOutputs.emplace(
                                    output,
                                    Realisation {
                                        .id = outputId,
                                        .outPath = outputPath,
                                    });
                            }
                        }

                        results.push_back(res);
                    }
                },
                path.raw());
        }

        return results;
    }
}


BuildResult RemoteStore::buildDerivation(const StorePath & drvPath, const BasicDerivation & drv,
    BuildMode buildMode)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::BuildDerivation << printStorePath(drvPath);
    writeDerivation(conn->to, *this, drv);
    conn->to << buildMode;
    conn.processStderr();
    return WorkerProto::Serialise<BuildResult>::read(*this, *conn);
}


void RemoteStore::ensurePath(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::EnsurePath << printStorePath(path);
    conn.processStderr();
    readInt(conn->from);
}


void RemoteStore::addTempRoot(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::AddTempRoot << printStorePath(path);
    conn.processStderr();
    readInt(conn->from);
}


Roots RemoteStore::findRoots(bool censor)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::FindRoots;
    conn.processStderr();
    size_t count = readNum<size_t>(conn->from);
    Roots result;
    while (count--) {
        Path link = readString(conn->from);
        auto target = parseStorePath(readString(conn->from));
        result[std::move(target)].emplace(link);
    }
    return result;
}


void RemoteStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    auto conn(getConnection());

    conn->to
        << WorkerProto::Op::CollectGarbage << options.action;
    WorkerProto::write(*this, *conn, options.pathsToDelete);
    conn->to << options.ignoreLiveness
        << options.maxFreed
        /* removed options */
        << 0 << 0 << 0;

    conn.processStderr();

    results.paths = readStrings<PathSet>(conn->from);
    results.bytesFreed = readLongLong(conn->from);
    readLongLong(conn->from); // obsolete

    {
        auto state_(Store::state.lock());
        state_->pathInfoCache.clear();
    }
}


void RemoteStore::optimiseStore()
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::OptimiseStore;
    conn.processStderr();
    readInt(conn->from);
}


bool RemoteStore::verifyStore(bool checkContents, RepairFlag repair)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::VerifyStore << checkContents << repair;
    conn.processStderr();
    return readInt(conn->from);
}


void RemoteStore::addSignatures(const StorePath & storePath, const StringSet & sigs)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::AddSignatures << printStorePath(storePath) << sigs;
    conn.processStderr();
    readInt(conn->from);
}


void RemoteStore::queryMissing(const std::vector<DerivedPath> & targets,
    StorePathSet & willBuild, StorePathSet & willSubstitute, StorePathSet & unknown,
    uint64_t & downloadSize, uint64_t & narSize)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::QueryMissing;
    WorkerProto::write(*this, *conn, targets);
    conn.processStderr();
    willBuild = WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
    willSubstitute = WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
    unknown = WorkerProto::Serialise<StorePathSet>::read(*this, *conn);
    conn->from >> downloadSize >> narSize;
}


void RemoteStore::addBuildLog(const StorePath & drvPath, std::string_view log)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::AddBuildLog << drvPath.to_string();
    StringSource source(log);
    conn.withFramedSink([&](Sink & sink) {
        source.drainInto(sink);
    });
    readInt(conn->from);
}


std::optional<std::string> RemoteStore::getVersion()
{
    auto conn(getConnection());
    return conn->daemonNixVersion;
}


void RemoteStore::connect()
{
    auto conn(getConnection());
}


unsigned int RemoteStore::getProtocol()
{
    auto conn(connections->get());
    return conn->daemonVersion;
}

std::optional<TrustedFlag> RemoteStore::isTrustedClient()
{
    auto conn(getConnection());
    return conn->remoteTrustsUs;
}


RemoteStore::Connection::~Connection()
{
    try {
        to.flush();
    } catch (...) {
        ignoreException();
    }
}

void RemoteStore::narFromPath(const StorePath & path, Sink & sink)
{
    auto conn(connections->get());
    conn->to << WorkerProto::Op::NarFromPath << printStorePath(path);
    conn->processStderr();
    copyNAR(conn->from, sink);
}

ref<FSAccessor> RemoteStore::getFSAccessor()
{
    return make_ref<RemoteFSAccessor>(ref<Store>(shared_from_this()));
}

static Logger::Fields readFields(Source & from)
{
    Logger::Fields fields;
    size_t size = readInt(from);
    for (size_t n = 0; n < size; n++) {
        auto type = (decltype(Logger::Field::type)) readInt(from);
        if (type == Logger::Field::tInt)
            fields.push_back(readNum<uint64_t>(from));
        else if (type == Logger::Field::tString)
            fields.push_back(readString(from));
        else
            throw Error("got unsupported field type %x from Nix daemon", (int) type);
    }
    return fields;
}


std::exception_ptr RemoteStore::Connection::processStderr(Sink * sink, Source * source, bool flush)
{
    if (flush)
        to.flush();

    while (true) {

        auto msg = readNum<uint64_t>(from);

        if (msg == STDERR_WRITE) {
            auto s = readString(from);
            if (!sink) throw Error("no sink");
            (*sink)(s);
        }

        else if (msg == STDERR_READ) {
            if (!source) throw Error("no source");
            size_t len = readNum<size_t>(from);
            auto buf = std::make_unique<char[]>(len);
            to << std::string_view((const char *) buf.get(), source->read(buf.get(), len));
            to.flush();
        }

        else if (msg == STDERR_ERROR) {
            if (GET_PROTOCOL_MINOR(daemonVersion) >= 26) {
                return std::make_exception_ptr(readError(from));
            } else {
                auto error = readString(from);
                unsigned int status = readInt(from);
                return std::make_exception_ptr(Error(status, error));
            }
        }

        else if (msg == STDERR_NEXT)
            printError(chomp(readString(from)));

        else if (msg == STDERR_START_ACTIVITY) {
            auto act = readNum<ActivityId>(from);
            auto lvl = (Verbosity) readInt(from);
            auto type = (ActivityType) readInt(from);
            auto s = readString(from);
            auto fields = readFields(from);
            auto parent = readNum<ActivityId>(from);
            logger->startActivity(act, lvl, type, s, fields, parent);
        }

        else if (msg == STDERR_STOP_ACTIVITY) {
            auto act = readNum<ActivityId>(from);
            logger->stopActivity(act);
        }

        else if (msg == STDERR_RESULT) {
            auto act = readNum<ActivityId>(from);
            auto type = (ResultType) readInt(from);
            auto fields = readFields(from);
            logger->result(act, type, fields);
        }

        else if (msg == STDERR_LAST)
            break;

        else
            throw Error("got unknown message type %x from Nix daemon", msg);
    }

    return nullptr;
}

void RemoteStore::ConnectionHandle::withFramedSink(std::function<void(Sink & sink)> fun)
{
    (*this)->to.flush();

    std::exception_ptr ex;

    /* Handle log messages / exceptions from the remote on a separate
       thread. */
    std::thread stderrThread([&]()
    {
        try {
            ReceiveInterrupts receiveInterrupts;
            processStderr(nullptr, nullptr, false);
        } catch (...) {
            ex = std::current_exception();
        }
    });

    Finally joinStderrThread([&]()
    {
        if (stderrThread.joinable()) {
            stderrThread.join();
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (...) {
                    ignoreException();
                }
            }
        }
    });

    {
        FramedSink sink((*this)->to, ex);
        fun(sink);
        sink.flush();
    }

    stderrThread.join();
    if (ex)
        std::rethrow_exception(ex);
}

}
