//===--- Background.h - Build an index in a background thread ----*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_BACKGROUND_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_BACKGROUND_H

#include "Context.h"
#include "FSProvider.h"
#include "GlobalCompilationDatabase.h"
#include "SourceCode.h"
#include "Threading.h"
#include "index/BackgroundRebuild.h"
#include "index/FileIndex.h"
#include "index/Index.h"
#include "index/Serialization.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Threading.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace clang {
namespace clangd {

// Handles storage and retrieval of index shards. Both store and load
// operations can be called from multiple-threads concurrently.
class BackgroundIndexStorage {
public:
  virtual ~BackgroundIndexStorage() = default;

  // Shards of the index are stored and retrieved independently, keyed by shard
  // identifier - in practice this is a source file name
  virtual llvm::Error storeShard(llvm::StringRef ShardIdentifier,
                                 IndexFileOut Shard) const = 0;

  // Tries to load shard with given identifier, returns nullptr if shard
  // couldn't be loaded.
  virtual std::unique_ptr<IndexFileIn>
  loadShard(llvm::StringRef ShardIdentifier) const = 0;

  // The factory provides storage for each CDB.
  // It keeps ownership of the storage instances, and should manage caching
  // itself. Factory must be threadsafe and never returns nullptr.
  using Factory =
      llvm::unique_function<BackgroundIndexStorage *(llvm::StringRef)>;

  // Creates an Index Storage that saves shards into disk. Index storage uses
  // CDBDirectory + ".clangd/index/" as the folder to save shards.
  static Factory createDiskBackedStorageFactory();
};

// A priority queue of tasks which can be run on (external) worker threads.
class BackgroundQueue {
public:
  /// A work item on the thread pool's queue.
  struct Task {
    template <typename Func>
    explicit Task(Func &&F) : Run(std::forward<Func>(F)){};

    std::function<void()> Run;
    llvm::ThreadPriority ThreadPri = llvm::ThreadPriority::Background;
    // Higher-priority tasks will run first.
    unsigned QueuePri = 0;

    bool operator<(const Task &O) const { return QueuePri < O.QueuePri; }
  };

  // Add tasks to the queue.
  void push(Task);
  void append(std::vector<Task>);

  // Process items on the queue until the queue is stopped.
  // If the queue becomes empty, OnIdle will be called (on one worker).
  void work(std::function<void()> OnIdle = nullptr);

  // Stop processing new tasks, allowing all work() calls to return soon.
  void stop();

  // Disables thread priority lowering to ensure progress on loaded systems.
  // Only affects tasks that run after the call.
  static void preventThreadStarvationInTests();
  LLVM_NODISCARD bool
  blockUntilIdleForTest(llvm::Optional<double> TimeoutSeconds);

private:
  std::mutex Mu;
  unsigned NumActiveTasks = 0; // Only idle when queue is empty *and* no tasks.
  std::condition_variable CV;
  bool ShouldStop = false;
  std::vector<Task> Queue; // max-heap
};

// Builds an in-memory index by by running the static indexer action over
// all commands in a compilation database. Indexing happens in the background.
// FIXME: it should also persist its state on disk for fast start.
// FIXME: it should watch for changes to files on disk.
class BackgroundIndex : public SwapIndex {
public:
  /// If BuildIndexPeriodMs is greater than 0, the symbol index will only be
  /// rebuilt periodically (one per \p BuildIndexPeriodMs); otherwise, index is
  /// rebuilt for each indexed file.
  BackgroundIndex(
      Context BackgroundContext, const FileSystemProvider &,
      const GlobalCompilationDatabase &CDB,
      BackgroundIndexStorage::Factory IndexStorageFactory,
      size_t ThreadPoolSize = llvm::heavyweight_hardware_concurrency());
  ~BackgroundIndex(); // Blocks while the current task finishes.

  // Enqueue translation units for indexing.
  // The indexing happens in a background thread, so the symbols will be
  // available sometime later.
  void enqueue(const std::vector<std::string> &ChangedFiles) {
    Queue.push(changedFilesTask(ChangedFiles));
  }

  // Cause background threads to stop after ther current task, any remaining
  // tasks will be discarded.
  void stop() {
    Rebuilder.shutdown();
    Queue.stop();
  }

  // Wait until the queue is empty, to allow deterministic testing.
  LLVM_NODISCARD bool
  blockUntilIdleForTest(llvm::Optional<double> TimeoutSeconds = 10) {
    return Queue.blockUntilIdleForTest(TimeoutSeconds);
  }

private:
  /// Represents the state of a single file when indexing was performed.
  struct ShardVersion {
    FileDigest Digest{{0}};
    bool HadErrors = false;
  };

  /// Given index results from a TU, only update symbols coming from files with
  /// different digests than \p ShardVersionsSnapshot. Also stores new index
  /// information on IndexStorage.
  void update(llvm::StringRef MainFile, IndexFileIn Index,
              const llvm::StringMap<ShardVersion> &ShardVersionsSnapshot,
              BackgroundIndexStorage *IndexStorage, bool HadErrors);

  // configuration
  const FileSystemProvider &FSProvider;
  const GlobalCompilationDatabase &CDB;
  Context BackgroundContext;

  llvm::Error index(tooling::CompileCommand,
                    BackgroundIndexStorage *IndexStorage);

  FileSymbols IndexedSymbols;
  BackgroundIndexRebuilder Rebuilder;
  llvm::StringMap<ShardVersion> ShardVersions; // Key is absolute file path.
  std::mutex ShardVersionsMu;

  BackgroundIndexStorage::Factory IndexStorageFactory;
  struct Source {
    std::string Path;
    bool NeedsReIndexing;
    Source(llvm::StringRef Path, bool NeedsReIndexing)
        : Path(Path), NeedsReIndexing(NeedsReIndexing) {}
  };
  // Loads the shards for a single TU and all of its dependencies. Returns the
  // list of sources and whether they need to be re-indexed.
  std::vector<Source> loadShard(const tooling::CompileCommand &Cmd,
                                BackgroundIndexStorage *IndexStorage,
                                llvm::StringSet<> &LoadedShards);
  // Tries to load shards for the ChangedFiles.
  std::vector<std::pair<tooling::CompileCommand, BackgroundIndexStorage *>>
  loadShards(std::vector<std::string> ChangedFiles);

  BackgroundQueue::Task
  changedFilesTask(const std::vector<std::string> &ChangedFiles);
  BackgroundQueue::Task indexFileTask(tooling::CompileCommand Cmd,
                                      BackgroundIndexStorage *Storage);

  // from lowest to highest priority
  enum QueuePriority {
    IndexFile,
    LoadShards,
  };
  BackgroundQueue Queue;
  AsyncTaskRunner ThreadPool;
  GlobalCompilationDatabase::CommandChanged::Subscription CommandsChanged;
};

} // namespace clangd
} // namespace clang

#endif
