#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "zarchivecommon.h"

class ZArchiveWriter
{
	struct PathNode
	{
		PathNode() : isFile(false), nameIndex(0xFFFFFFFF) {};
		PathNode(bool isFile, uint32_t nameIndex) : isFile(isFile), nameIndex(nameIndex) {};
		~PathNode() { for (PathNode* n : subnodes) delete n; }

		bool isFile;
		uint32_t nameIndex; // index in m_nodeNames

		std::vector<PathNode*> subnodes;

		// file properties
		uint64_t fileOffset{};
		uint64_t fileSize{};
		// directory properties
		uint32_t nodeStartIndex{};
	};

public:
	typedef void(*CB_NewOutputFile)(const int32_t partIndex, void* ctx);
	typedef void(*CB_WriteOutputData)(const void* data, size_t length, void* ctx);

	// What happened to the 64KiB blocks written so far (blocks still waiting in the compression
	// batch are not counted until they are written).
	struct Stats
	{
		uint64_t blocks{};           // blocks written
		uint64_t storedBlocks{};     // stored uncompressed (any reason)
		uint64_t precheckSkipped{};  // stored uncompressed because the fast pre-check couldn't shrink them enough
		uint64_t storeOnlyBlocks{};  // stored uncompressed because all their data came from store-only files
		uint64_t inputBytes{};       // bytes appended (file data, without the final padding)
		uint64_t outputBytes{};      // bytes written to the output so far
	};

	ZArchiveWriter(CB_NewOutputFile cbNewOutputFile, CB_WriteOutputData cbWriteOutputData, void* ctx);
	~ZArchiveWriter();

	// ---- Compression settings. Call before the first AppendData. Archives written with any of them
	// are read by every ZArchive reader: only the writer decides how each block is stored.

	// zstd level for each block: ZSTD_minCLevel() .. ZSTD_maxCLevel() (22). Default 6, like upstream.
	void SetCompressionLevel(int level);
	int GetCompressionLevel() const { return m_compressionLevel; }
	// 0 = off (default). Otherwise every block is first compressed at zstd's fastest level, and
	// blocks that don't get below this percentage of the block size are stored uncompressed
	// without trying the (slow) configured level. Already-compressed data (compressed disc images,
	// video, archives) then costs almost no time, and reads back without decompression.
	void SetPrecheck(double percent);
	// Threads compressing blocks: 0 = all cores, 1 = on the calling thread (default). The output is
	// identical for any thread count.
	void SetThreads(unsigned threads);

	bool StartNewFile(const char* path); // creates a new virtual file and makes it active
	// Same, but compress = false stores the file's data without compressing it (blocks it shares
	// with compressible neighbours are still compressed).
	bool StartNewFile(const char* path, bool compress);
	void AppendData(const void* data, size_t size); // appends data to currently active file
	// StartNewFile + AppendData for a file already in memory.
	bool AddFile(const char* path, const void* data, size_t size, bool compress = true);
	bool MakeDir(const char* path, bool recursive = false);
	void Finalize();

	Stats GetStats() const;

private:
	PathNode* GetNodeByPath(PathNode* root, std::string_view path);
	PathNode* FindSubnodeByName(PathNode* parent, std::string_view nodeName);

	uint32_t CreateNameEntry(std::string_view name);

	void OutputData(const void* data, size_t length);
	uint64_t GetCurrentOutputOffset() const;

	// A full block of input, compressed now (one thread) or batched (several).
	void QueueBlock(const uint8_t* uncompressedData, bool compress);
	void FlushBatch();
	void WriteBlock(const uint8_t* data, size_t size);

	void WriteOffsetRecords();
	void WriteNameTable();
	void WriteFileTree();
	void WriteMetaData();
	void WriteFooter();

	// ---- block compression
	struct BlockJob
	{
		std::vector<uint8_t> input;   // COMPRESSED_BLOCK_SIZE bytes
		std::vector<uint8_t> output;  // compressed data (unused when stored)
		size_t outputSize{};
		bool compress{};
		bool stored{};
		bool precheckSkipped{};
	};
	struct Compressor;  // a zstd context
	void CompressBlock(Compressor& c, BlockJob& job) const;
	void StartWorkers();
	void StopWorkers();
	void WorkerMain();
	void RunBatchJobs();

private:
	// callbacks
	CB_NewOutputFile m_cbNewOutputFile;
	CB_WriteOutputData m_cbWriteOutputData;
	void* m_cbCtx;
	// file tree
	PathNode m_rootNode;
	PathNode* m_currentFileNode{ nullptr };
	bool m_currentFileCompress{ true };
	std::vector<std::string> m_nodeNames;
	std::vector<uint32_t> m_nodeNameOffsets;
	std::unordered_map<std::string, uint32_t> m_nodeNameLookup;
	// footer
	_ZARCHIVE::Footer m_footer;
	// writes and compression
	std::vector<uint8_t> m_currentWriteBuffer;
	bool m_currentBlockCompress{ false }; // the partial block holds data from a compressible file
	uint64_t m_currentCompressedWriteIndex{ 0 }; // output file write index
	uint64_t m_currentInputOffset{ 0 }; // current offset within uncompressed file data
	// settings
	int m_compressionLevel{ 6 };
	double m_precheckPercent{ 0.0 };
	unsigned m_threads{ 1 };
	bool m_started{ false };
	// single-thread path
	std::unique_ptr<Compressor> m_mainCompressor;
	BlockJob m_singleJob;
	// batched path
	std::vector<BlockJob> m_batch;
	size_t m_batchUsed{ 0 };
	std::vector<std::thread> m_workers;
	std::mutex m_workMutex;
	std::condition_variable m_workCv, m_doneCv;
	uint64_t m_workGeneration{ 0 };
	std::atomic<size_t> m_nextJob{ 0 };
	std::atomic<size_t> m_runCount{ 0 }; // jobs in the batch being compressed
	size_t m_jobsDone{ 0 };
	unsigned m_workersBusy{ 0 };
	bool m_stopWorkers{ false };
	// uncompressed-to-compressed offset records
	uint64_t m_numWrittenOffsetRecords{ 0 };
	std::vector<_ZARCHIVE::CompressionOffsetRecord> m_compressionOffsetRecord;
	// stats
	Stats m_stats;
	// hashing
	struct Sha_256* m_mainShaCtx{};
	uint8_t m_integritySha[32];
};
