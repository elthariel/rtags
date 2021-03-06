/* This file is part of RTags (http://rtags.net).

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#ifndef Project_h
#define Project_h

#include <cstdint>
#include <mutex>

#include "Diagnostic.h"
#include "FileMap.h"
#include "IndexerJob.h"
#include "IndexMessage.h"
#include "QueryMessage.h"
#include "rct/EmbeddedLinkedList.h"
#include "rct/FileSystemWatcher.h"
#include "rct/Flags.h"
#include "rct/Path.h"
#include "rct/StopWatch.h"
#include "rct/Timer.h"
#include "RTags.h"

class Connection;
class Dirty;
class FileManager;
class IndexDataMessage;
class Match;
class RestoreThread;
struct DependencyNode
{
    DependencyNode(uint32_t f)
        : fileId(f)
    {}

    void include(DependencyNode *dependee)
    {
        assert(!includes.contains(dependee->fileId) || includes.value(dependee->fileId) == dependee);
        includes[dependee->fileId] = dependee;
        assert(!dependee->dependents.contains(fileId) || dependee->dependents.value(fileId) == this);
        dependee->dependents[fileId] = this;
    }

    Dependencies dependents, includes;
    uint32_t fileId;
};
class Project : public std::enable_shared_from_this<Project>
{
public:
    Project(const Path &path);
    ~Project();
    bool init();

    std::shared_ptr<FileManager> fileManager() const { return mFileManager; }

    Path path() const { return mPath; }
    void setCompilationDatabaseInfo(const Path &dir,
                                    const List<Path> &pathEnvironment,
                                    Flags<IndexMessage::Flag> flags);

    bool match(const Match &match, bool *indexed = 0) const;

    enum FileMapType {
        Symbols,
        SymbolNames,
        Targets,
        Usrs
    };
    static const char *fileMapName(FileMapType type)
    {
        switch (type) {
        case Symbols:
            return "symbols";
        case SymbolNames:
            return "symnames";
        case Targets:
            return "targets";
        case Usrs:
            return "usrs";
        }
        return 0;
    }
    std::shared_ptr<FileMap<String, Set<Location> > > openSymbolNames(uint32_t fileId, String *err = 0)
    {
        assert(mFileMapScope);
        return mFileMapScope->openFileMap<String, Set<Location> >(SymbolNames, fileId, mFileMapScope->symbolNames, err);
    }
    std::shared_ptr<FileMap<Location, Symbol> > openSymbols(uint32_t fileId, String *err = 0)
    {
        assert(mFileMapScope);
        return mFileMapScope->openFileMap<Location, Symbol>(Symbols, fileId, mFileMapScope->symbols, err);
    }
    std::shared_ptr<FileMap<String, Set<Location> > > openTargets(uint32_t fileId, String *err = 0)
    {
        assert(mFileMapScope);
        return mFileMapScope->openFileMap<String, Set<Location> >(Targets, fileId, mFileMapScope->targets, err);
    }
    std::shared_ptr<FileMap<String, Set<Location> > > openUsrs(uint32_t fileId, String *err = 0)
    {
        assert(mFileMapScope);
        return mFileMapScope->openFileMap<String, Set<Location> >(Usrs, fileId, mFileMapScope->usrs, err);
    }

    enum DependencyMode {
        DependsOnArg,
        ArgDependsOn
    };

    Set<uint32_t> dependencies(uint32_t fileId, DependencyMode mode) const;
    bool dependsOn(uint32_t source, uint32_t header) const;
    String dumpDependencies(uint32_t fileId,
                            const List<String> &args = List<String>(),
                            Flags<QueryMessage::Flag> flags = Flags<QueryMessage::Flag>()) const;
    const Hash<uint32_t, DependencyNode*> &dependencies() const { return mDependencies; }
    DependencyNode *dependencyNode(uint32_t fileId) const { return mDependencies.value(fileId); }

    static bool readSources(const Path &path, Sources &sources, String *error);
    enum SymbolMatchType {
        Exact,
        Wildcard,
        StartsWith
    };
    void findSymbols(const String &symbolName,
                     const std::function<void(SymbolMatchType, const String &, const Set<Location> &)> &func,
                     Flags<QueryMessage::Flag> queryFlags,
                     uint32_t fileFilter = 0);

    static bool matchSymbolName(const String &pattern, const String &symbolName, String::CaseSensitivity cs)
    {
        return Rct::wildCmp(pattern.constData(), symbolName.constData(), cs);
    }

    Symbol findSymbol(const Location &location, int *index = 0);
    Set<Symbol> findTargets(const Location &location) { return findTargets(findSymbol(location)); }
    Set<Symbol> findTargets(const Symbol &symbol);
    Symbol findTarget(const Location &location) { return RTags::bestTarget(findTargets(location)); }
    Symbol findTarget(const Symbol &symbol) { return RTags::bestTarget(findTargets(symbol)); }
    Set<Symbol> findAllReferences(const Location &location) { return findAllReferences(findSymbol(location)); }
    Set<Symbol> findAllReferences(const Symbol &symbol);
    Set<Symbol> findCallers(const Location &location) { return findCallers(findSymbol(location)); }
    Set<Symbol> findCallers(const Symbol &symbol);
    Set<Symbol> findVirtuals(const Location &location) { return findVirtuals(findSymbol(location)); }
    Set<Symbol> findVirtuals(const Symbol &symbol);
    Set<String> findTargetUsrs(const Location &loc);
    Set<Symbol> findSubclasses(const Symbol &symbol);

    Set<Symbol> findByUsr(const String &usr, uint32_t fileId, DependencyMode mode, const Location &filtered = Location());

    Path sourceFilePath(uint32_t fileId, const char *path = "") const;

    List<RTags::SortedSymbol> sort(const Set<Symbol> &symbols,
                                   Flags<QueryMessage::Flag> flags = Flags<QueryMessage::Flag>());

    const Files &files() const { return mFiles; }
    Files &files() { return mFiles; }

    const Set<uint32_t> &suspendedFiles() const;
    bool toggleSuspendFile(uint32_t file);
    bool isSuspended(uint32_t file) const;
    void clearSuspendedFiles();

    bool isIndexed(uint32_t fileId) const;

    void index(const std::shared_ptr<IndexerJob> &job);
    List<Source> sources(uint32_t fileId) const;
    bool hasSource(uint32_t fileId) const;
    bool isActiveJob(uint64_t key) { return !key || mActiveJobs.contains(key); }
    inline bool visitFile(uint32_t fileId, const Path &path, uint64_t id);
    inline void releaseFileIds(const Set<uint32_t> &fileIds);
    String fixIts(uint32_t fileId) const;
    int reindex(const Match &match,
                const std::shared_ptr<QueryMessage> &query,
                const std::shared_ptr<Connection> &wait);
    int remove(const Match &match);
    void onJobFinished(const std::shared_ptr<IndexerJob> &job, const std::shared_ptr<IndexDataMessage> &msg);
    Sources sources() const { return mSources; }
    String toCompilationDatabase() const;
    enum WatchMode {
        Watch_FileManager = 0x1,
        Watch_SourceFile = 0x2,
        Watch_Dependency = 0x4,
        Watch_CompilationDatabase = 0x8
    };

    void watch(const Path &dir, WatchMode mode);
    void unwatch(const Path &dir, WatchMode mode);
    void clearWatch(Flags<WatchMode> mode);
    Hash<Path, Flags<WatchMode> > watchedPaths() const { return mWatchedPaths; }

    bool isIndexing() const { return !mActiveJobs.isEmpty(); }
    void onFileAdded(const Path &path);
    void onFileModified(const Path &path);
    void onFileRemoved(const Path &path);
    void dumpFileMaps(const std::shared_ptr<QueryMessage> &msg, const std::shared_ptr<Connection> &conn);
    Hash<uint32_t, Path> visitedFiles() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mVisitedFiles;
    }
    void encodeVisitedFiles(Serializer &serializer)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        serializer << mVisitedFiles;
    }

    void beginScope();
    void endScope();
    void dirty(uint32_t fileId);
    bool save();
    void prepare(uint32_t fileId);
    String estimateMemory() const;
    void diagnose(uint32_t fileId);
    void diagnoseAll();
    uint32_t fileMapOptions() const;
    void fixPCH(Source &source);
private:
    void reloadCompilationDatabase();
    void removeSource(Sources::iterator it);
    void onFileAddedOrModified(const Path &path);
    void watchFile(uint32_t fileId);
    enum ValidateMode {
        StatOnly,
        Validate
    };
    bool validate(uint32_t fileId, ValidateMode mode, String *error = 0) const;
    void removeDependencies(uint32_t fileId);
    void updateDependencies(const std::shared_ptr<IndexDataMessage> &msg);
    void loadFailed(uint32_t fileId);
    void updateFixIts(const Set<uint32_t> &visited, FixIts &fixIts);
    Diagnostics updateDiagnostics(const Diagnostics &diagnostics);
    int startDirtyJobs(Dirty *dirty,
                       IndexerJob::Flag type,
                       const UnsavedFiles &unsavedFiles = UnsavedFiles(),
                       const std::shared_ptr<Connection> &wait = std::shared_ptr<Connection>());
    void onDirtyTimeout(Timer *);

    struct FileMapScope {
        FileMapScope(const std::shared_ptr<Project> &proj, int m)
            : project(proj), openedFiles(0), totalOpened(0), max(m)
        {}
        ~FileMapScope()
        {
            warning() << "Query opened" << totalOpened << "files for project" << project->path();
        }

        struct LRUKey {
            FileMapType type;
            uint32_t fileId;
            bool operator<(const LRUKey &other) const
            {
                return fileId < other.fileId || (fileId == other.fileId && type < other.type);
            }
        };
        struct LRUEntry {
            LRUEntry(FileMapType t, uint32_t f)
                : key({ t, f })
            {}
            const LRUKey key;

            std::shared_ptr<LRUEntry> next, prev;
        };

        void poke(FileMapType t, uint32_t f)
        {
            const LRUKey key = { t, f };
            auto ptr = entryMap.value(key);
            assert(ptr);
            entryList.remove(ptr);
            entryList.append(ptr);
        }

        template <typename Key, typename Value>
        std::shared_ptr<FileMap<Key, Value> > openFileMap(FileMapType type, uint32_t fileId,
                                                          Hash<uint32_t, std::shared_ptr<FileMap<Key, Value> > > &cache,
                                                          String *errPtr)
        {
            auto it = cache.find(fileId);
            if (it != cache.end()) {
                poke(type, fileId);
                return it->second;
            }
            const Path path = project->sourceFilePath(fileId, Project::fileMapName(type));
            std::shared_ptr<FileMap<Key, Value> > fileMap(new FileMap<Key, Value>);
            String err;
            if (fileMap->load(path, project->fileMapOptions(), &err)) {
                ++totalOpened;
                cache[fileId] = fileMap;
                std::shared_ptr<LRUEntry> entry(new LRUEntry(type, fileId));
                entryList.append(entry);
                entryMap[entry->key] = entry;
                if (++openedFiles > max) {
                    const std::shared_ptr<LRUEntry> e = entryList.takeFirst();
                    assert(e);
                    entryMap.remove(e->key);
                    switch (e->key.type) {
                    case SymbolNames:
                        assert(symbolNames.contains(e->key.fileId));
                        symbolNames.remove(e->key.fileId);
                        break;
                    case Symbols:
                        assert(symbols.contains(e->key.fileId));
                        symbols.remove(e->key.fileId);
                        break;
                    case Targets:
                        assert(targets.contains(e->key.fileId));
                        targets.remove(e->key.fileId);
                        break;
                    case Usrs:
                        assert(usrs.contains(e->key.fileId));
                        usrs.remove(e->key.fileId);
                        break;
                    }
                    --openedFiles;
                }
                assert(openedFiles <= max);
            } else {
                if (errPtr) {
                    *errPtr = "Failed to open: " + path + " " + Location::path(fileId) + ": " + err;
                } else {
                    error() << "Failed to open" << path << Location::path(fileId) << err;
                }
                project->loadFailed(fileId);
                fileMap.reset();
            }
            return fileMap;
        }

        Hash<uint32_t, std::shared_ptr<FileMap<String, Set<Location> > > > symbolNames;
        Hash<uint32_t, std::shared_ptr<FileMap<Location, Symbol> > > symbols;
        Hash<uint32_t, std::shared_ptr<FileMap<String, Set<Location> > > > targets, usrs;
        std::shared_ptr<Project> project;
        int openedFiles, totalOpened;
        const int max;

        EmbeddedLinkedList<std::shared_ptr<LRUEntry> > entryList;
        Map<LRUKey, std::shared_ptr<LRUEntry> > entryMap;
    };

    std::shared_ptr<FileMapScope> mFileMapScope;

    const Path mPath, mSourceFilePathBase;
    struct CompilationDataBaseInfo {
        Path dir;
        uint64_t lastModified;
        List<Path> pathEnvironment;
        Flags<IndexMessage::Flag> indexFlags;
    } mCompilationDatabaseInfo;
    Path mProjectFilePath, mSourcesFilePath;

    Files mFiles;

    Hash<uint32_t, Path> mVisitedFiles;
    int mJobCounter, mJobsStarted;

    Diagnostics mDiagnostics;

    // key'ed on Source::key()
    Hash<uint64_t, std::shared_ptr<IndexerJob> > mActiveJobs;

    Timer mDirtyTimer;
    Set<uint32_t> mPendingDirtyFiles;

    StopWatch mTimer;
    FileSystemWatcher mWatcher;
    Sources mSources;
    Set<uint64_t> *mMarkSources;
    Hash<Path, Flags<WatchMode> > mWatchedPaths;
    std::shared_ptr<FileManager> mFileManager;
    FixIts mFixIts;

    Hash<uint32_t, DependencyNode*> mDependencies;
    Set<uint32_t> mSuspendedFiles;

    mutable std::mutex mMutex;
};

RCT_FLAGS(Project::WatchMode);

inline bool Project::visitFile(uint32_t visitFileId, const Path &path, uint64_t key)
{
    std::lock_guard<std::mutex> lock(mMutex);
    assert(visitFileId);
    Path &p = mVisitedFiles[visitFileId];
    if (p.isEmpty()) {
        p = path;
        if (key) {
            assert(mActiveJobs.contains(key));
            std::shared_ptr<IndexerJob> &job = mActiveJobs[key];
            assert(job);
            job->visited.insert(visitFileId);
        }
        return true;
    }
    return false;
}

inline void Project::releaseFileIds(const Set<uint32_t> &fileIds)
{
    if (!fileIds.isEmpty()) {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const auto &f : fileIds) {
            // error() << "Returning files" << Location::path(f);
            mVisitedFiles.remove(f);
        }
    }
}

inline Path Project::sourceFilePath(uint32_t fileId, const char *type) const
{
    return String::format<1024>("%s%d/%s", mSourceFilePathBase.constData(), fileId, type);
}

#endif
